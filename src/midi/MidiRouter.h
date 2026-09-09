// Routeur vers implémentations RTP-MIDI et OSC existantes
#pragma once

#include <Arduino.h>
#include "MidiSender.h"
#include "../mapping/MappingEngine.h"   // Etat par pipeline de chaque emplacement

class MidiRouter : public MidiSender {
public:
    /* Emplacements de scripts map montes en CHAINE (voir plus bas). */
    static constexpr uint8_t MAX_SCRIPTS_MAP = 4;

    MidiRouter();
    ~MidiRouter() override;

    void begin() override;
    void update() override;

    void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override;
    void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override;
    void sendControlChange(uint8_t channel, uint8_t control, uint8_t value) override;
    
    // Nouveaux messages MIDI
    void sendProgramChange(uint8_t channel, uint8_t program) override;
    void sendPitchBend(uint8_t channel, int bend) override;
    void sendAftertouch(uint8_t channel, uint8_t pressure) override;
    void sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) override;
    void sendClock() override;
    void sendStart() override;
    void sendStop() override;
    void sendContinue() override;

    void enableRtpMidi(bool enabled);
    void enableOsc(bool enabled);
    void enableBluetooth(bool enabled);
    void enableUsbMidi(bool enabled);

    void setOscTargetSta(bool sta);
    void setOscPort(uint16_t port);

    void setMidiChannel(uint8_t channel); // défaut 1
    
    // ── Script .nms de mapping du MIDI ENTRANT ──────────────────────────────
    // TOUT est fait dans la carte : le navigateur POUSSE le script, il ne
    // l'execute jamais. Debranche, la carte continue d'appliquer le meme
    // traitement — c'est la regle du headless.
    void setScriptMidi(const String& script, uint8_t emplacement = 0);

    /* Le battement d'horloge du script map. Appele periodiquement par la tache
     * temps reel : c'est lui qui fait tourner metro(), et qui envoie le Init de
     * loadbang() apres un chargement. Sans lui, un script map ne s'executait
     * QUE sur un MIDI entrant — donc jamais en headless si rien n'entre, et
     * deux scripts ne pouvaient pas se parler par le registre. */
    void battreHorloge(uint32_t maintenant);


    // Parametres du script, format "cle=valeur;cle=valeur". Un .nms lit ses
    // reglages par r("param","nom",min,max,defaut) : sans eux il retombe sur le
    // DEFAUT et parait ne rien faire — c'est ce qui rendait un bloc transpose
    // silencieux alors que le script etait bien charge.
    void setParamsScript(const String& params);

    // Charge un script PAR NOM depuis mapfs. persister = memoriser ce nom en
    // NVS pour qu'il revienne au demarrage : la carte redevient autonome, sans
    // qu'un navigateur ait a la reconfigurer. Seul le NOM va en NVS — le
    // contenu vit dans LittleFS, et l'y ecrire a chaque cue ferait payer une
    // ecriture flash, donc un craquement audio (MESURES.md §13).
    bool chargerScriptNomme(const char* nom, bool persister = false, uint8_t emplacement = 0);
    const String& nomScript() const { return emplacements[0].nom; }
    /* Le nom porte par un emplacement donne — chaine vide s'il est libre. */
    const String& nomEmplacement(uint8_t e) const {
        static const String vide;
        return (e < MAX_SCRIPTS_MAP) ? emplacements[e].nom : vide;
    }

    // Au boot : recharge le script memorise. Appele une fois depuis nidmi_setup.
    void restaurerScript();
    const String& scriptMidi() const { return emplacements[0].contenu; }

    // Point d'entree UNIQUE de toute note ENTRANTE (USB, clavier de l'app par
    // WebSocket, RTP...) : applique le script s'il y en a un, puis joue.
    // Sans lui, chaque source appelait AudioEngine directement et le script
    // n'aurait porte que sur celles qu'on aurait pense cabler.
    void noteEntrante(uint8_t channel, uint8_t note, uint8_t velocity, bool estNoteOff);

    // Meme role pour les CONTROLEURS CONTINUS. Un CC entrant n'allait qu'aux
    // composants (LEDs) : rien ne le reliait a un parametre, donc le CC learn
    // n'existait que dans le navigateur et mourait avec lui. Il passe
    // desormais par la table CcMap — apprentissage compris — avant d'aller
    // aux composants comme avant.
    void ccEntrant(uint8_t channel, uint8_t control, uint8_t value);

    /* La chaine des scripts map appliquee a un CC, sans emission ni
     * apprentissage : ce que les emplacements font de (canal, cc, valeur).
     * Rend false si l'un d'eux a avale l'evenement. Employee par ccEntrant ET
     * par /api/midi/chaine — un seul code, donc un essai qui prouve la
     * production. */
    bool chaineScriptsCc(uint8_t& canal, uint8_t& cc, uint8_t& valeur);

    // Réception MIDI pour piloter les LEDs
    void handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiControlChange(uint8_t channel, uint8_t control, uint8_t value);

private:
    /* PLUSIEURS SCRIPTS MAP, en CHAINE.
     *
     * Il n'y en avait qu'un (`scriptEntrant`), si bien qu'une composition a deux
     * pistes map ne pouvait pas exister en headless. La limite n'etait pas dans
     * le stockage — mapfs tient les fichiers depuis toujours — mais dans le
     * runtime, qui n'en tenait qu'un.
     *
     * SEMANTIQUE : une chaine, pas un parallele. C'est deja ce que le code
     * faisait avec un seul script — il transforme l'evenement, et la suite voit
     * le resultat, pas l'original. Chaque emplacement peut donc transformer a
     * son tour, ou AVALER l'evenement (traite sans emission), ce qui rend un
     * filtre exprimable. Les blocs d'une piste s'enchainent de la meme facon
     * cote app : la carte dit la meme chose.
     *
     * Chaque emplacement a SON etat par pipeline. Sans ca ils partageraient le
     * tableau global du moteur — deux toggle() dans deux scripts differents se
     * marcheraient dessus sans rien dire (meme piege que le §59). */

    struct Emplacement {
        String contenu;        // "" = emplacement vide, ignore
        String nom;            // nom du fichier .nms, ou "(en ligne)"
        bool   initEnAttente = true;   // un loadbang() est du
        MappingEngine::Etat etats[MappingEngine::MAX_PIPELINES_SCRIPT];
    };
    Emplacement emplacements[MAX_SCRIPTS_MAP];
    bool rtpEnabled;
    bool oscEnabled;
    bool bluetoothEnabled;
    bool usbMidiEnabled;
    bool oscToSta;
    uint16_t oscPort;
    uint8_t defaultChannel;
};


