#include "APICommon.h"
#include "../Globals.h"
#include "../server/ServerCore.h"
#include "../server/WebDebugConsole.h"
#include "../server/ServerCallbacks.h"
#include "../network/UsbNetBootstrap.h"
String nidmi_essaiWifiJson();          // NiDMI.cpp
String nidmi_cablePrioritaireJson();   // NiDMI.cpp
#include <Preferences.h>
#include <WiFi.h>

// Version firmware : en-tête généré au build (sync_files) et gravé dans l'image.
// Valeurs par défaut si l'en-tête est absent (ex. build IDE sans le script).
#if defined(__has_include)
#  if __has_include("../nidmi_fw_version.h")
#    include "../nidmi_fw_version.h"
#  endif
#endif
#ifndef NIDMI_FW_VERSION
#define NIDMI_FW_VERSION "dev"
#endif
#ifndef NIDMI_FW_VARIANT
#define NIDMI_FW_VARIANT "?"
#endif

void setupNetworkAPI(AsyncWebServer& server) {
    // API - Statut général
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        // Récupérer mDNS
        Preferences preferences;
        preferences.begin("nidmi", true);
        String mdnsName = preferences.getString("mdns_name", "nidmi");
        
        // Récupérer OSC
        String oscTarget = preferences.getString("osc_target", "sta");
        int oscPort = preferences.getInt("osc_port", 8000);
        String oscIp = preferences.getString("osc_ip", "");
        bool oscBroadcast = preferences.getBool("osc_broadcast", false);
        preferences.end();
        
        String json = "{";
        {
            /* Lire le WiFi SOUS LE VERROU RADIO : une transition de la bascule
             * detruit les interfaces avant d'en effacer les pointeurs. Sans
             * verrou, ce gestionnaire a lu une interface detruite et fait
             * paniquer la carte (MESURES §149). Radio coupee : champs vides. */
            ServerCore::LectureRadio radio(pdMS_TO_TICKS(300));
            const bool r = radio.allumee();
            json += "\"ap_ssid\":\"" + (r ? WiFi.softAPSSID() : String("")) + "\",";
            json += "\"ap_ip\":\"" + (r ? WiFi.softAPIP().toString() : String("")) + "\",";
            json += "\"sta_ssid\":\"" + (r ? WiFi.SSID() : String("")) + "\",";
            json += "\"sta_ip\":\"" + (r ? WiFi.localIP().toString() : String("")) + "\",";
            json += "\"sta_connected\":" + String(r && WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
        }
        json += "\"mdns_name\":\"" + mdnsName + "\",";
        json += "\"mdns_address\":\"" + mdnsName + ".local\",";
        json += "\"osc_target\":\"" + oscTarget + "\",";
        json += "\"osc_port\":" + String(oscPort);
        if(oscIp.length() > 0) {
            json += ",\"osc_ip\":\"" + oscIp + "\"";
        }
        json += ",\"osc_broadcast\":" + String(oscBroadcast ? "true" : "false");
        json += ",\"web_debug_console\":" + String(nidmi_web_debug_is_supported() ? "true" : "false");
        json += ",\"fw_version\":\"" NIDMI_FW_VERSION "\"";
        json += ",\"fw_variant\":\"" NIDMI_FW_VARIANT "\"";
        json += "}";
        request->send(200, "application/json", json);
    });
    
    // API - Configuration Wi-Fi STA
    //
    // NOTE:
    // On ne redémarre plus automatiquement l'ESP après sauvegarde STA.
    // Le reboot auto pouvait donner l'impression que "le serveur plante"
    // (perte temporaire de l'AP, reconnexion WiFi côté client, etc.).
    // Le nom de réseau et la connexion STA seront pris en compte
    // au prochain redémarrage manuel (ou reset bouton).
    /* ── LE CABLE OU LE WIFI ────────────────────────────────────────────────
     * « On n'a pas besoin de deux acces en simultane : soit l'un, soit l'autre. »
     *
     * La bascule « cable prioritaire » (NiDMI.cpp) coupe la radio quand le
     * cable vit, sur une preuve de vie qui ne ment pas — des trames recues —
     * et la rallume des qu'il se tait. Son reglage est en NVS, oui par defaut.
     * La coupure LIBRE, elle, reste retiree : son repli croyait linkUp(), vu
     * vrai sur un lien mort (§143). L'ESSAI ne compte que sur son minuteur : il
     * coupe la radio N secondes, mesure le tas, la rallume — sur TOUS les
     * builds. Pendant que la bascule tient la radio coupee, l'essai et
     * etat=on sont refuses : ils defairaient ce qu'elle tient. */
    server.on("/api/reseau/liens", HTTP_GET, [](AsyncWebServerRequest *request){
        String j = "{\"wifi\":";
        j += serverCore.radioWifiAllumee() ? "true" : "false";
        j += ",\"usb\":" + nidmi_usbnet::etatJson();
        j += ",\"essai_wifi\":" + nidmi_essaiWifiJson();
        j += ",\"cable\":" + nidmi_cablePrioritaireJson() + "}";
        request->send(200, "application/json", j);
    });

    server.on("/api/reseau/cable-prioritaire", HTTP_POST, [](AsyncWebServerRequest *request){
        const String etat = request->hasParam("etat", true)
                          ? request->getParam("etat", true)->value() : String("");
        if (etat != "on" && etat != "off") {
            request->send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"etat=on ou etat=off\"}");
            return;
        }
        nidmi_demanderCablePrioritaire(etat == "on");
        request->send(200, "application/json",
            String("{\"status\":\"ok\",\"prioritaire\":") + (etat == "on" ? "true" : "false") +
            ",\"message\":\"applique et memorise dans la seconde\"}");
    });

    /* RELANCER LE CABLE (MESURES §154) : refaire l'enumeration USB, quand
     * l'hote laisse le cable « inactive » sans jamais le reactiver (§153).
     * Coupe AUSSI le MIDI USB une a deux secondes — geste manuel (Reglages →
     * Carte → Reseau), jamais automatique. Executee par nidmi_loop dans la
     * seconde ; ici on ne fait que la demander. */
    server.on("/api/reseau/cable/relancer", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!nidmi_usbnet::enabled()) {
            request->send(409, "application/json",
                "{\"status\":\"error\",\"message\":\"pas de lien reseau USB dans ce firmware\"}");
            return;
        }
        nidmi_demanderRelanceCable();
        request->send(200, "application/json",
            "{\"status\":\"ok\",\"message\":\"relance dans la seconde : le cable repart, puis revient\"}");
    });

    server.on("/api/reseau/wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        const String etat = request->hasParam("etat", true)
                          ? request->getParam("etat", true)->value() : String("");
        if ((etat == "on" || etat == "essai") && nidmi_cableTientLeWifi()) {
            request->send(409, "application/json",
                "{\"status\":\"error\",\"message\":\"Le cable prioritaire tient le WiFi coupe : "
                "le retirer (POST /api/reseau/cable-prioritaire etat=off) pour rallumer ou essayer.\"}");
            return;
        }
        if (etat == "on") {
            request->send(200, "application/json",
                "{\"status\":\"ok\",\"wifi\":\"rallume dans 300 ms\"}");
            nidmi_requestRallumerWifi();
            return;
        }
        if (etat == "essai") {
            long d = request->hasParam("duree", true)
                   ? request->getParam("duree", true)->value().toInt() : 10;
            if (d < 5)  d = 5;     // la mesure « WiFi coupe » est prise a 3 s
            if (d > 60) d = 60;    // borne le pire cas : d secondes sans reseau
            request->send(200, "application/json",
                "{\"status\":\"ok\",\"essai\":\"WiFi coupe dans 300 ms, pour " + String(d) +
                " s\",\"message\":\"La carte ne repondra plus pendant ce temps. Le WiFi revient "
                "sur minuteur, quoi qu'il arrive. Resultat dans /api/reseau/liens.\"}");
            nidmi_requestEssaiWifi((unsigned long)d * 1000UL);
            return;
        }
        if (etat == "off") {
            request->send(409, "application/json",
                "{\"status\":\"error\",\"message\":\"Coupure libre retiree : le lien USB a "
                "ete vu mort pendant que les deux bouts le disaient monte (MESURES §143). "
                "La bascule cable prioritaire coupe le WiFi sur preuve de vie "
                "(POST /api/reseau/cable-prioritaire). Pour mesurer : etat=essai.\"}");
            return;
        }
        request->send(400, "application/json",
            "{\"status\":\"error\",\"message\":\"etat=essai, etat=on (etat=off est retire)\"}");
    });

    server.on("/api/sta", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("ssid", true) && request->hasParam("pass", true)) {
            String ssid = request->getParam("ssid", true)->value();
            String pass = request->getParam("pass", true)->value();
            String ip = request->hasParam("ip", true) ? request->getParam("ip", true)->value() : String("");
            String gateway = request->hasParam("gw", true) ? request->getParam("gw", true)->value() : String("");
            String subnet = request->hasParam("sn", true) ? request->getParam("sn", true)->value() : String("");

            Preferences preferences;
            preferences.begin("nidmi", false);
            preferences.putString("sta_ssid", ssid);
            preferences.putString("sta_pass", pass);
            if (ip.length() > 0 && gateway.length() > 0 && subnet.length() > 0) {
                preferences.putString("sta_ip", ip);
                preferences.putString("sta_gw", gateway);
                preferences.putString("sta_sn", subnet);
            }
            preferences.end();

            // Reboot différé (~2 s, géré dans nidmi_loop) pour appliquer la config STA :
            // g_staSsid/g_staPass ne sont lus qu'au boot. L'AP reste actif (APSTA),
            // donc l'accès web n'est pas perdu. La réponse part avant le redémarrage.
            nidmi_requestReboot((String("wifi · ") + request->client()->remoteIP().toString()).c_str());
            request->send(200, "application/json", "{\"status\":\"ok\",\"reboot\":true}");
        } else {
            request->send(400, "application/json", "{\"error\":\"ssid and pass required\"}");
        }
    });

    // API - Lecture des identifiants STA stockés en NVS
    server.on("/api/sta/status", HTTP_GET, [](AsyncWebServerRequest *request){
        try {
            Preferences preferences;
            preferences.begin("nidmi", true);
            String ssid = preferences.getString("sta_ssid", "");
            String pass = preferences.getString("sta_pass", "");
            String ip   = preferences.getString("sta_ip",  "");
            String gw   = preferences.getString("sta_gw",  "");
            String sn   = preferences.getString("sta_sn",  "");
            preferences.end();
            
            String json = "{";
            json += "\"ssid\":\"" + ssid + "\",";
            json += "\"has_pass\":" + String(pass.length()>0 ? "true" : "false") + ",";
            json += "\"ip\":\"" + ip + "\",";
            json += "\"gw\":\"" + gw + "\",";
            json += "\"sn\":\"" + sn + "\"";
            json += "}";
            request->send(200, "application/json", json);
        } catch (...) {
            request->send(500, "application/json", "{\"error\":\"NVS read failed\"}");
        }
    });
    
    // API - Configuration mDNS
    server.on("/api/mdns", HTTP_POST, [](AsyncWebServerRequest *request){
        if(request->hasParam("name", true)){
            String name = request->getParam("name", true)->value();
            
            Preferences preferences;
            preferences.begin("nidmi", false);
            preferences.putString("mdns_name", name);
            preferences.end();
            
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(400, "application/json", "{\"error\":\"name required\"}");
        }
    });
    
    // API - Statut mDNS
    server.on("/api/mdns/status", HTTP_GET, [](AsyncWebServerRequest *request){
        Preferences preferences;
        preferences.begin("nidmi", false);
        String name = preferences.getString("mdns_name", "nidmi");
        preferences.end();
        String json = "{";
        json += "\"name\":\"" + name + "\"";
        json += "}";
        request->send(200, "application/json", json);
    });
}
