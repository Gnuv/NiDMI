// Routeur vers implémentations RTP-MIDI et OSC existantes
#pragma once

#include <Arduino.h>
#include "MidiSender.h"

class MidiRouter : public MidiSender {
public:
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
    void setScriptMidi(const String& script);

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
    bool chargerScriptNomme(const char* nom, bool persister = false);
    const String& nomScript() const { return nomScriptActif; }

    // Au boot : recharge le script memorise. Appele une fois depuis nidmi_setup.
    void restaurerScript();
    const String& scriptMidi() const { return scriptEntrant; }

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

    // Réception MIDI pour piloter les LEDs
    void handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiControlChange(uint8_t channel, uint8_t control, uint8_t value);

private:
    String scriptEntrant;      // .nms applique au MIDI entrant ("" = passage direct)
    String nomScriptActif;     // nom du fichier .nms charge ("" = aucun)
    bool rtpEnabled;
    bool oscEnabled;
    bool bluetoothEnabled;
    bool usbMidiEnabled;
    bool oscToSta;
    uint16_t oscPort;
    uint8_t defaultChannel;
};


