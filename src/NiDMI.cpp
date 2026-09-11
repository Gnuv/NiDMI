#include "NiDMI.h"
#include <soc/rtc_cntl_reg.h>
#include "server/ServerCore.h"
#include "managers/ComponentManager.h"
#include "utils/PinMapper.h"
#include "midi/MidiRouter.h"
#include "midi/CcMap.h"
#include "mapping/MappingEngine.h"
#include "network/UsbMidiManager.h"
#include "server/WebDebugConsole.h"
#include "Globals.h"
#include <Preferences.h>
#include <WiFi.h>
#include "audio/AudioEngine.h"
#include "mapping/CueStore.h"
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
#include <esp32-hal-tinyusb.h>
#endif

// Variables globales pour la gestion des composants
MidiRouter g_midiRouter;
ComponentManager g_componentManager;

// Configuration STA mise en cache pour permettre une reconnexion automatique
static String g_staSsid;
static String g_staPass;
static String g_staIpStr;
static String g_staGwStr;
static String g_staSnStr;

// Gestion de la reconnexion STA (backoff exponentiel pour éviter le spam série
// et les tentatives inutiles quand le réseau est durablement absent)
static unsigned long g_lastStaConnectAttempt = 0;
static const unsigned long STA_RECONNECT_BASE_MS = 10000;  // 1re tentative après 10 s
static const unsigned long STA_RECONNECT_MAX_MS  = 60000;  // plafond du backoff
static unsigned long g_staReconnectInterval = STA_RECONNECT_BASE_MS;
static bool g_staWasConnected = false;  // pour logguer les transitions STA (visibilité)

// Version du schéma de données NVS. À INCRÉMENTER quand le format stocké en NVS
// change de façon incompatible (clés/types JSON, layout des blobs mux, etc.).
// Au boot, si la version stockée diffère, la config (pins/mux/mappings) est
// réinitialisée mais le réseau (STA + mDNS) est préservé -> évite de charger
// d'anciennes données dans une nouvelle structure (crash / valeurs fausses).
static const uint32_t NIDMI_NVS_SCHEMA_VERSION = 1;

// Demande de rechargement des configs pins depuis l'API (débounce 500 ms pour grouper les sauvegardes séquentielles)
static volatile bool g_requestReloadPins = false;
static unsigned long g_reloadRequestTime = 0;
extern "C" void nidmi_requestReloadPins(){
    g_requestReloadPins = true;
    g_reloadRequestTime = millis();
}

// Redémarrage différé (depuis la loop, pas depuis le handler HTTP — évite de couper la NVS en plein écriture)
static volatile bool g_requestReboot = false;
static unsigned long g_rebootRequestTime = 0;
/* Le redemarrage etait CONFIE A LA BOUCLE. Cela marche tant que la boucle
 * tourne — mais apres un OTA, elle ne tourne plus : l'ecriture de l'image
 * laisse le coeur applicatif fige (journal : « [OTA] Image validee,
 * redemarrage... » puis plus rien, tache audio comprise), si bien que
 * ESP.restart() n'etait jamais atteint. La carte restait en vie par sa seule
 * pile reseau : elle repondait en HTTP, mais ne redemarrait pas, ne rechargeait
 * pas ses configs, et l'image fraichement ecrite n'etait jamais lancee. Il
 * fallait la debrancher.
 *
 * On confie donc le redemarrage a une TACHE DEDIEE, qui dort deux secondes —
 * le temps que la reponse HTTP parte — puis redemarre. Elle ne depend de rien
 * d'autre. La boucle garde son propre chemin pour les cas ordinaires. */
static void tacheRedemarrage(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

extern "C" void nidmi_requestReboot(){
    g_rebootRequestTime = millis();
    g_requestReboot = true;
    // Filet de securite : si la boucle est morte, cette tache redemarre quand meme.
    xTaskCreate(tacheRedemarrage, "reboot", 2048, nullptr, configMAX_PRIORITIES - 2, nullptr);
}

// Mode téléchargement (bootloader ROM), demandé par l'API. Même différé que le
// reboot : bloquer ici bloquerait async_tcp, donc la réponse HTTP ne partirait
// jamais — c'est ce qui s'était passé au premier essai.
static volatile bool g_requestDownload = false;
extern "C" void nidmi_requestDownloadMode(){
    g_rebootRequestTime = millis();
    g_requestDownload = true;
}

// Le mapping GPIO est maintenant géré par PinMapper

// Charger configuration pins depuis NVS
void loadPinConfigs() {
    // Le ComponentManager gère maintenant le chargement des configurations
    g_componentManager.reloadConfigs();
}

// Traitement des composants dans la boucle
void processComponents() {
    // Le ComponentManager gère maintenant tous les composants
    g_componentManager.update();
}

// Diagnostic touch au boot : mettre à 1 pour activer, 0 pour désactiver.
// Surchargeable au build : -DTOUCH_BOOT_DIAG=0 desactive le diagnostic.
// GPIO1 est AUSSI le BCK de l'I2S (AudioEngine.h) : touchRead(1) bascule la
// broche en mode RTC, et rien ne garantit qu'un i2s.begin() ulterieur la
// reprenne. Piste testee pour le blocage de la tache audio.
// 0 PAR DEFAUT : le diagnostic sonde GPIO1 (T1) au boot, or GPIO1 est le BCK de
// l'I2S sur la maquette audio. touchRead() capture la broche au peripherique
// tactile et l'I2S ne peut plus la cadencer -> tache audio figee (MESURES.md
// §19). Le remede par touch_pad_deinit() crashait (conflit legacy/new driver) :
// on NE SONDE simplement PAS. Reactiver avec -DTOUCH_BOOT_DIAG=1 sur une carte
// sans audio sur ces broches.
#ifndef TOUCH_BOOT_DIAG
#define TOUCH_BOOT_DIAG 0
#endif

static void touchDiag(const char* label) {
#if TOUCH_BOOT_DIAG && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32S3))
    Serial.printf("[TOUCH DIAG] %s: ", label);
    for (int i = 0; i < 5; i++) {
        uint32_t v = touchRead(1); // GPIO1 = T1
        Serial.printf("%lu ", (unsigned long)v);
        delay(50);
    }
    Serial.println();
#else
    (void)label;
#endif
}

void nidmi_begin() {
    /* AVANT tout chargement : lire la phase laissee par la vie precedente,
     * sinon le premier marquer() de ce demarrage l'ecraserait. */
    ComponentManager::capturerPhasePrecedente();
    Serial.begin(115200);
    delay(50);

    touchDiag("AVANT tout (juste apres Serial)");

    // Ne pas laisser la pile WiFi relire/écrire une config STA/AP dans la NVS système
    // (notre SSID AP et STA viennent du namespace Preferences "nidmi").
    WiFi.persistent(false);

    // Détecter et afficher le MCU
    PinMapper::detectMcu();
    PinMapper::printMappings();

    // Nettoyer les anciens réglages NVS si nécessaire
    // (décommentez la ligne suivante pour forcer le reset)
    // Preferences::clear("nidmi\n\n");

    // Lire nom serveur + STA depuis NVS (ne pas appeler get* si begin a échoué)
    Preferences preferences;
    String serverName = "nidmi";
    g_staSsid = "";
    g_staPass = "";
    g_staIpStr = "";
    g_staGwStr = "";
    g_staSnStr = "";
    bool touchEnabled = false;
    uint32_t storedSchema = 0;
    const bool usbMidiEnabled = nidmi_usb_midi_enabled_at_compile_time();

    if (preferences.begin("nidmi", true)) {
        serverName = preferences.getString("mdns_name", "nidmi");
        g_staSsid = preferences.getString("sta_ssid", "");
        g_staPass = preferences.getString("sta_pass", "");
        g_staIpStr = preferences.getString("sta_ip", "");
        g_staGwStr = preferences.getString("sta_gw", "");
        g_staSnStr = preferences.getString("sta_sn", "");
        touchEnabled = preferences.getBool("touch_enabled", false);
        storedSchema = preferences.getUInt("nvs_schema", 0);
        preferences.end();
    } else {
        Serial.println("[NiDMI] ERREUR: ouverture NVS en lecture échouée - NVS peut être corrompue");
        Serial.println("[NiDMI] Utilisation des valeurs par défaut. Flashez nidmi_clear_nvs pour réinitialiser.");
    }
    
    // Nettoyer le nom serveur (supprimer caractères invalides pour SSID WiFi)
    serverName.trim();  // Supprimer espaces en début/fin
    // Supprimer les caractères de contrôle et caractères invalides
    serverName.replace("\n", "");
    serverName.replace("\r", "");
    serverName.replace("\t", "");
    if (serverName.length() == 0) serverName = "nidmi";
    
    // Sauvegarder le nom mDNS dans NVS pour RTP-MIDI (seulement si NVS ouvre en écriture)
    if (preferences.begin("nidmi", false)) {
        // Garde de schéma NVS : si le format stocké diffère du firmware (mise à jour
        // incompatible), on réinitialise la config (pins/mux/mappings/osc) mais on
        // PRÉSERVE le réseau (STA + mDNS) -> la carte reste joignable, et on ne charge
        // jamais d'anciennes données dans une nouvelle structure.
        if (storedSchema == 0) {
            // Aucune version stockée : appareil existant déjà au format courant (ou NVS vierge).
            // On tamponne la version SANS rien effacer -> les réglages existants sont conservés.
            // (À la 1re introduction de la garde, le format courant EST le schéma v1.)
            preferences.putUInt("nvs_schema", NIDMI_NVS_SCHEMA_VERSION);
        } else if (storedSchema != NIDMI_NVS_SCHEMA_VERSION) {
            // Version connue mais différente -> format NVS incompatible (vrai changement, ex. v1->v2) :
            // on réinitialise la config (pins/mux/mappings/osc) mais on PRÉSERVE le réseau.
            Serial.printf("[NiDMI] Schéma NVS stocké=%u attendu=%u -> reset config (réseau préservé)\n",
                          (unsigned)storedSchema, (unsigned)NIDMI_NVS_SCHEMA_VERSION);
            preferences.clear();
            if (g_staSsid.length() > 0)  preferences.putString("sta_ssid", g_staSsid);
            if (g_staPass.length() > 0)  preferences.putString("sta_pass", g_staPass);
            if (g_staIpStr.length() > 0) preferences.putString("sta_ip", g_staIpStr);
            if (g_staGwStr.length() > 0) preferences.putString("sta_gw", g_staGwStr);
            if (g_staSnStr.length() > 0) preferences.putString("sta_sn", g_staSnStr);
            preferences.putUInt("nvs_schema", NIDMI_NVS_SCHEMA_VERSION);
        }
        // storedSchema == NIDMI_NVS_SCHEMA_VERSION -> rien à faire (déjà à jour)
        preferences.putString("mdns_name", serverName);
        preferences.putString("rtp_name", serverName);
        preferences.end();
    }
    
    Serial.println("[NiDMI] Names synchronized:");
    Serial.printf("  SSID: %s\n", serverName.c_str());
    Serial.printf("  mDNS: %s.local\n", serverName.c_str());

    const char* apSsid = serverName.c_str();
    const char* apPass = "nidmipass";
    const char* host   = serverName.c_str();

    touchDiag("AVANT WiFi/serveur");

    // Démarre l’AP : AP seul si aucun STA en NVS (évite soucis d’association client en APSTA « vide »)
    NIDMI_WEB_LOG("[MEM] avant WiFi: %d\n", (int)ESP.getFreeHeap());
    serverCore.begin(apSsid, apPass, host, g_staSsid.length() == 0);
    NIDMI_WEB_LOG("[MEM] apres WiFi+serveur: %d\n", (int)ESP.getFreeHeap());

    touchDiag("APRES WiFi/serveur");

    // Tente STA après que le mode APSTA soit configuré
    if (g_staSsid.length() > 0) {
        if (g_staIpStr.length() > 0 && g_staGwStr.length() > 0 && g_staSnStr.length() > 0) {
            IPAddress ip, gw, sn;
            if (ip.fromString(g_staIpStr) && gw.fromString(g_staGwStr) && sn.fromString(g_staSnStr)) {
                serverCore.setStaticStaIp(ip, gw, sn);
                Serial.printf("[NiDMI] STA static IP: %s GW: %s SN: %s\n", g_staIpStr.c_str(), g_staGwStr.c_str(), g_staSnStr.c_str());
            }
        }
        serverCore.connectSta(g_staSsid.c_str(), g_staPass.length() > 0 ? g_staPass.c_str() : nullptr);
    } else {
        Serial.println("[NiDMI] No STA configuration found");
    }
    
    // USB-MIDI : NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME dans UsbMidiManager.h (pas NVS)
    g_midiRouter.enableUsbMidi(usbMidiEnabled);

    // Initialiser MidiRouter (qui initialisera USB MIDI si activé et supporté)
    g_midiRouter.begin();

    /* Script .nms memorise : la carte se reconfigure SEULE au demarrage. C'est
       la condition du headless — une carte deployee n'a pas de navigateur pour
       lui redire quoi faire. Le NOM vient de la NVS, le CONTENU de mapfs. */
    g_midiRouter.restaurerScript();
    // La table CC -> parametre revient elle aussi de la NVS : sans elle, un
    // redemarrage rendait muets tous les potentiometres appris.
    CcMap::monter();

    /* print() d'un .nms : au journal ET vers l'app.
     * En headless, le moteur du navigateur n'execute plus rien — son
     * _msePrintLog n'est donc jamais appele, et la console de la zone I/O
     * restait vide alors que le script tournait tres bien. La carte pousse
     * maintenant une trame « NMS_PRINT:<etiquette>\x1f<valeur> » que l'app
     * affiche dans la console du bloc concerne. */
    MappingEngine::surImpression([](const char* origine, const char* etiquette,
                                   float valeur, bool graphe) {
        /* L'ORIGINE D'ABORD. La trame ne portait que l'etiquette et la valeur :
         * l'app ne pouvait donc pas savoir QUI avait imprime, et attribuait
         * tout au bloc map dont le script tourne sur la carte — les print() des
         * broches compris. Trois champs desormais : « pin:5 », « map:2 » ou
         * « essai », puis l'etiquette, puis la valeur. */
        const char* org = (origine && origine[0]) ? origine : "?";
        char trame[112];
        /* ⚠️ CE RAPPEL S'EXECUTE DANS MidiTask, SUR LE COEUR 0. Il ne doit donc
         * PAS toucher a la WebSocket : cleanupClients() efface la liste de
         * clients depuis loopTask, et l'iterer d'ici est un acces a de la
         * memoire rendue (ServerCore.h). On POUSSE dans une file, loopTask
         * draine. Le compteur, lui, se lit sans risque de partout.
         * Un graphe n'a pas d'historique : s'il ne part pas il n'existe pas, on
         * sort donc AVANT de formater — en headless ce chemin ne coute rien. */
        if (graphe && !nidmi_ws_quelqu_un_ecoute()) return;
        if (graphe) {
            /* PAS dans le journal texte : c'est tout l'objet de graph(). Une
             * valeur continue qui defile en chiffres noie le journal — vingt
             * lignes par seconde pour un potentiometre qui tremble. */
            snprintf(trame, sizeof(trame), "NMS_GRAPH:%s\x1f%s\x1f%.4f",
                     org, etiquette, valeur);
        } else {
            /* Le journal de bord AVANT la garde, et toujours : c'est
             * l'historique qu'un client rejouera en s'abonnant plus tard. Une
             * trace qu'on n'emet pas n'est pas une trace qu'on efface. */
            NIDMI_WEB_LOG("[%s] %s : %.4f", org, etiquette, valeur);
            snprintf(trame, sizeof(trame), "NMS_PRINT:%s\x1f%s\x1f%.4f",
                     org, etiquette, valeur);
        }
        /* Personne n'ecoute : on s'arrete la. Sinon on POUSSE — file pleine =
         * le client ne suit pas, la trame est jetee sans bloquer cette tache.
         * Empiler pour ne rien perdre, c'est perdre tout. */
        if (!nidmi_ws_quelqu_un_ecoute()) return;
        nidmi_ws_pousser(trame);
    });

    /* MIDI USB ENTRANT -> moteur audio.
       Le port USB de la carte etait uniquement SORTANT : un clavier ou un DAW
       branche dessus n'avait aucun effet (UsbMidiManager::update() ne lisait
       rien). On branche donc la reception sur les memes deux destinations que
       le MIDI RTP : les composants (LEDs appairees) ET le moteur audio, pour
       que la carte SONNE quand on la joue de l'exterieur.
       Note : la porte de silence reste maitresse — sans PLAY, ces notes
       n'atteignent pas le DAC (MESURES.md §19). */
    serverCore.usbMidi().setMidiInputHooks(
        [](uint8_t ch, uint8_t note, uint8_t vel) { g_midiRouter.noteEntrante(ch, note, vel, false); },
        [](uint8_t ch, uint8_t note, uint8_t vel) { g_midiRouter.noteEntrante(ch, note, vel, true); },
        [](uint8_t ch, uint8_t cc, uint8_t val) { g_midiRouter.ccEntrant(ch, cc, val); }
    );
    NIDMI_WEB_LOG("[MEM] apres MidiRouter: %d\n", (int)ESP.getFreeHeap());
    
    // Initialiser RTP-MIDI
    serverCore.rtpMidi().begin(serverName.c_str());
    serverCore.rtpMidi().setMidiInputHooks(
        [](uint8_t ch, uint8_t note, uint8_t vel) { g_componentManager.handleMidiNoteOn(ch, note, vel); },
        [](uint8_t ch, uint8_t note, uint8_t vel) { g_componentManager.handleMidiNoteOff(ch, note, vel); },
        [](uint8_t ch, uint8_t cc,   uint8_t val) { g_midiRouter.ccEntrant(ch, cc, val); }
    );
    NIDMI_WEB_LOG("[MEM] apres RTP-MIDI: %d\n", (int)ESP.getFreeHeap());
    
    // Initialiser Bluetooth MIDI
    serverCore.bluetooth().begin(serverName.c_str());
    NIDMI_WEB_LOG("[MEM] apres Bluetooth: %d\n", (int)ESP.getFreeHeap());
    
    touchDiag("AVANT ComponentManager.begin");

    // Initialiser ComponentManager
    g_componentManager.begin(&g_midiRouter);
    NIDMI_WEB_LOG("[MEM] apres ComponentManager: %d\n", (int)ESP.getFreeHeap());

    touchDiag("APRES ComponentManager.begin (MuxTask+MidiTask demarres)");
    
    Serial.println("[NiDMI] Ready");
    NIDMI_WEB_LOG("[NiDMI] Ready (console web dispo sur S3 si activée)");
    Serial.print("  AP SSID: "); Serial.println(apSsid);
    Serial.print("  AP PASS: "); Serial.println(apPass);
    Serial.print("  AP IP: "); Serial.println(WiFi.softAPIP());
    Serial.print("  mDNS: http://"); Serial.print(host); Serial.println(".local/");
    Serial.print("  RTP-MIDI: "); Serial.println(serverCore.rtpMidi().isReady() ? "Initialized" : "Failed");
    Serial.print("  Bluetooth: "); Serial.println(serverCore.bluetooth().isInitialized() ? "Initialized" : "Failed");
    Serial.printf("Touch Enabled: %s\n", touchEnabled ? "true" : "false");
    Serial.println();
}

void nidmi_loop() {
    /* Chargement du process audio mémorisé, sur un tas encore vierge : c'est
       l'ordre d'allocation qui décide (MESURES.md §15), et trois secondes après
       le boot on est très loin devant l'ouverture d'un navigateur.

       Note honnête : ce déplacement de setup() vers loop() avait été fait en
       poursuivant un blocage de la tâche audio, sur l'hypothèse que le contexte
       d'appel comptait. C'ÉTAIT FAUX — la cause était le diagnostic tactile qui
       laissait GPIO1 (le BCK) au périphérique de touch (voir AudioEngine.cpp).
       L'appel est resté ici parce qu'il y est correct et qu'il laisse setup()
       se terminer, pas parce que setup() poserait un problème. */
    static bool audioRestaure = false;
    if (!audioRestaure && millis() > 3000) {
        audioRestaure = true;
        AudioEngine::restaurerAuBoot();
    }

    AudioEngine::entretienBoot();   // écrit la NVS hors du contexte async
    Cues::boucle();                 // avance les cues minutées — la carte tient son propre temps

    // Redémarrage différé (laisse le temps à la réponse HTTP et à la NVS de se fermer proprement)
    if (g_requestDownload && (millis() - g_rebootRequestTime >= 2000)) {
        REG_WRITE(RTC_CNTL_OPTION1_REG, 0x1);   // force_download_boot
        ESP.restart();
    }
    if (g_requestReboot && (millis() - g_rebootRequestTime >= 2000)) {
        ESP.restart();
    }

    // Tentative de reconnexion STA automatique si des identifiants sont connus.
    // connectSta() est non bloquant : on se contente de relancer WiFi.begin() et
    // d'espacer les tentatives via un backoff (10 s -> 60 s) remis à zéro une fois connecté.
    if (g_staSsid.length() > 0) {
        wl_status_t staStatus = WiFi.status();
        unsigned long now = millis();
        if (staStatus == WL_CONNECTED) {
            if (!g_staWasConnected) {
                g_staWasConnected = true;
                NIDMI_WEB_LOG("[NiDMI] STA connectée, IP: %s", WiFi.localIP().toString().c_str());
            }
            g_staReconnectInterval = STA_RECONNECT_BASE_MS;
        } else {
            if (g_staWasConnected) {
                g_staWasConnected = false;
                NIDMI_WEB_LOG("[NiDMI] STA déconnectée");
            }
            if (now - g_lastStaConnectAttempt >= g_staReconnectInterval) {
                NIDMI_WEB_LOG("[NiDMI] STA non connecté, reconnexion auto (backoff %lus)...", g_staReconnectInterval / 1000);
                // Reconfigurer éventuellement l'IP statique
                if (g_staIpStr.length() > 0 && g_staGwStr.length() > 0 && g_staSnStr.length() > 0) {
                    IPAddress ip, gw, sn;
                    if (ip.fromString(g_staIpStr) && gw.fromString(g_staGwStr) && sn.fromString(g_staSnStr)) {
                        serverCore.setStaticStaIp(ip, gw, sn);
                    }
                }
                serverCore.connectSta(g_staSsid.c_str(), g_staPass.length() > 0 ? g_staPass.c_str() : nullptr);
                g_lastStaConnectAttempt = now;
                g_staReconnectInterval *= 2;
                if (g_staReconnectInterval > STA_RECONNECT_MAX_MS) g_staReconnectInterval = STA_RECONNECT_MAX_MS;
            }
        }
    }

    serverCore.update();
    
    // Recharger pins si demandé (débounce 500 ms pour grouper les sauvegardes séquentielles)
    if (g_requestReloadPins && (millis() - g_reloadRequestTime >= 500)) {
        g_requestReloadPins = false;
        g_componentManager.reloadConfigs();
    }
    
    processComponents();
    /* Le rattrapage de la console web : une ligne par tour, hors du rappel
     * WebSocket (voir WebDebugConsole.cpp). */
    /* LA SEULE FENETRE ou l'on ecrit sur la WebSocket : loopTask, la meme tache
     * que ws.cleanupClients() de serverCore.update(). Tout le reste du firmware
     * POUSSE dans la file. Voir ServerCore.h. */
    nidmi_ws_drainer();
    nidmi_web_debug_pump();
}

// Instance globale
NiDMIServer nidmi;

// Implémentation de l'interface publique
void NiDMIServer::begin() {
    nidmi_begin();
}

void NiDMIServer::loop() {
    nidmi_loop();
}
