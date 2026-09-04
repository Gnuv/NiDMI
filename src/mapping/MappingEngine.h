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
    // Utilisation de const char* pour être plus léger que String
    static void execute(const char* script, float inputVal, MidiSender* midi_sender = nullptr);

    // ── Traitement d'un EVENEMENT MIDI ENTRANT ──────────────────────────────
    // execute() ci-dessus prend une seule valeur flottante : c'est le modele des
    // CAPTEURS (un potentiometre, un bouton). Un message MIDI porte trois
    // champs, et le script doit pouvoir lire la note ET la republier.
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