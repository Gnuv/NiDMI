#include "MidiRouter.h"
#include <Arduino.h>

// Dépendances vers le serveur core
#include "../server/ServerCore.h"
#include "../managers/ComponentManager.h"
#include "../Globals.h"
#include "../audio/AudioEngine.h"

MidiRouter::MidiRouter()
    : rtpEnabled(true), oscEnabled(true), bluetoothEnabled(true), usbMidiEnabled(true), oscToSta(true), oscPort(8000), defaultChannel(1) {}

MidiRouter::~MidiRouter() {}

void MidiRouter::begin() {
    // Initialiser USB MIDI si supporté et activé
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().begin();
    }
}

void MidiRouter::update() {
    // Mise à jour RTP si nécessaire
    serverCore.rtpMidi().update();
}

void MidiRouter::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendNoteOn(ch, note, velocity);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendNoteOn(ch, note, velocity);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendNoteOn(ch, note, velocity);
    }
    // Optionnel: route OSC si disponible côté serveur
    // Activez avec -DNIDMI_ENABLE_OSC_ROUTER et implémentez les wrappers dans NiDMIServer
    // Écho local : les LEDs/bargraphs sont pilotés par note+canal via handleMidiNoteOn(), qui
    // n'était câblé qu'aux notes ENTRANTES (hooks RTP-MIDI). Sans cet écho, une note générée
    // localement par un composant du boîtier n'allume aucune LED du même boîtier.
    g_componentManager.handleMidiNoteOn(ch, note, velocity);

    // Écho AUDIO : le boîtier s'entend lui-même. Le moteur démarre à la
    // première note (init paresseuse) — rien au boot, l'OTA reste sauf.
    AudioEngine::noteOn(note, velocity);

    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscNote(ch, note, velocity, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendNoteOff(ch, note, velocity);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendNoteOff(ch, note, velocity);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendNoteOff(ch, note, velocity);
    }
    // Écho local (cf. sendNoteOn) : éteint les LEDs appairées à cette note.
    g_componentManager.handleMidiNoteOff(ch, note, velocity);

    AudioEngine::noteOff(note);

    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscNoteOff(ch, note, velocity, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendControlChange(uint8_t channel, uint8_t control, uint8_t value) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendControlChange(ch, control, value);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendControlChange(ch, control, value);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendControlChange(ch, control, value);
    }
    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscCC(ch, control, value, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendProgramChange(uint8_t channel, uint8_t program) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendProgramChange(ch, program);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendProgramChange(ch, program);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendProgramChange(ch, program);
    }
}

void MidiRouter::sendPitchBend(uint8_t channel, int bend) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendPitchBend(ch, bend);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendPitchBend(ch, bend);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendPitchBend(ch, bend);
    }
}

void MidiRouter::sendAftertouch(uint8_t channel, uint8_t pressure) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendAftertouch(ch, pressure);
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendAftertouch, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendAftertouch(ch, pressure);
    }
}

void MidiRouter::sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendKeyPressure(ch, note, pressure);
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendKeyPressure, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendKeyPressure(ch, note, pressure);
    }

    // Écho local (cf. sendNoteOn) : module la luminosité des LEDs en mode PWM appairées
    // à cette note, pour que l'aftertouch soit visible sur le boîtier lui-même.
    g_componentManager.handleMidiKeyPressure(ch, note, pressure);
    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscKeyPressure(ch, note, pressure, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendClock() {
    if (rtpEnabled) {
        serverCore.rtpMidi().sendClock();
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendClock, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendClock();
    }
}

void MidiRouter::sendStart() {
    if (rtpEnabled) {
        serverCore.rtpMidi().sendStart();
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendStart, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendStart();
    }
}

void MidiRouter::sendStop() {
    if (rtpEnabled) {
        serverCore.rtpMidi().sendStop();
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendStop, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendStop();
    }
}

void MidiRouter::sendContinue() {
    if (rtpEnabled) {
        serverCore.rtpMidi().sendContinue();
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendContinue, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendContinue();
    }
}

void MidiRouter::enableRtpMidi(bool enabled) { rtpEnabled = enabled; }
void MidiRouter::enableOsc(bool enabled) { oscEnabled = enabled; }
void MidiRouter::enableBluetooth(bool enabled) { bluetoothEnabled = enabled; }
void MidiRouter::enableUsbMidi(bool enabled) { usbMidiEnabled = enabled; }
void MidiRouter::setOscTargetSta(bool sta) { oscToSta = sta; }
void MidiRouter::setOscPort(uint16_t port) { oscPort = port; }
void MidiRouter::setMidiChannel(uint8_t channel) { defaultChannel = channel; }

void MidiRouter::handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    // Transmettre au ComponentManager pour piloter les LEDs
    g_componentManager.handleMidiNoteOn(channel, note, velocity);
}

void MidiRouter::handleMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    // Transmettre au ComponentManager pour piloter les LEDs
    g_componentManager.handleMidiNoteOff(channel, note, velocity);
}

void MidiRouter::handleMidiControlChange(uint8_t channel, uint8_t control, uint8_t value) {
    // Transmettre au ComponentManager pour piloter les LEDs
    g_componentManager.handleMidiControlChange(channel, control, value);
}


