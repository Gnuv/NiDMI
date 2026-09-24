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

    /** La sortie USB ne perd rien (voir « LA SORTIE MIDI USB NE PERD RIEN »
     *  dans le .cpp) : ce qu'elle a fait, pour /api/diag/reservoirs. */
    struct StatsSortie {
        uint32_t envoyes;     // paquets remis a TinyUSB
        uint32_t attentes;    // tours ou la file de TinyUSB (16 messages) etait pleine
        uint32_t debordes;    // refuses par NOTRE file pleine (rattrapes par la reconciliation)
        uint32_t sansHote;    // non remis : USB non monte, ou en veille
        uint32_t relaches;    // note-off emis par la reconciliation
        uint16_t fileMax;     // remplissage maximal vu de notre file
        uint16_t capacite;
    };
    static void statsSortie(StatsSortie& s);
    static void reinitStatsSortie();

    /** Banc (POST /api/diag/midi-rafale) : un accord de `notes` notes, toutes
     *  les note-on puis toutes les note-off, d'un seul coup. `direct` rejoue
     *  l'ANCIEN chemin — ecriture directe dans la file de TinyUSB, retour
     *  ignore — pour mesurer ce qu'il perdait, dans le meme demarrage. */
    void rafaleBanc(uint16_t notes, uint8_t canal, uint8_t velocite, bool direct);

private:
    bool isUsbOtgEnabled() const;
};
