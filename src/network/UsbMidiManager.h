#pragma once

#include <Arduino.h>

// USB MIDI seulement pour ESP32-S3 avec USB-OTG activé
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32S3)
#define NIDMI_USB_MIDI_SUPPORTED
#endif

#ifdef NIDMI_USB_MIDI_SUPPORTED
#ifndef NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
/** 1 = USB-MIDI initialisé au boot ; 0 = désactivé (série USB plus simple pour le dev). */
#define NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME 1
#endif
#endif

/** Vrai si le firmware a été compilé avec USB-MIDI activé au boot (S3 uniquement). */
inline bool nidmi_usb_midi_enabled_at_compile_time() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && (NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME)
    return true;
#else
    return false;
#endif
}

#ifdef NIDMI_USB_MIDI_SUPPORTED
#include <USB.h>
#if NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
#include <USBMIDI.h>
#endif
#endif

/**
 * @brief Gestionnaire USB MIDI
 * 
 * Cette classe gère la communication MIDI via USB.
 * Disponible uniquement sur ESP32-S3 avec USB-OTG activé (CONFIG_SOC_USB_OTG_SUPPORTED=y).
 */
class UsbMidiManager {
public:
    // (canal 1..16, note/controleur, velocite/valeur)
    typedef void (*HookNote)(uint8_t, uint8_t, uint8_t);
    typedef void (*HookCC)(uint8_t, uint8_t, uint8_t);

private:
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    USBMIDI* usbMidi;
    bool usbInitialized;
#endif
    bool isStarted;
    bool available; // USB disponible

    // Hooks d'ENTREE. Le MIDI USB etait jusqu'ici uniquement SORTANT : update()
    // portait le commentaire « peut etre ajoute plus tard pour la reception ».
    // Meme forme que les hooks RTP-MIDI de NiDMIServer, pour que le cablage se
    // lise pareil des deux cotes.
    HookNote onNoteOn = nullptr;
    HookNote onNoteOff = nullptr;
    HookCC   onControlChange = nullptr;
    
public:
    UsbMidiManager();
    ~UsbMidiManager();
    
    // Initialisation
    bool begin();
    void stop();
    void update();
    
    // Envoi MIDI
    void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    void sendControlChange(uint8_t channel, uint8_t control, uint8_t value);
    void sendProgramChange(uint8_t channel, uint8_t program);
    void sendPitchBend(uint8_t channel, int bend);
    void sendAftertouch(uint8_t channel, uint8_t pressure);
    void sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure);
    void sendClock();
    void sendStart();
    void sendStop();
    void sendContinue();
    
    // Reception : brancher ce que devient une note entrante. Sans ces hooks,
    // update() lit les paquets et les jette.
    void setMidiInputHooks(HookNote noteOn, HookNote noteOff, HookCC cc) {
        onNoteOn = noteOn; onNoteOff = noteOff; onControlChange = cc;
    }

    // État de connexion
    bool isConnected() const;
    bool isInitialized() const { return isStarted; }
    bool isSupported() const;
    
private:
    void sendMidiMessage(uint8_t status, uint8_t data1, uint8_t data2);
    void sendMidiMessage(uint8_t status, uint8_t data1);
    bool isUsbOtgEnabled() const;
};
