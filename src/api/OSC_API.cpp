#include "APICommon.h"
#include "../server/ServerCallbacks.h"
#include "../managers/ComponentManager.h"
#include <Preferences.h>

void setupOSC_API(AsyncWebServer& server) {
    /* API - Configuration OSC */
    server.on("/api/osc", HTTP_POST, [](AsyncWebServerRequest *request){
        if(request->hasParam("target", true) && request->hasParam("port", true)){
            String target = request->getParam("target", true)->value();
            int port = request->getParam("port", true)->value().toInt();
            bool broadcast = request->hasParam("broadcast", true) && 
                           request->getParam("broadcast", true)->value() == "true";
            String interface = request->hasParam("interface", true) ? 
                             request->getParam("interface", true)->value() : "ap";
            
            /* Sauvegarder en NVS */
            Preferences preferences;
            preferences.begin("nidmi", false);
            preferences.putString("osc_target", target);
            preferences.putInt("osc_port", port);
            preferences.putBool("osc_broadcast", broadcast);
            preferences.putString("osc_interface", interface);
            preferences.end();
            
            /* APPLIQUER TOUT DE SUITE au gestionnaire VIVANT.
             *
             * La route n'ecrivait qu'en NVS, et /api/osc/status relisait la
             * NVS : la configuration PARAISSAIT prise alors que le gestionnaire
             * continuait de viser l'ancienne cible. Il fallait redemarrer pour
             * que ce que montrait l'app devienne vrai — le meme piege que la
             * zone d'E/S, qui montrait une carte imaginaire (MESURES.md §40). */
            OSCManager& o = g_componentManager.osc();
            o.setTarget(target, (uint16_t)port);
            o.setBroadcast(broadcast);
            o.setInterface(interface == "sta"  ? OSC_INTERFACE_STA
                         : interface == "both" ? OSC_INTERFACE_BOTH
                                               : OSC_INTERFACE_AP);

            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(400, "application/json", "{\"error\":\"target and port required\"}");
        }
    });

    /* API - Activer / désactiver la sortie OSC globale (toutes les pins + MUX), NVS + rechargement runtime */
    server.on("/api/osc/output-enable", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("enable", true)) {
            request->send(400, "application/json", "{\"status\":\"error\",\"error\":\"enable required\"}");
            return;
        }
        bool enabled = request->getParam("enable", true)->value() == "true";
        Preferences preferences;
        preferences.begin("nidmi", false);
        preferences.putBool("osc_out_all", enabled);
        preferences.end();
        nidmi_requestReloadPins();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    /* API - Statut OSC */
    server.on("/api/osc/status", HTTP_GET, [](AsyncWebServerRequest *request){
        Preferences preferences;
        preferences.begin("nidmi", true);
        String target = preferences.getString("osc_target", "192.168.4.100");
        int port = preferences.getInt("osc_port", 8000);
        bool broadcast = preferences.getBool("osc_broadcast", false);
        String interface = preferences.getString("osc_interface", "ap");
        bool outputAll = preferences.getBool("osc_out_all", true);
        preferences.end();
        /* Ce que la carte FAIT, a cote de ce qu'elle a RETENU. Les deux
         * doivent coincider ; quand ils divergent, c'est visible plutot que
         * silencieux. `enabled` dit si un osc.out() partira reellement. */
        OSCManager& o = g_componentManager.osc();
        String json = "{";
        json += "\"target\":\"" + target + "\",";
        json += "\"port\":" + String(port) + ",";
        json += "\"vivant\":{\"target\":\"" + o.getTargetIP() + "\","
              + "\"port\":" + String(o.getTargetPort()) + ","
              + "\"broadcast\":" + String(o.isBroadcastEnabled() ? "true" : "false") + ","
              + "\"interface\":" + String((int)o.getInterface()) + ","
              + "\"initialized\":" + String(o.isInitialized() ? "true" : "false") + ","
              + "\"enabled\":" + String(o.isEnabled() ? "true" : "false") + "},";
        json += "\"broadcast\":" + String(broadcast ? "true" : "false") + ",";
        json += "\"interface\":\"" + interface + "\",";
        json += "\"output_all_enabled\":" + String(outputAll ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
    });
}
