#include "ServerCore.h"
#include <ESPmDNS.h>
#include <Preferences.h>
// setupWebAPI est déclaré plus bas et défini dans WebAPI.cpp

// Déclaration de la fonction setupHttp définie dans WebAPI.cpp
void setupWebAPI(AsyncWebServer& server, AsyncWebSocket& ws);

// Instance globale
ServerCore serverCore;

ServerCore::ServerCore()
    : server(80), ws("/ws") {}

void ServerCore::begin(const char* apSsid, const char* apPass, const char* hostname, bool apOnlyMode) {
    /* Événements WiFi : visibilité des drops STA (avec la RAISON, indisponible par polling)
     * et de l'obtention d'IP. Log Serial uniquement — pas d'accès WebSocket depuis la tâche
     * event WiFi, pour éviter les races avec la tâche serveur (AsyncWebSocket). */
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info){
        if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            Serial.printf("[WiFi] STA got IP: %s\n", WiFi.localIP().toString().c_str());
        } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            Serial.printf("[WiFi] STA déconnectée (raison=%d)\n", (int)info.wifi_sta_disconnected.reason);
        }
    });

    /* Sans STA enregistré : AP seul (WIFI_AP). APSTA avec interface STA inactive peut provoquer
     * échecs ou boucles « mot de passe » / reconnexion sur téléphones (notamment ESP32-C3/S3). */
    if (apOnlyMode) {
        WiFi.mode(WIFI_MODE_AP);
        Serial.println("[ServerCore] WiFi: mode AP uniquement");
    } else {
        WiFi.mode(WIFI_MODE_APSTA);
    }
    
    // Augmenter la puissance WiFi pour XIAO_ESP32C3
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // Puissance maximale
    
    /* Configurer l'IP de l'AP explicitement à 192.168.4.1 */
    IPAddress apIp = IPAddress(192, 168, 4, 1);
    IPAddress apGateway = IPAddress(192, 168, 4, 1);
    IPAddress apSubnet = IPAddress(255, 255, 255, 0);
    WiFi.softAPConfig(apIp, apGateway, apSubnet);
    
    /* Canal 1 : meilleure compatibilité avec les clients 2,4 GHz */
    WiFi.softAP(apSsid, apPass, 1);
    
    /* Attendre que l'AP soit prêt avant de continuer */
    delay(100);
    
    /* Vérifier l'IP de l'AP (devrait être 192.168.4.1) */
    apIp = WiFi.softAPIP();
    
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

void ServerCore::update() {
    // Mise à jour du WebSocket
    ws.cleanupClients();
    
    // Mise à jour RTP-MIDI
    rtpMidiInstance.update();
    
    // Mise à jour Bluetooth
    bluetoothInstance.update();
    
    // Mise à jour USB MIDI
    usbMidiInstance.update();
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

/* Voir ServerCore.h pour le pourquoi. Deux questions, pas une : « quelqu'un
 * ecoute-t-il ? » puis « suit-il ? ». */
bool nidmi_ws_peut_emettre(AsyncWebSocket& ws) {
    if (ws.count() == 0) return false;          // headless : personne n'ecoute
    return ws.availableForWriteAll();           // un client a la traine : on jette
}
