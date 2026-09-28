// Interface d'envoi MIDI et OSC unifiée
#pragma once

#include <Arduino.h>

class MidiSender {
public:
    virtual ~MidiSender() {}

    virtual void begin() = 0;

    // Messages MIDI basiques
    virtual void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) = 0;
    virtual void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) = 0;
    virtual void sendControlChange(uint8_t channel, uint8_t control, uint8_t value) = 0;
    
    // Nouveaux messages MIDI
    virtual void sendProgramChange(uint8_t channel, uint8_t program) = 0;
    virtual void sendPitchBend(uint8_t channel, int bend) = 0;  // -8192 à +8191, centre=0
    virtual void sendAftertouch(uint8_t channel, uint8_t pressure) = 0;
    virtual void sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) = 0;  // Key Pressure (Polyphonic Aftertouch)
    virtual void sendClock() = 0;      // MIDI Clock (pas de canal)
    virtual void sendStart() = 0;      // MIDI Start
    virtual void sendStop() = 0;       // MIDI Stop
    virtual void sendContinue() = 0;   // MIDI Continue
};


