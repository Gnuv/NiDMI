#include "UsbMidiManager.h"
#include "../server/WebDebugConsole.h"
#include "../diag/Activite.h"   // la LED d'activite de l'app (MESURES §167)
#include "../diag/Chronos.h"    // ce que coute un message entrant (MESURES §170)

#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
#include <Preferences.h>
#include <atomic>
#include "tusb.h"   // tud_ready, tud_mounted, tud_midi_packet_write
#include "device/usbd_pvt.h"   // usbd_defer_func : relire l'entree dans la tache usbd
#include <esp_heap_caps.h>   // le stockage de la file, en PSRAM
#include <esp_timer.h>       // l'heure d'arrivee d'un message, en microsecondes

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
// « sans file » ou l'on reviendrait a l'ecriture directe. 2 560 o : elle en
// utilise 1 072 au plus, releve apres les rafales du banc (MESURES §151) ;
// c'est le chemin du MIDI, la marge est large a dessein.
StackType_t s_pilePompe[2560];   // en octets sous ESP-IDF
StaticTask_t s_tcbPompe;

std::atomic<uint32_t> s_voulues[16][4];   // 128 bits par canal
uint32_t s_chezHote[16][4];               // la pompe seule y touche
std::atomic<bool> s_aReconcilier{false};

std::atomic<uint32_t> s_envoyes{0}, s_attentes{0}, s_debordes{0}, s_sansHote{0}, s_relaches{0}, s_fileMax{0};
// Un geste TENU est passe par l'USB depuis le demarrage (§183) — jamais remis a zero.
std::atomic<bool> s_tenuDepuisDemarrage{false};
// Tenu : une note, le pitch bend, une pedale (CC 64..69) — voir le .h.
inline bool estTenu(uint8_t statut, uint8_t d1) {
    const uint8_t t = statut & 0xF0;
    return t == 0x80 || t == 0x90 || t == 0xE0 || (t == 0xB0 && d1 >= 64 && d1 <= 69);
}

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

// COEUR 1, CELUI DE L'INTERRUPTION USB (MESURES §163). Elle tournait sur le
// coeur 0 (§149) : ses ecritures dans TinyUSB (dcd_edpt_xfer) prennent un
// verrou qui ne masque l'interruption que sur LEUR coeur, et le gestionnaire
// d'interruption, lui, touche aux memes registres sans verrou (le masque des
// FIFO d'emission, la FIFO du point MIDI). Le 26/09, avec un CC continu (un
// potentiometre) et le reseau du cable charge (l'app ouverte) : le reseau du
// cable mourait en quelques minutes — l'emission ne repartait plus —, ou
// l'interruption USB bouclait sans fin sur le coeur 1 jusqu'au chien de garde
// (pile relevee : handle_ep_irq). Meme regle qu'au §147 : tout ce qui ecrit
// dans le controleur USB, sur le coeur de son interruption. Priorite 19 : au
// rang du MIDI, au-dessus de l'audio (11) — elle ne fait que deposer quatre
// octets, et dort sur la file ; reveillee toutes les 20 ms seulement quand une
// reconciliation attend l'hote.
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
    if (statut < 0xF0 && estTenu(statut, d1)) s_tenuDepuisDemarrage.store(true);   // §183
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

/* ── L'ENTREE MIDI USB, TRAITEE DES QU'ELLE ARRIVE (MESURES §170) ─────────
 * Elle etait lue par update(), dans nidmi_loop() : loopTask, priorite 1, la
 * tache la moins prioritaire de la carte. Chaque geste dans l'app — ouvrir un
 * panneau, changer un script — passe par async_tcp (10), qui la preempte :
 * les notes d'un sequenceur branche en USB attendaient dans la file de TinyUSB
 * et repartaient par paquets. « La sequence saute un peu quand je touche a
 * l'UI » (28/09).
 *
 * TinyUSB appelle tud_midi_rx_cb() dans la tache usbd (coeur 1, priorite 24)
 * des qu'un transfert arrive. On y lit tout, on DATE chaque message et on le
 * depose dans NOTRE file (PSRAM, 512 messages) : quelques microsecondes, rien
 * d'autre — cette tache passe avant l'audio. MidiTask (coeur 0, 19) dort sur
 * cette file et traite chaque message des qu'il est la : scripts, son,
 * composants. La lecture de TinyUSB reste sur le coeur 1 : elle peut rearmer
 * le point d'entree, une ecriture dans le controleur (§163).
 *
 * RIEN NE SE PERD. Notre file pleine (MidiTask arretee : rechargement des
 * broches), on laisse le reste dans celle de TinyUSB, qui cesse alors
 * d'accepter les transferts : l'hote attend, il ne jette rien. MidiTask, en
 * faisant de la place, redemande la lecture a la tache usbd
 * (usbd_defer_func). Retarde, jamais perdu. */
struct EntreeUsb { uint32_t paquet; uint32_t t_us; };
constexpr UBaseType_t kCapaciteEntree = 512;
StaticQueue_t s_entreeStruct;   // en RAM interne : il porte le verrou de la file
QueueHandle_t s_entree = nullptr;
std::atomic<bool> s_relectureDue{false};
/* L'ATTENTE de chaque message, de son arrivee a son traitement : ce que le
 * sequenceur entend. Publiee par /api/diag/gigue (« entree_usb »). */
std::atomic<uint32_t> s_eMessages{0}, s_eAttenteMax{0}, s_eAttenteCumul{0},
                      s_eRetards1ms{0}, s_eRetards5ms{0}, s_eRetenues{0}, s_eFileMax{0};

// TACHE USBD SEULEMENT (tud_midi_rx_cb, ou relire() par usbd_defer_func).
void lireTinyUsb() {
    uint8_t p[4];
    while (uxQueueSpacesAvailable(s_entree) > 0 && tud_midi_n_packet_read(0, p)) {
        EntreeUsb e;
        memcpy(&e.paquet, p, 4);
        e.t_us = (uint32_t)esp_timer_get_time();
        xQueueSend(s_entree, &e, 0);   // la place vient d'etre verifiee : seul producteur
    }
    const uint32_t n = (uint32_t)uxQueueMessagesWaiting(s_entree);
    if (n > s_eFileMax.load()) s_eFileMax.store(n);
    if (uxQueueSpacesAvailable(s_entree) == 0 && tud_midi_n_available(0, 0)) {
        s_relectureDue = true;         // le reste attend chez TinyUSB, l'hote aussi
        s_eRetenues++;
    }
}

void relire(void*) { lireTinyUsb(); }

}  // namespace

/* Appele par TinyUSB (midi_device.c) a chaque transfert recu, dans la tache
 * usbd. Le symbole faible de la bibliotheque ne faisait rien. */
extern "C" void tud_midi_rx_cb(uint8_t itf) {
    (void)itf;
    if (s_entree) lireTinyUsb();
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

    if (s_entree == nullptr) {
        /* La file d'ENTREE, avant USB.begin() : un hote peut envoyer des le
         * montage. En PSRAM, comme celle de sortie ; a defaut, un quart en RAM
         * interne. Seules des taches y touchent. */
        UBaseType_t n = kCapaciteEntree;
        uint8_t* stockage = (uint8_t*)heap_caps_malloc(n * sizeof(EntreeUsb), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (stockage == nullptr) {
            n = kCapaciteEntree / 4;
            stockage = (uint8_t*)heap_caps_malloc(n * sizeof(EntreeUsb), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (stockage != nullptr) {
            s_entree = xQueueCreateStatic(n, sizeof(EntreeUsb), stockage, &s_entreeStruct);
        } else {
            NIDMI_WEB_LOG("[USB-MIDI] ERREUR : pas de memoire pour la file d'entree — le MIDI USB entrant est ignore");
        }
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
            // Coeur 1 : celui de l'interruption USB (voir « pompe », MESURES §163).
            xTaskCreateStaticPinnedToCore(pompe, "usbmidi_tx", sizeof(s_pilePompe), nullptr, 19, s_pilePompe,
                                          &s_tcbPompe, 1);
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

/* MidiTask SEULEMENT : voir « L'ENTREE MIDI USB, TRAITEE DES QU'ELLE ARRIVE ».
 * Attend au plus `attente` le premier message, puis traite tout ce qui est la.
 *
 * Format d'un paquet USB-MIDI (4 octets) :
 *   header = (cable << 4) | CIN,  CIN 0x8 = note off, 0x9 = note on,
 *                                 0xB = control change
 *   byte1  = statut (0x9n / 0x8n / 0xBn), byte2 = note/CC, byte3 = velo/valeur
 *
 * Convention MIDI respectee : un note-on de velocite 0 EST un note-off. Sans
 * ca, les claviers qui n'emettent jamais de 0x8n laissent des notes tenues pour
 * toujours. */
bool UsbMidiManager::traiterEntree(TickType_t attente) {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    if (s_entree == nullptr) {
        if (attente) vTaskDelay(attente);
        return false;
    }
    EntreeUsb e;
    bool traite = false;
    if (xQueueReceive(s_entree, &e, attente) == pdTRUE) {
        traite = true;
        do {
            const uint32_t d = (uint32_t)esp_timer_get_time() - e.t_us;
            s_eMessages++;
            s_eAttenteCumul += d;
            if (d > s_eAttenteMax.load()) s_eAttenteMax.store(d);
            if (d > 1000) s_eRetards1ms++;
            if (d > 5000) s_eRetards5ms++;
            /* Arrete depuis l'API : lu, et laisse la. L'hote ne s'engorge pas
             * pour autant. */
            if (!isStarted) continue;
            const uint32_t t0 = (uint32_t)esp_timer_get_time();
            uint8_t o[4];
            memcpy(o, &e.paquet, 4);
            const uint8_t cin   = o[0] & 0x0F;
            if (cin >= 0x8 && cin <= 0xE && estTenu(o[1], o[2])) s_tenuDepuisDemarrage.store(true);   // §183
            const uint8_t canal = (uint8_t)((o[1] & 0x0F) + 1);   // 1..16
            switch (cin) {
                case 0x9:
                    if (o[3] == 0) {
                        if (onNoteOff) onNoteOff(canal, o[2], 0);
                    } else if (onNoteOn) {
                        onNoteOn(canal, o[2], o[3]);
                    }
                    break;
                case 0x8:
                    if (onNoteOff) onNoteOff(canal, o[2], o[3]);
                    break;
                case 0xB:
                    if (onControlChange) onControlChange(canal, o[2], o[3]);
                    break;
                default:
                    break;   // horloge, SysEx, pitch bend : pas encore route
            }
            Chronos::traitementUsb.noter((uint32_t)esp_timer_get_time() - t0);
        } while (xQueueReceive(s_entree, &e, 0) == pdTRUE);
        /* Un OU par lot : la LED d'activite (§167). Tout ce qui est arrive
         * compte, route ou non — l'horloge d'un sequenceur est une entree. */
        Activite::noter(Activite::MIDI_USB);
    }
    /* De la place est revenue : la tache usbd relit ce que TinyUSB gardait. */
    if (s_relectureDue.exchange(false)) usbd_defer_func(relire, nullptr, false);
    return traite;
#else
    if (attente) vTaskDelay(attente);
    return false;
#endif
}

void UsbMidiManager::statsEntree(StatsEntree& s) {
    s = {};
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    s.messages   = s_eMessages;
    s.attenteMax = s_eAttenteMax;
    s.attenteMoy = s.messages ? (uint32_t)(s_eAttenteCumul / s.messages) : 0;
    s.retards1ms = s_eRetards1ms;
    s.retards5ms = s_eRetards5ms;
    s.retenues   = s_eRetenues;
    s.fileMax    = (uint16_t)s_eFileMax.load();
    s.capacite   = s_entree ? (uint16_t)(uxQueueMessagesWaiting(s_entree) + uxQueueSpacesAvailable(s_entree)) : 0;
#endif
}

void UsbMidiManager::reinitStatsEntree() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    s_eMessages = 0; s_eAttenteMax = 0; s_eAttenteCumul = 0;
    s_eRetards1ms = 0; s_eRetards5ms = 0; s_eRetenues = 0; s_eFileMax = 0;
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

bool UsbMidiManager::tenuDepuisDemarrage() {
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
    return s_tenuDepuisDemarrage.load();
#else
    return false;
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
