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
    // jouer la note brute. false = script muet ou absent, passage tel quel.
    struct SortieNote {
        bool   emise   = false;
        uint8_t note   = 0;
        uint8_t velo   = 0;
        uint8_t canal  = 1;
    };
    static bool executeMidiNote(const char* script,
                                uint8_t noteIn, uint8_t veloIn, uint8_t canalIn,
                                bool estNoteOff, SortieNote& sortie);
};