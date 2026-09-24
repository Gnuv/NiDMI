#include "UsbMidiManager.h"
#include "../server/WebDebugConsole.h"

#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
#include <Preferences.h>
#include <atomic>
#include "tusb.h"   // tud_ready, tud_mounted, tud_midi_packet_write
#include <esp_heap_caps.h>   // le stockage de la file, en PSRAM

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

/* ── LA SORTIE MIDI USB NE PERD RIEN ─────────────────────────────────────
 * La file d'emission de TinyUSB tient 64 octets : 16 messages (figee dans la
 * lib precompilee, CFG_TUD_MIDI_TX_BUFSIZE). tud_midi_packet_write rend
 * `false` quand elle est pleine — et chaque envoi ignorait ce retour : une
 * rafale de plus de 16 messages avant que l'hote ne vide la file PERDAIT des
 * messages, note-off compris, donc des notes bloquees (MESURES §151).
 *
 * Aucun emetteur n'ecrit plus dans la file de TinyUSB. Ils deposent dans NOTRE
 * file (1 024 messages) sans jamais attendre — les capteurs et le MIDI ne se
 * bloquent pas pour l'USB — et une tache, la POMPE, la vide dans TinyUSB, en
 * attendant la place quand il le faut (1 ms : une trame USB). Un seul
 * consommateur : les messages sortent dans l'ordre des depots. Le stockage de
 * la file est en PSRAM : seules des taches y touchent, jamais cache coupe ;
 * 256 messages en RAM interne debordaient sous une rafale de 1 024 en 5 ms
 * (MESURES §151).
 *
 * Si notre file deborde (l'hote n'a pas lu depuis 1 024 messages) ou si l'hote
 * est absent (USB non monte, ou en veille), des messages ne partent pas. Aucune
 * note ne doit rester bloquee pour autant. D'ou deux cartes, 16 canaux x 128
 * notes :
 *   s_voulues  — ce que les emetteurs VEULENT allume, mise a jour a chaque
 *                depot AVANT la file : elle compte aussi ce qui a deborde ;
 *   s_chezHote — ce que l'hote a RECU allume, tenue par la pompe seule.
 * Apres toute perte, des que la file est vide et l'hote present, la pompe
 * RECONCILIE : chaque note allumee chez l'hote et eteinte dans l'intention
 * recoit son note-off. Une note-on perdue n'est jamais rejouee en retard : une
 * note manquee plutot qu'une note a contretemps. */
namespace {

constexpr UBaseType_t kCapaciteFile = 1024;
StaticQueue_t s_fileStruct;   // en RAM interne : il porte le verrou de la file
QueueHandle_t s_file = nullptr;
UBaseType_t s_capacite = 0;

// Statiques : la creation ne peut pas echouer, il n'y a pas de chemin
// « sans file » ou l'on reviendrait a l'ecriture directe.
StackType_t s_pilePompe[2048];   // en octets sous ESP-IDF
StaticTask_t s_tcbPompe;

std::atomic<uint32_t> s_voulues[16][4];   // 128 bits par canal
uint32_t s_chezHote[16][4];               // la pompe seule y touche
std::atomic<bool> s_aReconcilier{false};

std::atomic<uint32_t> s_envoyes{0}, s_attentes{0}, s_debordes{0}, s_sansHote{0}, s_relaches{0}, s_fileMax{0};

// Un paquet USB-MIDI (4 octets : entete = cable<<4 | CIN, statut, d1, d2) tient
// dans un mot ; l'ordre des octets en memoire est celui du paquet.
inline uint32_t emballer(uint8_t cin, uint8_t statut, uint8_t d1, uint8_t d2) {
    const uint8_t o[4] = {cin, statut, d1, d2};
    uint32_t p;
    memcpy(&p, o, 4);
    return p;
}

// Ce qu'un paquet fait d'une note : +1 l'allume, -1 l'eteint, 0 rien. Une
// note-on de velocite 0 EST une note-off. Les CC 120 et 123 (tous sons coupes,
// toutes notes eteintes) eteignent le canal entier : `canalEntier`.
inline int effet(uint32_t p, uint8_t& canal, uint8_t& note, bool& canalEntier) {
    uint8_t o[4];
    memcpy(o, &p, 4);
    const uint8_t cin = o[0] & 0x0F;
    canal = o[1] & 0x0F;
    note = o[2] & 0x7F;
    canalEntier = (cin == 0xB) && (note == 120 || note == 123);
    if (cin == 0x9) return o[3] ? +1 : -1;
    if (cin == 0x8) return -1;
    return 0;
}

void noterIntention(uint32_t p) {
    uint8_t canal, note;
    bool tout;
    const int e = effet(p, canal, note, tout);
    if (e > 0) {
        s_voulues[canal][note >> 5].fetch_or(1u << (note & 31));
    } else if (e < 0) {
        s_voulues[canal][note >> 5].fetch_and(~(1u << (note & 31)));
    } else if (tout) {
        for (auto& mot : s_voulues[canal]) mot.store(0);
    }
}

void noterChezHote(uint32_t p) {
    uint8_t canal, note;
    bool tout;
    const int e = effet(p, canal, note, tout);
    if (e > 0) {
        s_chezHote[canal][note >> 5] |= 1u << (note & 31);
    } else if (e < 0) {
        s_chezHote[canal][note >> 5] &= ~(1u << (note & 31));
    } else if (tout) {
        memset(s_chezHote[canal], 0, sizeof s_chezHote[canal]);
    }
}

// Remet un paquet a TinyUSB, en attendant la place. Faux si l'hote est absent :
// le paquet ne part pas, et une reconciliation est due a son retour.
bool livrer(uint32_t p) {
    for (;;) {
        if (!tud_ready()) {
            if (!tud_mounted()) {
                memset(s_chezHote, 0, sizeof s_chezHote);   // demonte : l'hote a tout oublie
            }
            s_sansHote++;
            s_aReconcilier = true;
            return false;
        }
        uint8_t octets[4];
        memcpy(octets, &p, 4);
        if (tud_midi_packet_write(octets)) {
            noterChezHote(p);
            s_envoyes++;
            return true;
        }
        s_attentes++;
        vTaskDelay(1);   // la file de TinyUSB se vide a chaque transfert : une trame USB
    }
}

void reconcilier() {
    s_aReconcilier = false;   // AVANT de lire : une perte pendant le balayage la redemande
    for (uint8_t canal = 0; canal < 16; ++canal) {
        for (uint8_t m = 0; m < 4; ++m) {
            uint32_t reste = s_chezHote[canal][m] & ~s_voulues[canal][m].load();
            while (reste) {
                const uint8_t note = (uint8_t)(m * 32 + __builtin_ctz(reste));
                reste &= reste - 1;
                if (!livrer(emballer(0x08, (uint8_t)(0x80 | canal), note, 0))) {
                    return;   // l'hote est reparti : a son retour
                }
                s_relaches++;
            }
        }
    }
}

// Coeur 0, priorite 19 : sous les capteurs (20), au rang du MIDI, au-dessus de
// la pile reseau (18) — MESURES §149. Elle dort sur la file ; reveillee toutes
// les 20 ms seulement quand une reconciliation attend l'hote.
void pompe(void*) {
    uint32_t p;
    for (;;) {
        const TickType_t attente = s_aReconcilier ? pdMS_TO_TICKS(20) : portMAX_DELAY;
        if (xQueueReceive(s_file, &p, attente) == pdTRUE) {
            livrer(p);
            continue;
        }
        if (s_aReconcilier && tud_ready()) {
            reconcilier();
        }
    }
}

// Depot par un emetteur, quel qu'il soit (capteurs, MIDI, scripts, web) : ne
// bloque jamais.
void deposer(uint8_t cin, uint8_t statut, uint8_t d1, uint8_t d2) {
    const uint32_t p = emballer(cin, statut, d1, d2);
    noterIntention(p);   // AVANT la file : la reconciliation voit aussi ce qui deborde
    if (s_file == nullptr || xQueueSend(s_file, &p, 0) != pdTRUE) {
        s_debordes++;
        s_aReconcilier = true;
        return;
    }
    const uint32_t n = (uint32_t)uxQueueMessagesWaiting(s_file);
    uint32_t m = s_fileMax.load();
    while (n > m && !s_fileMax.compare_exchange_weak(m, n)) {
    }
}

}  // namespace
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

/* ── LE COEUR DE L'INTERRUPTION USB ───────────────────────────────────────
 * esp_intr_alloc(), appele par USB.begin() (tinyusb_driver_install), attache
 * l'interruption au coeur QUI L'APPELLE : depuis setup(), le coeur 1, celui de
 * l'audio. La tache usbd la suit (nidmi-core, UsbNetService). C'est VOULU,
 * et mesure (MESURES §149) : l'avoir mise sur le coeur 0 donnait plus de
 * decrochages audio (+6 a +38 par 20 s de charge au lieu de +1 a +3) et
 * effondrait le MIDI. Ne pas la deplacer sans remesurer. */
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

    if (s_file == nullptr) {
        // En PSRAM ; a defaut (jamais vu : 8 Mo libres), un quart en RAM interne.
        UBaseType_t n = kCapaciteFile;
        uint8_t* stockage = (uint8_t*)heap_caps_malloc(n * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (stockage == nullptr) {
            n = kCapaciteFile / 4;
            stockage = (uint8_t*)heap_caps_malloc(n * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (stockage != nullptr) {
            s_capacite = n;
            s_file = xQueueCreateStatic(n, sizeof(uint32_t), stockage, &s_fileStruct);
            xTaskCreateStaticPinnedToCore(pompe, "usbmidi_tx", sizeof(s_pilePompe), nullptr, 19, s_pilePompe,
                                          &s_tcbPompe, 0);
        } else {
            NIDMI_WEB_LOG("[USB-MIDI] ERREUR : pas de memoire pour la file de sortie");
        }
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
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    // RECEPTION. Le MIDI USB etait uniquement SORTANT : un clavier ou un
    // sequenceur branche au port USB de la carte n'avait aucun effet, faute
    // d'etre lu. On draine ici la file d'entree et on dispatche par les hooks.
    //
    // Format d'un paquet USB-MIDI (4 octets) :
    //   header = (cable << 4) | CIN,  CIN 0x8 = note off, 0x9 = note on,
    //                                 0xB = control change
    //   byte1  = statut (0x9n / 0x8n / 0xBn), byte2 = note/CC, byte3 = velo/valeur
    //
    // Convention MIDI respectee : un note-on de velocite 0 EST un note-off.
    // Sans ca, les claviers qui n'emettent jamais de 0x8n laissent des notes
    // tenues pour toujours.
    if (!isStarted || !usbInitialized || usbMidi == nullptr) return;

    midiEventPacket_t paquet;
    // Borne dure : une rafale (glissando, dump SysEx d'un DAW) ne doit pas
    // monopoliser la boucle principale, qui sert aussi les requetes HTTP.
    int garde = 0;
    while (usbMidi->readPacket(&paquet) && ++garde <= 64) {
        const uint8_t cin   = paquet.header & 0x0F;
        const uint8_t canal = (uint8_t)((paquet.byte1 & 0x0F) + 1);   // 1..16
        switch (cin) {
            case 0x9:
                if (paquet.byte3 == 0) {
                    if (onNoteOff) onNoteOff(canal, paquet.byte2, 0);
                } else if (onNoteOn) {
                    onNoteOn(canal, paquet.byte2, paquet.byte3);
                }
                break;
            case 0x8:
                if (onNoteOff) onNoteOff(canal, paquet.byte2, paquet.byte3);
                break;
            case 0xB:
                if (onControlChange) onControlChange(canal, paquet.byte2, paquet.byte3);
                break;
            default:
                break;   // horloge, SysEx, pitch bend : pas encore route
        }
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

/* Tous les envois passent par deposer() : voir « LA SORTIE MIDI USB NE PERD
 * RIEN ». Format d'un paquet : entete CIN (0x8 note off, 0x9 note on, 0xA
 * pression polyphonique, 0xB CC, 0xC programme, 0xD pression de canal, 0xE
 * pitch bend, 0xF temps reel sur un octet), puis l'octet de statut et deux
 * donnees. */
void UsbMidiManager::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        deposer(0x09, (uint8_t)(0x90 | nidmiChannelNibble(channel)), note & 0x7F, velocity & 0x7F);
    }
#endif
}

void UsbMidiManager::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        deposer(0x08, (uint8_t)(0x80 | nidmiChannelNibble(channel)), note & 0x7F, velocity & 0x7F);
    }
#endif
}

void UsbMidiManager::sendControlChange(uint8_t channel, uint8_t control, uint8_t value) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        deposer(0x0B, (uint8_t)(0xB0 | nidmiChannelNibble(channel)), control & 0x7F, value & 0x7F);
    }
#endif
}

void UsbMidiManager::sendProgramChange(uint8_t channel, uint8_t program) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        deposer(0x0C, (uint8_t)(0xC0 | nidmiChannelNibble(channel)), program & 0x7F, 0x00);
    }
#endif
}

void UsbMidiManager::sendPitchBend(uint8_t channel, int bend) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        // -8192..8191 -> 0..16383, poids faible (bits 0-6) puis poids fort (7-13)
        const uint16_t b = (uint16_t)(constrain(bend, -8192, 8191) + 8192);
        deposer(0x0E, (uint8_t)(0xE0 | nidmiChannelNibble(channel)), b & 0x7F, (b >> 7) & 0x7F);
    }
#endif
}

void UsbMidiManager::sendAftertouch(uint8_t channel, uint8_t pressure) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        deposer(0x0D, (uint8_t)(0xD0 | nidmiChannelNibble(channel)), pressure & 0x7F, 0x00);
    }
#endif
}

void UsbMidiManager::sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) {
        deposer(0x0A, (uint8_t)(0xA0 | nidmiChannelNibble(channel)), note & 0x7F, pressure & 0x7F);
    }
#endif
}

void UsbMidiManager::sendClock() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) deposer(0x0F, 0xF8, 0x00, 0x00);
#endif
}

void UsbMidiManager::sendStart() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) deposer(0x0F, 0xFA, 0x00, 0x00);
#endif
}

void UsbMidiManager::sendStop() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) deposer(0x0F, 0xFC, 0x00, 0x00);
#endif
}

void UsbMidiManager::sendContinue() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (usbMidi && isConnected()) deposer(0x0F, 0xFB, 0x00, 0x00);
#endif
}

void UsbMidiManager::statsSortie(StatsSortie& s) {
    s = {};
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    s.envoyes = s_envoyes;
    s.attentes = s_attentes;
    s.debordes = s_debordes;
    s.sansHote = s_sansHote;
    s.relaches = s_relaches;
    s.fileMax = (uint16_t)s_fileMax.load();
    s.capacite = (uint16_t)s_capacite;
#endif
}

void UsbMidiManager::reinitStatsSortie() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    s_envoyes = 0;
    s_attentes = 0;
    s_debordes = 0;
    s_sansHote = 0;
    s_relaches = 0;
    s_fileMax = 0;
#endif
}

void UsbMidiManager::rafaleBanc(uint16_t notes, uint8_t canal, uint8_t velocite, bool direct) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (!usbMidi || !isConnected()) return;
    const uint8_t c = nidmiChannelNibble(canal);
    for (int passe = 0; passe < 2; ++passe) {   // toutes les note-on, puis toutes les note-off
        const uint8_t cin = passe ? 0x08 : 0x09;
        const uint8_t statut = (uint8_t)((passe ? 0x80 : 0x90) | c);
        const uint8_t v = passe ? 0 : (uint8_t)(velocite & 0x7F);
        for (uint16_t i = 0; i < notes && i < 128; ++i) {
            if (direct) {
                midiEventPacket_t paquet = {cin, statut, (uint8_t)i, v};
                usbMidi->writePacket(&paquet);   // l'ancien chemin : retour ignore
            } else {
                deposer(cin, statut, (uint8_t)i, v);
            }
        }
    }
#endif
}
