#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "ServerCore.h"
#include "../Globals.h"
#include "../midi/MidiRouter.h"      // emettreRtp : le RTP sortant, emis par la boucle (§175)
#include "../audio/AudioEngine.h"
#include "WebDebugConsole.h"
#include "../network/UsbNetBootstrap.h"   // parLeCable : le cable ou le WiFi, pour chaque onglet
#include <ESPmDNS.h>
#include <mdns.h>
#include <esp_netif.h>
#include <Preferences.h>
// setupWebAPI est déclaré plus bas et défini dans WebAPI.cpp

// Déclaration de la fonction setupHttp définie dans WebAPI.cpp
void setupWebAPI(AsyncWebServer& server, AsyncWebSocket& ws);

// Instance globale
ServerCore serverCore;

ServerCore::ServerCore()
    : server(80), ws("/ws") {}

/* ── ALLUMER LA RADIO, SEPAREMENT ──────────────────────────────────────────
 * Extrait de begin() sans rien changer a son contenu : meme mode, meme
 * puissance, meme IP d'AP, meme canal, meme delai de 100 ms.
 *
 * Idempotent. Le repli de nidmi_loop() appelle sans savoir, et un second appel
 * ne doit pas reconfigurer un AP qui sert deja des clients. */
static SemaphoreHandle_t verrouRadio() {
    static SemaphoreHandle_t v = xSemaphoreCreateRecursiveMutex();
    return v;
}

ServerCore::LectureRadio::LectureRadio(TickType_t attente)
    : pris(xSemaphoreTakeRecursive(verrouRadio(), attente) == pdTRUE) {}

ServerCore::LectureRadio::~LectureRadio() {
    if (pris) xSemaphoreGiveRecursive(verrouRadio());
}

bool ServerCore::LectureRadio::allumee() const {
    return pris && serverCore.radioWifiAllumee();
}

void ServerCore::demarrerRadioWifi() {
    if (radioAllumee) return;
    LectureRadio transition(portMAX_DELAY);   // voir « LE VERROU RADIO »

    /* Sans STA enregistré : AP seul (WIFI_AP). APSTA avec interface STA inactive peut provoquer
     * échecs ou boucles « mot de passe » / reconnexion sur téléphones (notamment ESP32-C3/S3). */
    /* ALLUMEE SEULEMENT SI LE PILOTE A DEMARRE. Rallumee en marche, la radio
     * peut manquer de memoire (esp_wifi_init rend ESP_ERR_NO_MEM, WiFi.mode
     * rend faux) : se dire allumee rendrait l'appel suivant muet, et le WiFi
     * ne reviendrait jamais. Faux reste faux : l'appelant reessaie. */
    /* MESURE (tache « rallumage sans effet sur le son ») : le pire bloc audio
     * pendant chaque etape. */
    struct Etape { const char* nom; uint32_t us; uint32_t audio; };
    Etape etapes[5]; int ne = 0;
    AudioEngine::pireBlocEtRaz();
    uint32_t te = micros();
    auto noter = [&](const char* nom) {
        const uint32_t now = micros();
        etapes[ne++] = { nom, now - te, AudioEngine::pireBlocEtRaz() };
        te = micros();
    };
    const bool ok = apOnlyRetenu ? WiFi.mode(WIFI_MODE_AP) : WiFi.mode(WIFI_MODE_APSTA);
    noter("mode");
    if (!ok) {
        Serial.println("[ServerCore] WiFi: le pilote n'a pas demarre (memoire ?)");
        return;
    }
    radioAllumee = true;
    if (apOnlyRetenu) Serial.println("[ServerCore] WiFi: mode AP uniquement");

    // Augmenter la puissance WiFi pour XIAO_ESP32C3
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // Puissance maximale
    noter("txpower");

    /* Configurer l'IP de l'AP explicitement à 192.168.4.1 */
    IPAddress apIp = IPAddress(192, 168, 4, 1);
    IPAddress apGateway = IPAddress(192, 168, 4, 1);
    IPAddress apSubnet = IPAddress(255, 255, 255, 0);
    WiFi.softAPConfig(apIp, apGateway, apSubnet);
    noter("apconfig");

    /* Canal 1 : meilleure compatibilité avec les clients 2,4 GHz */
    WiFi.softAP(apSsidRetenu.c_str(), apPassRetenu.c_str(), 1);
    noter("softap");

    /* Attendre que l'AP soit prêt avant de continuer */
    delay(100);
    noter("delay");
    String j;
    for (int i = 0; i < ne; i++) {
        char l[64];
        snprintf(l, sizeof l, "%s %lu ms (audio %lu.%lu ms) ", etapes[i].nom,
                 (unsigned long)(etapes[i].us / 1000), (unsigned long)(etapes[i].audio / 1000),
                 (unsigned long)((etapes[i].audio % 1000) / 100));
        j += l;
    }
    NIDMI_WEB_LOG("[radio] rallumage : %s", j.c_str());
}

/* ── COUPER LA RADIO, EN MARCHE ────────────────────────────────────────────
 * WiFi.mode(WIFI_OFF) va jusqu'au bout : esp_wifi_stop(), puis destruction des
 * interfaces AP et STA, puis esp_wifi_deinit() — la memoire du pilote est
 * RENDUE (WiFiGeneric.cpp, wifiLowLevelDeinit). L'interface USB, elle, porte
 * sa propre cle et n'est pas touchee ; esp_netif et la boucle d'evenements,
 * globaux, restent en place.
 *
 * Jamais appelee au demarrage : seulement sur commande, par nidmi_loop(), et
 * seulement quand un lien USB peut prendre le relais. */
/* Le mDNS sur les deux interfaces WiFi, oui ou non. Appele depuis loopTask, la
 * seule tache qui detruit ces interfaces : leurs poignees sont sures ici. */
void ServerCore::mdnsInterfacesWifi(bool actives) {
    const mdns_event_actions_t a = actives
        ? (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4)
        : (mdns_event_actions_t)(MDNS_EVENT_DISABLE_IP4 | MDNS_EVENT_DISABLE_IP6);
    for (const char* cle : { "WIFI_STA_DEF", "WIFI_AP_DEF" }) {
        esp_netif_t* n = esp_netif_get_handle_from_ifkey(cle);
        if (n && (!actives || esp_netif_is_netif_up(n))) mdns_netif_action(n, a);
    }
}

bool ServerCore::coupureSure() {
    if (!radioAllumee) return true;
    const unsigned long now = millis();
    coupureDemandeeA = now;
    // Pas preparee, ou un evenement reseau depuis : (re)desactiver, et attendre.
    if (!coupurePrepareeA || (long)(dernierEvenementReseauA - coupurePrepareeA) >= 0) {
        mdnsInterfacesWifi(false);
        coupurePrepareeA = now ? now : 1;
        return false;
    }
    return now - coupurePrepareeA >= 1500;
}

void ServerCore::couperRadioWifi() {
    if (!radioAllumee) return;
    LectureRadio transition(portMAX_DELAY);   // voir « LE VERROU RADIO »
    radioAllumee = false;                     // avant : un lecteur qui attend lira « coupee »
    WiFi.mode(WIFI_OFF);
    coupurePrepareeA = 0;
}

void ServerCore::begin(const char* apSsid, const char* apPass, const char* hostname, bool apOnlyMode) {
    nidmi_ws_file_init();   // avant tout client : voir ServerCore.h
    /* Événements WiFi : visibilité des drops STA (avec la RAISON, indisponible par polling)
     * et de l'obtention d'IP. Log Serial uniquement — pas d'accès WebSocket depuis la tâche
     * event WiFi, pour éviter les races avec la tâche serveur (AsyncWebSocket). */
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info){
        if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED || event == ARDUINO_EVENT_WIFI_STA_GOT_IP ||
            event == ARDUINO_EVENT_WIFI_AP_START || event == ARDUINO_EVENT_WIFI_STA_START)
            serverCore.noterEvenementReseau();   // une coupure preparee recommence
        if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            // L'adresse de l'EVENEMENT, pas WiFi.localIP() : pas de lecture
            // d'interface depuis cette tache (voir « LE VERROU RADIO »).
            Serial.printf("[WiFi] STA got IP: %s\n",
                          IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
        } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            Serial.printf("[WiFi] STA déconnectée (raison=%d)\n", (int)info.wifi_sta_disconnected.reason);
        }
    });

    /* Les parametres de l'AP sont retenus pour qu'on puisse RALLUMER la radio
     * en marche, apres une coupure sur commande (voir couperRadioWifi()).
     * Le demarrage, lui, l'allume TOUJOURS : c'est le chemin qui marche, et on
     * n'y touche plus (MESURES §142). */
    apSsidRetenu = apSsid ? apSsid : "";
    apPassRetenu = apPass ? apPass : "";
    apOnlyRetenu = apOnlyMode;
    demarrerRadioWifi();
    IPAddress apIp = WiFi.softAPIP();
    
    Serial.begin(115200);
    Serial.println();
    Serial.println("[ServerCore] AP up");
    Serial.print("  SSID: "); Serial.println(apSsid);
    Serial.print("  PASS: "); Serial.println(apPass);
    Serial.print("  AP IP: "); Serial.println(apIp);

    // Configuration mDNS - Détecter le mode automatiquement
    bool staConnected = (WiFi.status() == WL_CONNECTED);
    // Serial.printf("[ServerCore] Starting mDNS for %s mode...\n", staConnected ? "STA" : "AP");
    if (staConnected) {
        // Serial.printf("[ServerCore] STA IP: %s\n", WiFi.localIP().toString().c_str());
    }
    // Serial.print("[ServerCore] Starting mDNS with hostname: "); Serial.println(hostname);
    
    // Essayer plusieurs noms mDNS
    /* mDNS ne repond pas a la question AAAA faute d'adresse IPv6, ce qui coute
     * cinq secondes de resolution par connexion sur macOS (MESURES.md §33).
     * Le remede — WiFi.enableIPv6(true) — a ete RETIRE : il touche la pile
     * reseau, et celle-ci est tombee (ni WiFi ni point d'acces) dans la session
     * qui a suivi. Rien ne prouve qu'il en soit la cause, mais il apportait un
     * CONFORT (taper nidmi.local) contre un risque sur une fonction VITALE.
     * On accede donc par adresse IP. A reintroduire seul, et a eprouver pour
     * lui-meme, quand le reste sera stable. */

    // Réduire le nombre de String simultanées - utiliser const char* au lieu de tableau String
    const char* mdnsNames[] = {hostname, "nidmi"};
    bool mdnsOk = false;
    const char* workingName = nullptr;
    
    for (int i = 0; i < 2; i++) { // Seulement 2 noms, pas 4
        // Serial.print("[ServerCore] Trying mDNS name: "); Serial.println(mdnsNames[i]);
        if (MDNS.begin(mdnsNames[i])) {
            MDNS.addService("http", "tcp", 80);
            mdnsOk = true;
            workingName = mdnsNames[i];
            // Serial.print("[ServerCore] mDNS success: http://"); Serial.print(workingName); Serial.println(".local/");
            break;
        } else {
            Serial.print("[ServerCore] mDNS failed for: "); Serial.println(mdnsNames[i]);
        }
        delay(500);
    }
    
    if (mdnsOk) {
        // Serial.println("[ServerCore] HTTP service registered");
        
        // mDNS configuré avec succès
        // Serial.println("[ServerCore] mDNS configuration complete");
        
        // Debug mDNS - limiter le nombre de String simultanées en les créant dans des blocs
        Serial.println("[ServerCore] mDNS Debug Info:");
        Serial.printf("  Hostname: %s\n", workingName);
        Serial.printf("  STA Connected: %s\n", staConnected ? "Yes" : "No");
        if (staConnected) {
            // Créer les String une par une pour éviter l'accumulation sur la pile
            {
                String staIp = WiFi.localIP().toString();
                Serial.printf("  STA IP: %s\n", staIp.c_str());
            }
            {
                String staGw = WiFi.gatewayIP().toString();
                Serial.printf("  STA Gateway: %s\n", staGw.c_str());
            }
            {
                String staSn = WiFi.subnetMask().toString();
                Serial.printf("  STA Subnet: %s\n", staSn.c_str());
            }
        }
        {
            String apIp = WiFi.softAPIP().toString();
            Serial.printf("  AP IP: %s\n", apIp.c_str());
        }
        Serial.printf("  WiFi Mode: %d\n", WiFi.getMode());
        
        if (staConnected) {
            // Serial.printf("[ServerCore] Access via: http://%s.local/ or http://%s/\n", workingName.c_str(), WiFi.localIP().toString().c_str());
        } else {
            // Serial.printf("[ServerCore] Access via: http://%s.local/ or http://%s/\n", workingName.c_str(), WiFi.softAPIP().toString().c_str());
        }
        // Serial.println("[ServerCore] Note: mDNS resolution may take a few seconds to propagate");
        // Serial.println("[ServerCore] If .local doesn't work, use direct IP address");
    } else {
        Serial.println("[ServerCore] All mDNS attempts failed - using direct IP only");
        if (staConnected) {
            Serial.printf("[ServerCore] Use direct IP: http://%s/\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.printf("[ServerCore] Use direct IP: http://%s/\n", WiFi.softAPIP().toString().c_str());
        }
    }
    
    // Configuration des endpoints HTTP
    setupWebAPI(server, ws);
    server.begin();
    // Serial.println("[ServerCore] HTTP server started on / (Async)");
    // Serial.println("[ServerCore] ServerCore initialization complete!");
}

void ServerCore::connectSta(const char* staSsid, const char* staPass) {
    Serial.printf("[ServerCore] STA: démarrage connexion à %s (non bloquant)\n", staSsid);

    if (useStaticSta) {
        Serial.printf("[ServerCore] Using static IP: %s\n", staIp.toString().c_str());
        WiFi.config(staIp, staGw, staSn);
    }

    /* Démarrage non bloquant : on lance l'association et on rend la main.
     * L'ancienne boucle d'attente (20 x 500 ms) figeait la boucle Core 1
     * — serveur web et nettoyage WebSocket — jusqu'à 10 s à chaque tentative,
     * y compris pendant les reconnexions appelées depuis nidmi_loop().
     * Le suivi de l'état de connexion se fait désormais dans la boucle
     * (et via les événements WiFi). */
    WiFi.begin(staSsid, staPass);
}

void ServerCore::setStaticStaIp(IPAddress ip, IPAddress gateway, IPAddress subnet) {
    useStaticSta = true; 
    staIp = ip; 
    staGw = gateway; 
    staSn = subnet;
}

void nidmi_chrono(const char* nom, uint32_t t0);   // NiDMI.cpp : le chrono de la boucle
uint32_t nidmi_section(const char* nom);

void ServerCore::update() {
    // Une coupure preparee puis delaissee (plus demandee depuis 3 s) : le mDNS
    // revient sur les interfaces WiFi, qui restent allumees.
    if (coupurePrepareeA && radioAllumee && millis() - coupureDemandeeA > 3000) {
        coupurePrepareeA = 0;
        mdnsInterfacesWifi(true);
    }
    /* Plus de ws.cleanupClients() : elle modifiait la liste de clients de la
     * bibliotheque depuis cette tache, pendant qu'async_tcp la modifiait de la
     * sienne (voir « LES ONGLETS », ServerCore.h, MESURES §171). */
    uint32_t tc = nidmi_section("rtpmidi");
    g_midiRouter.emettreRtp();      // les envois deposes par les autres taches (§175)
    rtpMidiInstance.update();
    nidmi_chrono("rtpmidi", tc); tc = nidmi_section("bluetooth");
    bluetoothInstance.update();
    nidmi_chrono("bluetooth", tc);
    /* Le MIDI USB n'est plus lu ici : MidiTask le traite des qu'il arrive
     * (UsbMidiManager, MESURES §170). */
}

void ServerCore::reconfigureMdns(const char* hostname) {
    Serial.println("[ServerCore] Reconfiguring mDNS for STA mode...");
    
    // Arrêter mDNS existant
    MDNS.end();
    delay(1000);
    
    // Vérifier si STA est connecté
    bool staConnected = (WiFi.status() == WL_CONNECTED);
    if (!staConnected) {
        Serial.println("[ServerCore] STA not connected, skipping mDNS reconfiguration");
        return;
    }
    
    Serial.printf("[ServerCore] STA IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.println("[ServerCore] Starting mDNS for STA mode...");
    Serial.print("[ServerCore] Starting mDNS with hostname: "); Serial.println(hostname);
    
    // Essayer plusieurs noms mDNS
    String mdnsNames[] = {hostname, "nidmi"};
    bool mdnsOk = false;
    String workingName = "";
    
    for (int i = 0; i < 2; i++) { // 2 noms (corrige une lecture hors limites: tableau de 2)
        // Serial.print("[ServerCore] Trying mDNS name: "); Serial.println(mdnsNames[i]);
        if (MDNS.begin(mdnsNames[i].c_str())) {
            MDNS.addService("http", "tcp", 80);
            mdnsOk = true;
            workingName = mdnsNames[i];
            // Serial.print("[ServerCore] mDNS success: http://"); Serial.print(workingName); Serial.println(".local/");
            break;
        } else {
            Serial.print("[ServerCore] mDNS failed for: "); Serial.println(mdnsNames[i]);
        }
        delay(500);
    }
    
    if (mdnsOk) {
        // Serial.println("[ServerCore] HTTP service registered");
        Serial.printf("[ServerCore] mDNS service: %s.local:80\n", workingName.c_str());
        
        // mDNS configuré avec succès
        // Serial.println("[ServerCore] mDNS configuration complete");
        if (staConnected) {
            // Serial.printf("[ServerCore] Access via: http://%s.local/ or http://%s/\n", workingName.c_str(), WiFi.localIP().toString().c_str());
        } else {
            // Serial.printf("[ServerCore] Access via: http://%s.local/ or http://%s/\n", workingName.c_str(), WiFi.softAPIP().toString().c_str());
        }
        // Serial.println("[ServerCore] Note: mDNS resolution may take a few seconds to propagate");
    } else {
        Serial.println("[ServerCore] All mDNS attempts failed - using direct IP only");
        if (staConnected) {
            Serial.printf("[ServerCore] Use direct IP: http://%s/\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.printf("[ServerCore] Use direct IP: http://%s/\n", WiFi.softAPIP().toString().c_str());
        }
    }
}

AsyncWebServer& ServerCore::web() { 
    return server; 
}

AsyncWebSocket& ServerCore::websocket() { 
    return ws; 
}

nidmi_core::RtpMidiService& ServerCore::rtpMidi() {
    return rtpMidiInstance;
}

BluetoothManager& ServerCore::bluetooth() { 
    return bluetoothInstance; 
}

UsbMidiManager& ServerCore::usbMidi() { 
    return usbMidiInstance; 
}

/* ── La file de sortie ─────────────────────────────────────────────────────
 * Pas un octet pris au TAS INTERNE, dont le plus gros bloc contigu decide si
 * AsyncTCP peut encore recevoir une image OTA : 24 x 216 = 5,2 ko, en PSRAM —
 * ils etaient en .bss, donc retires a ce meme tas avant le demarrage (MESURES
 * §152). Seules des taches y touchent, jamais cache coupe ; la structure de la
 * file, qui porte son verrou, reste en RAM interne. Sans PSRAM, le tas interne. */
namespace {
    constexpr size_t   kTrameMax   = 216;   // « DEBUG_LOG: » + 200 = le pire cas
    constexpr UBaseType_t kFileLen = 24;
    /* `pour` : 0 = tous les onglets ; sinon un abonnement (NIDMI_ABO_*), et
     * seuls ses abonnes la recoivent (MESURES §200). */
    struct TrameWs { char t[kTrameMax]; uint8_t pour; };

    StaticQueue_t  g_fileTCB;
    QueueHandle_t  g_fileWs = nullptr;
    volatile uint32_t g_jetees   = 0;

    /* LE REGISTRE DES ONGLETS (voir ServerCore.h, « LES ONGLETS »). Huit au
     * plus — la borne qu'appliquait cleanupClients() (DEFAULT_MAX_WS_CLIENTS) ;
     * seize places, pour les arrivants pendant qu'un ancien finit de partir.
     * Le verrou est cree a l'initialisation statique, comme celui de Serial. */
    constexpr uint8_t kOngletsMax    = 8;
    constexpr uint8_t kOngletsPlaces = 16;
    AsyncWebSocketClient* g_onglets[kOngletsPlaces] = {};
    volatile uint8_t g_nOnglets = 0;
    StaticSemaphore_t g_verrouOngletsTampon;
    SemaphoreHandle_t g_verrouOnglets = xSemaphoreCreateRecursiveMutexStatic(&g_verrouOngletsTampon);
    struct VerrouOnglets {
        VerrouOnglets()  { if (g_verrouOnglets) xSemaphoreTakeRecursive(g_verrouOnglets, portMAX_DELAY); }
        ~VerrouOnglets() { if (g_verrouOnglets) xSemaphoreGiveRecursive(g_verrouOnglets); }
    };

    /* CE QUE CHAQUE ONGLET DIT DE LUI, au meme indice que g_onglets (MESURES §199).
     * En PSRAM : alloue par nidmi_ws_file_init(). Sans PSRAM ni tas, le registre
     * marche comme avant et la liste est vide — rien ne casse. */
    struct InfoOnglet {
        uint32_t id;                 // celui d'AsyncWebSocketClient : sert aussi de cle
        uint32_t ip;                 // IPv4, octets dans l'ordre de IPAddress
        uint32_t depuis;             // millis() a l'arrivee
        uint32_t changementVisible;  // millis() du dernier passage premier plan / arriere-plan
        uint32_t rev;                // la revision de composition qu'il dit tenir
        char     nom[24];            // ASCII imprimable : il part tel quel dans du JSON
        char     jeton[13];          // choisi par l'onglet, pour se reconnaitre dans la liste
        uint8_t  mode;               // 0 inconnu, 1 EDIT, 2 REGIE, 3 SCENE
        uint8_t  visible;            // 0 inconnu, 1 premier plan, 2 arriere-plan
        uint8_t  aRev;               // `rev` est-il renseigne ?
    };
    InfoOnglet* g_infos = nullptr;
    volatile uint32_t g_generationClients = 0;

    /* LES ABONNEMENTS DE CHAQUE ONGLET (MESURES §200), au meme indice que
     * g_onglets : des bits NIDMI_ABO_*. En RAM interne, seize octets — pas dans
     * g_infos, qui peut manquer : sans eux, plus de suivi ni de console du tout.
     * `g_abosTous` en est le OU, lisible de partout sans verrou. */
    uint8_t g_abos[kOngletsPlaces] = {};
    volatile uint8_t g_abosTous = 0;
    void recalculerAbos() {   // sous le verrou du registre
        uint8_t tous = 0;
        for (uint8_t i = 0; i < g_nOnglets; i++) tous |= g_abos[i];
        g_abosTous = tous;
    }

    /* « la liste a change » : un evenement minuscule, jamais la liste. Appele
     * sous le verrou du registre — nidmi_ws_pousser n'en prend aucun. */
    void annoncerClients() {
        g_generationClients = g_generationClients + 1;
        char trame[40];
        snprintf(trame, sizeof trame, "NIDMI_CLIENTS:%u:%u",
                 (unsigned)g_nOnglets, (unsigned)g_generationClients);
        nidmi_ws_pousser(trame);
    }
}

void nidmi_ws_file_init() {
    if (g_fileWs) return;
    const size_t taille = kFileLen * sizeof(TrameWs);
    uint8_t* stock = (uint8_t*)heap_caps_malloc(taille, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stock) stock = (uint8_t*)heap_caps_malloc(taille, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stock) g_fileWs = xQueueCreateStatic(kFileLen, sizeof(TrameWs), stock, &g_fileTCB);
    if (!g_infos) {
        const size_t n = kOngletsPlaces * sizeof(InfoOnglet);
        g_infos = (InfoOnglet*)heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_infos) g_infos = (InfoOnglet*)heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
}
/* async_tcp, rappel WS_EVT_CONNECT. */
void nidmi_ws_client_arrive(AsyncWebSocketClient* c) {
    if (!c) return;
    /* Une file pleine JETTE la trame au lieu de FERMER l'onglet : fermer le
     * retirait de la liste en plein parcours (voir « LES ONGLETS »). */
    c->setCloseClientOnQueueFull(false);
    VerrouOnglets verrou;
    if (g_nOnglets >= kOngletsPlaces) { c->close(); return; }   // jamais vu : 16 onglets de front
    if (g_infos) {
        InfoOnglet& i = g_infos[g_nOnglets];
        memset(&i, 0, sizeof i);
        i.id = c->id();
        i.ip = (uint32_t)c->remoteIP();
        i.depuis = millis();
    }
    g_onglets[g_nOnglets] = c;
    g_abos[g_nOnglets] = 0;          // il arrive sans abonnement : il les demandera
    g_nOnglets = g_nOnglets + 1;
    /* Au-dela de huit, le plus ancien s'en va — ce que faisait cleanupClients().
     * Il reste inscrit jusqu'a son depart effectif (WS_EVT_DISCONNECT). */
    if (g_nOnglets > kOngletsMax) g_onglets[0]->close();
    annoncerClients();
}

/* async_tcp, rappel WS_EVT_DISCONNECT — emis par le destructeur du client,
 * avant que sa memoire ne soit rendue. Attendre ici le verrou, c'est attendre
 * que l'envoi en cours vers ce client soit fini. */
void nidmi_ws_client_parti(AsyncWebSocketClient* c) {
    VerrouOnglets verrou;
    for (uint8_t i = 0; i < g_nOnglets; i++) {
        if (g_onglets[i] != c) continue;
        for (uint8_t j = i + 1; j < g_nOnglets; j++) {
            g_onglets[j - 1] = g_onglets[j];
            g_abos[j - 1] = g_abos[j];
            if (g_infos) g_infos[j - 1] = g_infos[j];
        }
        g_nOnglets = g_nOnglets - 1;
        g_onglets[g_nOnglets] = nullptr;
        g_abos[g_nOnglets] = 0;
        if (g_infos) memset(&g_infos[g_nOnglets], 0, sizeof(InfoOnglet));
        /* Ses abonnements partent avec lui, ceux des autres restent : c'etait
         * ici que le suivi des broches s'eteignait pour tous (MESURES §200). */
        recalculerAbos();
        annoncerClients();
        return;
    }
}

/* Un OCTET, pas la liste : lisible depuis n'importe quelle tache sans verrou.
 * C'est ce qui permet a un producteur sur le coeur 0 de sortir immediatement
 * en headless. */
bool nidmi_ws_quelqu_un_ecoute() { return g_nOnglets > 0; }

void nidmi_ws_envoyer_a_tous(const char* texte) {
    if (!texte || !texte[0] || g_nOnglets == 0) return;
    VerrouOnglets verrou;
    /* Par INDICE, relu a chaque tour : un envoi qui echoue peut, dans cette
     * meme tache, rayer un onglet du registre (verrou recursif). */
    for (uint8_t i = 0; i < g_nOnglets; i++) {
        AsyncWebSocketClient* c = g_onglets[i];
        if (c->status() == WS_CONNECTED && c->canSend()) c->text(texte);
        /* sinon : il ne suit pas, la trame est jetee pour lui seul */
    }
}

int nidmi_ws_envoyer_a(uint32_t id, const char* texte) {
    VerrouOnglets verrou;
    for (uint8_t i = 0; i < g_nOnglets; i++) {
        AsyncWebSocketClient* c = g_onglets[i];
        if (c->id() != id) continue;
        if (c->status() != WS_CONNECTED) return -1;
        if (!c->canSend()) return 0;
        c->text(texte);
        return 1;
    }
    return -1;
}
uint32_t nidmi_ws_trames_jetees() { return g_jetees; }

/* ── LES ABONNEMENTS (MESURES §200) ─────────────────────────────────────────── */

void nidmi_ws_abonner(uint32_t id, uint8_t abo, bool oui) {
    VerrouOnglets verrou;
    for (uint8_t i = 0; i < g_nOnglets; i++) {
        if (g_onglets[i]->id() != id) continue;
        const uint8_t neuf = oui ? (uint8_t)(g_abos[i] | abo) : (uint8_t)(g_abos[i] & ~abo);
        if (neuf == g_abos[i]) return;      // deja dans cet etat : rien a dire
        g_abos[i] = neuf;
        recalculerAbos();
        annoncerClients();                  // la liste montre les abonnements
        return;
    }
}

bool nidmi_ws_abonne(uint8_t abo) { return (g_abosTous & abo) != 0; }

void nidmi_ws_envoyer_aux_abonnes(uint8_t abo, const char* texte) {
    if (!texte || !texte[0] || !(g_abosTous & abo)) return;
    VerrouOnglets verrou;
    for (uint8_t i = 0; i < g_nOnglets; i++) {
        if (!(g_abos[i] & abo)) continue;   // pas abonne : pas un paquet de plus pour lui
        AsyncWebSocketClient* c = g_onglets[i];
        if (c->status() == WS_CONNECTED && c->canSend()) c->text(texte);
    }
}

/* ── QUI EST CONNECTE (MESURES §199) ──────────────────────────────────────── */

/* Un nom que la carte rend tel quel dans du JSON : ASCII imprimable, sans les
 * quatre caracteres qui casseraient la trame ou la chaine. */
static void copierTexte(char* dest, size_t cap, const char* src, bool seulementAlnum) {
    size_t k = 0;
    for (; src && *src && k < cap - 1; src++) {
        const uint8_t c = (uint8_t)*src;
        if (seulementAlnum) { if (isalnum(c)) dest[k++] = (char)c; continue; }
        dest[k++] = (c < 0x20 || c > 0x7E || c == '"' || c == '\\' || c == '|' || c == ':') ? '?' : (char)c;
    }
    dest[k] = 0;
}

void nidmi_ws_client_etat(uint32_t id, const char* texte) {
    if (!g_infos || !texte) return;
    char brut[96];
    strlcpy(brut, texte, sizeof brut);
    char* champ[5] = { brut, nullptr, nullptr, nullptr, nullptr };
    uint8_t k = 1;
    for (char* p = brut; *p && k < 5; p++) if (*p == '|') { *p = 0; champ[k++] = p + 1; }
    VerrouOnglets verrou;
    for (uint8_t i = 0; i < g_nOnglets; i++) {
        InfoOnglet& o = g_infos[i];
        if (o.id != id) continue;
        bool change = false;
        auto texteChange = [&](char* dest, size_t cap, const char* src, bool alnum) {
            char neuf[24];
            copierTexte(neuf, sizeof neuf < cap ? sizeof neuf : cap, src, alnum);
            if (strncmp(dest, neuf, cap)) { strlcpy(dest, neuf, cap); change = true; }
        };
        texteChange(o.jeton, sizeof o.jeton, champ[0], true);
        if (champ[1]) texteChange(o.nom, sizeof o.nom, champ[1], false);
        if (champ[2]) {
            const uint8_t m = (champ[2][0] == 'e') ? 1 : (champ[2][0] == 'r') ? 2 : (champ[2][0] == 's') ? 3 : 0;
            if (m != o.mode) { o.mode = m; change = true; }
        }
        if (champ[3]) {
            const uint8_t v = (champ[3][0] == '1') ? 1 : (champ[3][0] == '0') ? 2 : 0;
            if (v != o.visible) { o.visible = v; o.changementVisible = millis(); change = true; }
        }
        if (champ[4]) {
            const uint8_t a = champ[4][0] != 0;
            const uint32_t r = a ? (uint32_t)strtoul(champ[4], nullptr, 16) : 0;
            if (a != o.aRev || r != o.rev) { o.aRev = a; o.rev = r; change = true; }
        }
        if (change) annoncerClients();         // rien de neuf : rien a dire
        return;
    }
}

void nidmi_ws_clients_ecrire(String& j) {
    VerrouOnglets verrou;
    j += "\"n\":" + String((unsigned)g_nOnglets);
    j += ",\"generation\":" + String((unsigned)g_generationClients);
    j += ",\"clients\":[";
    const uint32_t maintenant = millis();
    for (uint8_t i = 0; i < g_nOnglets; i++) {
        if (i) j += ",";
        if (!g_infos) { j += "{}"; continue; }
        const InfoOnglet& o = g_infos[i];
        const IPAddress ip((uint32_t)o.ip);
        /* Le cable ou le WiFi : la carte le sait par l'adresse locale que cette
         * connexion a empruntee (le meme calcul que /api/interface). Le pointeur
         * du client est valide tant qu'on tient le verrou : le destructeur
         * attend ce meme verrou (voir « LES ONGLETS »). */
        AsyncWebSocketClient* c = g_onglets[i];
        const bool cable = (c && c->client())
                         ? nidmi_usbnet::parLeCable(c->client()->localIP(), ip) : false;
        char rev[9];
        snprintf(rev, sizeof rev, "%08x", (unsigned)o.rev);
        j += "{\"id\":" + String((unsigned)o.id);
        j += ",\"jeton\":\""; j += o.jeton; j += "\"";
        j += ",\"nom\":\""; j += o.nom; j += "\"";
        j += ",\"ip\":\""; j += ip.toString(); j += "\"";
        j += ",\"cable\":"; j += cable ? "true" : "false";
        j += ",\"depuis_s\":" + String((unsigned)((maintenant - o.depuis) / 1000));
        j += ",\"mode\":\""; j += (o.mode == 1 ? "edit" : o.mode == 2 ? "regie" : o.mode == 3 ? "scene" : ""); j += "\"";
        j += ",\"visible\":"; j += (o.visible == 1 ? "true" : o.visible == 2 ? "false" : "null");
        j += ",\"visible_depuis_s\":" + String(o.visible ? (unsigned)((maintenant - o.changementVisible) / 1000) : 0u);
        j += ",\"rev\":\""; j += (o.aRev ? rev : ""); j += "\"";
        /* Ce a quoi il est abonne (§200) : ce qu'il recoit de plus que les autres. */
        j += ",\"abonnements\":[";
        j += (g_abos[i] & NIDMI_ABO_BROCHES) ? "\"broches\"" : "";
        j += ((g_abos[i] & NIDMI_ABO_BROCHES) && (g_abos[i] & NIDMI_ABO_CONSOLE)) ? "," : "";
        j += (g_abos[i] & NIDMI_ABO_CONSOLE) ? "\"console\"" : "";
        j += "]}";
    }
    j += "]";
}

static bool pousserPour(uint8_t pour, const char* trame) {
    if (!trame || !trame[0] || !g_fileWs) return false;
    TrameWs m;
    strlcpy(m.t, trame, sizeof m.t);
    m.pour = pour;
    /* File pleine = le client ne suit pas. ON JETTE, sans attendre : bloquer
     * ici bloquerait MidiTask, et une note en retard vaut pire qu'une courbe
     * trouee. */
    if (xQueueSend(g_fileWs, &m, 0) != pdTRUE) { g_jetees++; return false; }
    return true;
}
bool nidmi_ws_pousser(const char* trame) { return pousserPour(0, trame); }
bool nidmi_ws_pousser_aux_abonnes(uint8_t abo, const char* trame) {
    if (!(g_abosTous & abo)) return false;    // personne : rien a poser dans la file
    return pousserPour(abo, trame);
}

void nidmi_ws_drainer() {
    if (!g_fileWs) return;
    /* On vide la file MEME si personne n'ecoute : la laisser pleine ferait
     * jeter les trames suivantes a tort, et masquerait le retour d'un client. */
    TrameWs m;
    uint8_t n = 0;
    while (n < kFileLen && xQueueReceive(g_fileWs, &m, 0) == pdTRUE) {
        if (m.pour) nidmi_ws_envoyer_aux_abonnes(m.pour, m.t);
        else        nidmi_ws_envoyer_a_tous(m.t);
        n++;
    }
}
