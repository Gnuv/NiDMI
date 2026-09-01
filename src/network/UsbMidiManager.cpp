#include "UsbMidiManager.h"
#include "../server/WebDebugConsole.h"

#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
#include <Preferences.h>

// Même clé et règles que nidmi_begin() : mdns_name (SSID AP, mDNS, RTP, BT, nom USB MIDI).
static String nidmiUsbMidiHostNameFromNvs() {
    Preferences prefs;
    String name = "nidmi";
    if (prefs.begin("nidmi", true)) {
        name = prefs.getString("mdns_name", "nidmi");
        prefs.end();
    }
    name.trim();
    name.replace("\n", "");
    name.replace("\r", "");
    name.replace("\t", "");
    if (name.length() == 0) {
        name = "nidmi";
    }
    // Limite constructeur USBMIDI(const char*) sur Arduino-ESP32 >= 3.3.1
    const size_t kMaxUsbMidiName = 32;
    if (name.length() > kMaxUsbMidiName) {
        name = name.substring(0, kMaxUsbMidiName);
    }
    return name;
}
#endif

UsbMidiManager::UsbMidiManager() 
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    : usbMidi(nullptr), usbInitialized(false), isStarted(false), available(false) {
#else
    : isStarted(false), available(false) {
#endif
}

UsbMidiManager::~UsbMidiManager() {
    stop();

#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    // Nettoyer explicitement l'objet alloué (évite fuite mémoire si on re-désactive/active souvent)
    if (usbMidi) {
        delete usbMidi;
        usbMidi = nullptr;
    }
#endif
}

bool UsbMidiManager::isSupported() const {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    return true;
#else
    return false;
#endif
}

bool UsbMidiManager::isUsbOtgEnabled() const {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    // Sur ESP32-S3, si sdkconfig.defaults contient CONFIG_SOC_USB_OTG_SUPPORTED=y,
    // USB-OTG est activé au niveau hardware même si ARDUINO_USB_MODE indique mode série.
    // On ne peut pas vérifier sdkconfig.defaults depuis le code C++, donc on fait confiance
    // au fait que si le build a réussi avec sdkconfig.defaults, USB-OTG est disponible.
    
    // Si ARDUINO_USB_MODE est défini et != 1, c'est USB-OTG
    #ifdef ARDUINO_USB_MODE
        #if ARDUINO_USB_MODE != 1
            return true; // USB-OTG activé
        #endif
    #endif
    
    // Si ARDUINO_USB_MODE == 1 ou non défini, on retourne true par défaut
    // car sdkconfig.defaults peut activer USB-OTG indépendamment
    // (l'initialisation USB.begin() échouera si vraiment USB-OTG n'est pas disponible)
    return true;
#else
    return false; // Pas un ESP32-S3
#endif
}

bool UsbMidiManager::begin() {
    if (isStarted) {
        return true;
    }
    
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    // Vérifier que USB-OTG est activé
    if (!isUsbOtgEnabled()) {
        NIDMI_WEB_LOG("[USB-MIDI] ERREUR: USB-OTG non activé!");
        NIDMI_WEB_LOG("[USB-MIDI] Vérifiez ci.json / sdkconfig: CONFIG_SOC_USB_OTG_SUPPORTED=y");
        NIDMI_WEB_LOG("[USB-MIDI] Arduino: Outils > USB Type > USB-OTG (TinyUSB)");
        available = false;
        return false;
    }
    
    const String hostName = nidmiUsbMidiHostNameFromNvs();

    // Ne pas ré-initialiser complètement à chaque activation.
    // On crée l'objet une fois, puis on réutilise l'initialisation USB déjà faite.
    if (!usbMidi) {
        // Garde par FONCTIONNALITÉ, pas par version : le paquet "3.3.1" rapporte
        // ESP_ARDUINO_VERSION == 3.3.0 (PATCH=0 dans esp_arduino_version.h), donc
        // un test >= VAL(3,3,1) ne prend jamais le constructeur nommé. La macro
        // ESP32_USB_MIDI_DEFAULT_NAME n'existe que là où l'API nommée existe.
#if defined(ESP32_USB_MIDI_DEFAULT_NAME)
        usbMidi = new USBMIDI(hostName.c_str());
#else
        usbMidi = new USBMIDI();
#endif
    }

    if (!usbInitialized) {
        // Sans ceci, l’OS affiche encore le fabricant par défaut du core (« Espressif Systems »).
        USB.manufacturerName("NiDMI");
        USB.productName(hostName.c_str());
        usbMidi->begin();
        USB.begin();
        usbInitialized = true;
    }

    isStarted = true;
    available = true;

    NIDMI_WEB_LOG("[USB-MIDI] Initialise (USB-OTG), nom USB/MIDI: %s", hostName.c_str());
    return true;
#else
    NIDMI_WEB_LOG("[USB-MIDI] Non supporté sur ce MCU (ESP32-S3 requis)");
    available = false;
    return false;
#endif
}

void UsbMidiManager::stop() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    // Désactiver le routage (etat "connected/enabled" pour l'API/UI),
    // sans détruire l'instance USB pour limiter les risques de crash
    // lors d'un toggle depuis l'interface web.
    isStarted = false;
    available = false;
#endif
    // Pour les builds non supportés aussi, garantir un état "désactivé"
    isStarted = false;
    available = false;
}

void UsbMidiManager::update() {
    // Vérification de connexion USB si nécessaire
    // Sur ESP32-S3, USB est toujours disponible une fois initialisé
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (isStarted && usbInitialized) {
        // Lire les messages MIDI entrants si nécessaire
        // (peut être ajouté plus tard pour la réception)
    }
#endif
}

bool UsbMidiManager::isConnected() const {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    return isStarted && usbInitialized && available;
#else
    return false;
#endif
}

// [correctif NiDMI] Canal 1-16 (convention NiDMI, celle de l'UI) -> nibble 0-15
// (convention MIDI). Les six constructions d'octet de statut de ce fichier
// masquaient le canal SANS retrancher 1, malgre le commentaire qui l'annonçait :
// le canal 1 sortait donc sur le canal 2. BluetoothManager.cpp:124 fait, lui,
// la conversion correctement — d'ou la divergence entre transports.
static inline uint8_t nidmiChannelNibble(uint8_t channel) {
    return (uint8_t)((channel > 0 ? channel - 1 : 0) & 0x0F);
}

void UsbMidiManager::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        // USB MIDI format: CIN=0x09 pour Note On
        // Status: 0x90-0x9F (Note On, 0x9n où n=channel 0-15)
        uint8_t status = 0x90 | nidmiChannelNibble(channel);
        midiEventPacket_t packet = {0x09, status, note, velocity};
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        // USB MIDI format: CIN=0x08 pour Note Off
        // Status: 0x80-0x8F (Note Off, 0x8n où n=channel 0-15)
        uint8_t status = 0x80 | nidmiChannelNibble(channel);
        midiEventPacket_t packet = {0x08, status, note, velocity};
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendControlChange(uint8_t channel, uint8_t control, uint8_t value) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        // USB MIDI format: CIN=0x0B pour Control Change
        // Status: 0xB0-0xBF (Control Change, 0xBn où n=channel 0-15)
        uint8_t status = 0xB0 | nidmiChannelNibble(channel);
        midiEventPacket_t packet = {0x0B, status, control, value};
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendProgramChange(uint8_t channel, uint8_t program) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        // USB MIDI format: CIN=0x0C pour Program Change
        // Status: 0xC0-0xCF (Program Change, 0xCn où n=channel 0-15)
        // Data1: program (0-127), Data2: 0x00 (non utilisé)
        uint8_t status = 0xC0 | nidmiChannelNibble(channel);
        midiEventPacket_t packet = {0x0C, status, program, 0x00};
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendPitchBend(uint8_t channel, int bend) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        // Convertir bend (-8192 à 8191) en format MIDI (0-16383)
        uint16_t midiBend = (uint16_t)(bend + 8192);
        // USB MIDI format: CIN=0x0E pour Pitch Bend Change (3 octets)
        // Status: 0xE0-0xEF (Pitch Bend Change, 0xEn où n=channel 0-15)
        // Data1: LSB (bits 0-6), Data2: MSB (bits 7-13)
        uint8_t status = 0xE0 | nidmiChannelNibble(channel);
        uint8_t lsb = midiBend & 0x7F; // Bits 0-6
        uint8_t msb = (midiBend >> 7) & 0x7F; // Bits 7-13
        midiEventPacket_t packet = {0x0E, status, lsb, msb};
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendAftertouch(uint8_t channel, uint8_t pressure) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        // USB MIDI format: CIN=0x04 pour Channel Voice Messages à 2 octets
        // USB MIDI format: CIN=0x0D pour Channel Pressure (2 octets)
        // Status: 0xD0-0xDF (Channel Pressure, 0xDn où n=channel 0-15)
        // Data1: pressure (0-127), Data2: 0x00 (non utilisé)
        uint8_t status = 0xD0 | nidmiChannelNibble(channel);
        midiEventPacket_t packet = {0x0D, status, pressure, 0x00};
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        // USB MIDI format: CIN=0x0A pour Polyphonic Key Pressure (3 octets)
        // Status: 0xA0-0xAF (Polyphonic Key Pressure, 0xAn où n=channel 0-15)
        // Data1: note (0-127), Data2: pressure (0-127)
        uint8_t status = 0xA0 | nidmiChannelNibble(channel);
        midiEventPacket_t packet = {0x0A, status, note & 0x7F, pressure & 0x7F};
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendClock() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        // USB MIDI format: header (CIN=0x0F pour Real-Time), byte1=message, byte2=0, byte3=0
        midiEventPacket_t packet = {0x0F, 0xF8, 0x00, 0x00}; // MIDI Clock (0xF8)
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendStart() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        midiEventPacket_t packet = {0x0F, 0xFA, 0x00, 0x00}; // MIDI Start (0xFA)
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendStop() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        midiEventPacket_t packet = {0x0F, 0xFC, 0x00, 0x00}; // MIDI Stop (0xFC)
        usbMidi->writePacket(&packet);
    }
#endif
}

void UsbMidiManager::sendContinue() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        midiEventPacket_t packet = {0x0F, 0xFB, 0x00, 0x00}; // MIDI Continue (0xFB)
        usbMidi->writePacket(&packet);
    }
#endif
}
