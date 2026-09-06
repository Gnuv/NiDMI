#pragma once
#include <Arduino.h>

// Forward declaration
class MidiSender;

// 1. Le Registre : Stocke les valeurs de chaque composant par nom
class FluxRegistry {
public:
    struct Entry { 
        char name[16]; 
        float value; 
    };
    
    static Entry entries[32]; 
    static int count;

    static void update(const char* name, float val);
    static float get(const char* name);
    static bool has(const char* name);
};

// 2. Le Moteur : Découpe et exécute le script segment par segment
class MappingEngine {
public:
    // Etat d'UN pipeline, d'un evenement au suivant : toggle, counter, seq,
    // l'index retenu par sel pour map, lp, drunk, hysteresis, change.
    //
    // Il est PUBLIC et fourni par l'appelant parce qu'il n'y a pas un seul
    // script sur la carte : le script MIDI d'un cote, celui de chaque composant
    // de l'autre. Un tableau interne unique les ferait se marcher dessus — le
    // compteur d'un bouton avancerait quand une note arrive.
    struct Etat {
        int16_t selIdx   = -1;
        int8_t  toggle   = 0;
        int8_t  hyst     = 0;
        int16_t seq      = -1;
        float   compteur = NAN;    // NAN = jamais initialise
        float   drunk    = NAN;
        float   lp       = NAN;
        float   change   = NAN;
        void reinitialiser() { *this = Etat(); }
    };

    // ── Capteurs ────────────────────────────────────────────────────────────
    // Un potentiometre, un bouton, un joystick. Le script n'y est PAS declenche
    // par un message MIDI : on execute avec un evenement SANS famille, si bien
    // que seules les sources qui repondent a tout — r("nom"), f(x), i(x), un
    // litteral — tirent. C'est exactement le modele du moteur web, ou un
    // capteur publie dans le registre et le script le lit.
    //
    // La valeur est publiee sous le nom du composant ET sous « in », pour qu'un
    // composant sans nom reste scriptable :  r("in") : *(2) : ctl.out(1,74) ;
    //
    // Ici, contrairement au MIDI entrant, un verbe .out EMET vraiment : un
    // capteur produit du MIDI, il n'en transforme pas.
    static void executerCapteur(const char* script, float valeur,
                                MidiSender* midi_sender, Etat* etat = nullptr);

    // ── Traitement d'un EVENEMENT MIDI ENTRANT ──────────────────────────────
    // execute() ci-dessus prend une seule valeur flottante : c'est le modele des
    // CAPTEURS (un potentiometre, un bouton). Un message MIDI porte plusieurs
    // champs, et un script peut en EMETTRE PLUSIEURS — deux pipelines qui
    // repondent au meme evenement, un sel() qui eclate un accord. La sortie est
    // donc une LISTE, comme dans le moteur web (outEvents) : n'en garder qu'une
    // seule etait une simplification qui se voyait des le premier script a deux
    // pipelines.
    struct Evenement {
        enum Type { Aucun, NoteOn, NoteOff, Cc, Bend, Touch, PolyTouch, Pgm };
        Type    type     = Aucun;
        uint8_t canal    = 0;    // 1..16 (0 = inconnu)
        uint8_t a        = 0;    // note | numero de CC | note (polytouch) | programme
        uint8_t b        = 0;    // velocite | valeur de CC | pression
        int16_t valeur14 = 0;    // pitch bend, -8192..8191
    };

    struct Sortie {
        enum Type { Note, NoteOff, Cc, Bend, Touch, PolyTouch, Pgm, Print };
        Type    type     = Note;
        uint8_t canal    = 1;
        uint8_t a        = 0;    // note | numero de CC | note
        uint8_t b        = 0;    // velocite | valeur
        int16_t valeur14 = 0;    // pitch bend
        float   reel     = 0;    // print() : la valeur telle quelle
    };

    // Seize : deux pipelines qui emettent chacun un accord de quatre notes, et
    // il reste de la marge. Au-dela on ecrete plutot que de deborder — un
    // script qui emet seize evenements sur une seule note entrante est une
    // erreur d'ecriture, pas un cas a servir.
    static const int MAX_SORTIES = 16;


    // Execute le script sur un evenement.
    //   sorties/max : la liste a remplir ; renvoie le NOMBRE d'evenements emis.
    //   traite      : vrai si au moins un pipeline dont la SOURCE correspond au
    //                 genre de l'evenement s'est DECLENCHE. C'est ce qui permet
    //                 a l'appelant de distinguer « le script a pris cet
    //                 evenement en charge et a choisi de le taire » de « le
    //                 script ne parle pas de ce genre-la, laisse-le passer ».
    //                 Meme regle que le moteur web (noteHandled/ccHandled) : un
    //                 filtre qui ne correspond PAS (ctl.in(5) sur le canal 1) ne
    //                 prend rien en charge, l'evenement passe.
    // etats/nEtats : l'etat par pipeline. nullptr = le tableau interne, celui du
    // script MIDI. Un composant passe le sien.
    static int executer(const char* script, const Evenement& evt,
                        Sortie* sorties, int max, bool& traite,
                        Etat* etats = nullptr, int nEtats = 0);

    // Efface l'etat par pipeline (toggle, counter, seq, sel/map, lp, drunk...).
    // A appeler quand le SCRIPT change : sinon un compteur repart d'ou en etait
    // le script precedent, a une position qui n'a plus de sens.
    static void reinitialiser();

    // ── Enveloppes historiques ──────────────────────────────────────────────
    // executeMidiNote / executeMidiCc restent l'interface de MidiRouter : elles
    // appellent executer() et n'en gardent que le PREMIER evenement de leur
    // genre. Les conserver evite de reecrire les appelants dans le meme
    // mouvement que le moteur.
    //
    // Le pipeline reste le meme (segments separes par ':'), avec deux mots de
    // plus, ceux qu'emploient les .nms de mapping :
    //   note.in()      la valeur courante devient la NOTE entrante
    //   note.out(ch)   emet la valeur courante COMME note, sur le canal ch,
    //                  en gardant la velocite d'entree
    // (La forme a deux arguments note.out(note, ch) reste celle des capteurs :
    //  la note y est litterale et la valeur courante sert de velocite.)
    //
    // Retour : true si le script a emis une note — l'appelant ne doit alors PAS
    // jouer la note brute.
    //
    // `traite` distingue deux silences, et la distinction n'est pas theorique :
    //   - le script PARLE de notes (un note.in ou un note.out) mais n'a rien
    //     emis pour celle-ci : c'est un FILTRE, la note est bloquee ;
    //   - le script ne parle pas de notes du tout (il ne mappe que des CC) :
    //     la note passe telle quelle. Sans cette distinction, charger un
    //     script de CC rendait le clavier muet.
    // Meme regle que pour les CC, dans l'autre sens.
    struct SortieNote {
        bool   emise   = false;
        bool   traite  = false;
        uint8_t note   = 0;
        uint8_t velo   = 0;
        uint8_t canal  = 1;
    };
    static bool executeMidiNote(const char* script,
                                uint8_t noteIn, uint8_t veloIn, uint8_t canalIn,
                                bool estNoteOff, SortieNote& sortie);

    // ── Traitement d'un CONTROLEUR CONTINU entrant ──────────────────────────
    // Pendant du precedent pour les CC. Les verbes sont ceux que la langue
    // .nms definit deja et que sert le moteur web — on n'en invente aucun :
    //   ctl.in()          la valeur courante devient la VALEUR du CC (0..127)
    //   ctl.in(ch)        ... et le pipeline ne s'execute que sur ce canal
    //   ctl.in(ch,cc)     ... et seulement pour ce numero de CC
    //   ctl.out()         emet : numero et canal d'ORIGINE, valeur courante
    //   ctl.out(ch)       ... sur le canal donne
    //   ctl.out(ch,cc)    ... sur le canal et le numero donnes
    // (ccnum.in / ctlchan.in restent a faire.)
    //
    // ASYMETRIE VOULUE avec les notes. Pour une note, « script present mais
    // muet » BLOQUE la note. Appliquer la meme regle aux CC rendrait muet tout
    // controleur des qu'un transpose.nms est charge — il ne parle que de notes.
    // On distingue donc deux choses :
    //   emise  : un ctl.out() a produit un CC — l'appelant prend celui-la ;
    //   traite : le script DECLARE s'occuper des CC (il contient un ctl.in) —
    //            s'il n'a rien emis, il a choisi de taire ce CC.
    // Script sans aucun ctl.in : les CC passent tels quels, comme avant.
    //
    // Comme note.out, ctl.out n'emet PAS vers les sorties MIDI : il dit ce que
    // le CC DEVIENT pour la carte (table CcMap, composants). ccEntrant est un
    // chemin d'entree.
    struct SortieCc {
        bool    emise  = false;
        bool    traite = false;
        uint8_t cc     = 0;
        uint8_t valeur = 0;
        uint8_t canal  = 1;
    };
    static bool executeMidiCc(const char* script,
                              uint8_t ccIn, uint8_t valeurIn, uint8_t canalIn,
                              SortieCc& sortie);
};