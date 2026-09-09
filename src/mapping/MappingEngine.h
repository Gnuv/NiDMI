#pragma once
#include <math.h>   // NAN
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
        uint32_t metroProchain = 0;      // date du prochain bang
        bool     metroArme     = false;  // faux tant que le premier tick n'a pas planifie
        uint32_t debounceDernier = 0;    // date du dernier passage de debounce()
        /* La position COURANTE du glissando de lag(). Elle survit au glissando
         * lui-meme — c'est d'elle que repart le suivant. Les parametres du
         * glissando en cours (depart, cible, duree) vivent dans la reprise :
         * un glissando est une tache qui passe, pas une propriete du pipeline,
         * et 256 etats n'ont pas a payer pour seize taches. */
        float    lagCur = NAN;           // NAN = jamais glisse
        int8_t   mnNote = -1;            // note tenue par makenote() en bascule
                                         // (-1 = aucune)
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
    // `brut` : la lecture du capteur dans sa RESOLUTION NATIVE, celle que lit
    // raw.in(). NAN (le defaut) signifie « rien de plus fin que `valeur` » —
    // le cas d'un contact, qui n'a que 0/1.
    static void executerCapteur(const char* script, float valeur,
                                MidiSender* midi_sender,
                                Etat* etats = nullptr, int nEtats = 0,
                                float brut = NAN);



    // ── Traitement d'un EVENEMENT MIDI ENTRANT ──────────────────────────────
    // execute() ci-dessus prend une seule valeur flottante : c'est le modele des
    // CAPTEURS (un potentiometre, un bouton). Un message MIDI porte plusieurs
    // champs, et un script peut en EMETTRE PLUSIEURS — deux pipelines qui
    // repondent au meme evenement, un sel() qui eclate un accord. La sortie est
    // donc une LISTE, comme dans le moteur web (outEvents) : n'en garder qu'une
    // seule etait une simplification qui se voyait des le premier script a deux
    // pipelines.
    struct Evenement {
        /* `Tick` : le battement d'horloge. C'est lui qui fait tourner les
         * pipelines dont la source n'attend aucun MIDI — metro(), loadbang().
         * Sans lui, un script sans note.in() ne s'executait jamais sur la
         * carte, et deux scripts ne pouvaient pas se parler par le registre
         * (MESURES.md §57). */
        enum Type { Aucun, NoteOn, NoteOff, Cc, Bend, Touch, PolyTouch, Pgm,
                    Tick,    // battement d'horloge : fait tourner metro()
                    Init };  // une fois apres un chargement : fait tourner loadbang()
        Type    type     = Aucun;
        uint8_t canal    = 0;    // 1..16 (0 = inconnu)
        uint8_t a        = 0;    // note | numero de CC | note (polytouch) | programme
        uint8_t b        = 0;    // velocite | valeur de CC | pression
        int16_t valeur14 = 0;    // pitch bend, -8192..8191
        uint32_t instant = 0;    // millis() — n'a de sens que pour Tick
    };

    /* Longueur d'une adresse OSC portee par une sortie. Choisie sur MESURE :
     * la tache MIDI garde 6204 octets de pile libres, et un tableau de seize
     * sorties elargi lui en coute environ 750. « /composition/piste/3/volume »
     * tient dans 40. Au-dela, le message est REFUSE — jamais tronque, sans quoi
     * il partirait vers une autre adresse que celle qu'on a ecrite. */
    static const int MAX_ADRESSE_OSC = 40;
    static const int MAX_ARGS_OSC_SUP = 3;   // en plus de `reel`, la valeur courante

    struct Sortie {
        enum Type { Note, NoteOff, Cc, Bend, Touch, PolyTouch, Pgm, Print, Osc };
        Type    type     = Note;
        uint8_t canal    = 1;
        uint8_t a        = 0;    // note | numero de CC | note
        uint8_t b        = 0;    // velocite | valeur
        int16_t valeur14 = 0;    // pitch bend
        float   reel     = 0;    // print() et osc : la valeur telle quelle
        /* osc.out : l'adresse, les arguments FROIDS qui suivent la valeur
         * courante, et l'octet d'hote optionnel (0 = la cible configuree). */
        char    adresse[MAX_ADRESSE_OSC] = {0};
        float   argsSup[MAX_ARGS_OSC_SUP] = {0, 0, 0};
        uint8_t nArgsSup = 0;
        uint8_t hote     = 0;
    };

    // Seize : deux pipelines qui emettent chacun un accord de quatre notes, et
    // il reste de la marge. Au-dela on ecrete plutot que de deborder — un
    // script qui emet seize evenements sur une seule note entrante est une
    // erreur d'ecriture, pas un cas a servir.
    static const int MAX_SORTIES = 16;
    /* Pipelines qu'un script map peut porter avec un etat PROPRE. C'etait une
     * constante privee du moteur (le tableau global) ; elle devient publique
     * parce que chaque emplacement de script doit dimensionner le sien. */
    static const int MAX_PIPELINES_SCRIPT = 12;


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
    // horsLigne : cet appel SIMULE, il ne pilote pas la carte. Ses verbes de
    //              temps tiennent leur propre file et leur propre horloge, que
    //              la boucle MIDI ne vide jamais. Sans ca, la route d'essai
    //              deposait ses differes dans la file VIVANTE : ils partaient
    //              en MIDI reel et ne revenaient jamais a l'essai.
    static int executer(const char* script, const Evenement& evt,
                        Sortie* sorties, int max, bool& traite,
                        Etat* etats = nullptr, int nEtats = 0,
                        bool horsLigne = false);

    /* Le BATTEMENT D'HORLOGE : fait tourner les pipelines qui n'attendent aucun
     * MIDI. `Tick` (avec l'instant en millis) tire metro() ; `Init`, envoye une
     * fois apres un chargement, tire loadbang(). C'est la reponse du langage a
     * « que se passe-t-il quand il n'y a pas de note.in() ? » — heritage assume
     * d'un DSL passe de script MIDI a script generaliste (DMX, OSC...). */
    static void battre(const char* script, Evenement::Type type, uint32_t instant,
                       MidiSender* sender, Etat* etats = nullptr, int nEtats = 0);

    /* Les VERBES DE TEMPS — del(), et bientot makenote(), lag(), ramp() — ne
     * rendent pas leur valeur tout de suite : ils la mettent de cote et l'aval
     * du pipeline est rejoue plus tard. C'est ici qu'on vide cette file, une
     * fois par battement, pour TOUS les scripts a la fois.
     * `viderDifferes(script)` oublie les reprises d'un script donne — a appeler
     * quand son texte est libere, sinon on rejouerait sur de la memoire rendue.
     * Sans argument : tout oublier. */
    static void battreDifferes(uint32_t maintenant, MidiSender* sender);
    /* Variante qui REMPLIT une liste au lieu d'emettre : le banc de conformite
     * n'a pas de MidiSender, et c'est lui qui prouve ces verbes. */
    static int  battreDifferes(uint32_t maintenant, Sortie* sorties, int max,
                               bool horsLigne = false);
    static void viderDifferes(const char* script = nullptr);

    // Efface l'etat par pipeline (toggle, counter, seq, sel/map, lp, drunk...).
    // A appeler quand le SCRIPT change : sinon un compteur repart d'ou en etait
    // le script precedent, a une position qui n'a plus de sens.
    static void reinitialiser();

    // ── print() ─────────────────────────────────────────────────────────────
    // Le pipeline ne connait ni Serial ni WebSocket : c'est du calcul pur, et
    // c'est ce qui le rend eprouvable hors carte. print() delegue donc a un
    // rappel que l'appelant installe. Sur la carte il ecrit au journal et
    // pousse une trame vers l'app ; au banc, il n'y a rien a installer.
    typedef void (*Impression)(const char* etiquette, float valeur);
    static void surImpression(Impression fn);

    /* L'EMETTEUR OSC, pose de l'exterieur — comme l'impression. Le moteur ne
     * connait aucun transport : il recoit un MidiSender pour le MIDI et cette
     * fonction pour l'OSC. Non posee (le banc de conformite), rien ne part :
     * une epreuve ne doit pas arroser le reseau de l'usager.
     * `hote` : 0 = la cible configuree ; sinon le DERNIER OCTET a substituer
     * dans cette cible — la lecture embarquee de osc.out(N, "/x"), qui vaut
     * 127.0.0.N sur un poste de travail. */
    typedef void (*EmetteurOsc)(const char* adresse, const float* args, int n,
                                uint8_t hote);
    static void surOsc(EmetteurOsc fn);

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
    /* `etats` : l'etat par pipeline de CE script. Sans lui, on retombe sur le
     * tableau GLOBAL — ce qui allait tant qu'il n'y avait qu'un script map, et
     * qui ferait se marcher dessus les emplacements multiples (meme piege que
     * le §59 pour les broches). */
    static bool executeMidiNote(const char* script,
                                uint8_t noteIn, uint8_t veloIn, uint8_t canalIn,
                                bool estNoteOff, SortieNote& sortie,
                                Etat* etats = nullptr, int nEtats = 0);

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
                              SortieCc& sortie, Etat* etats = nullptr, int nEtats = 0);
};