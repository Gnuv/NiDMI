#include "APICommon.h"
#include "../config/SystemConfig.h"
#include "../server/ServerCallbacks.h"
#include <soc/rtc_cntl_reg.h>
#include <esp_system.h>

void setupSystemAPI(AsyncWebServer& server) {
    /* API - Obtenir les paramètres système */
    server.on("/api/system/get", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        json += "\"touchEnabled\":" + String(SystemConfig::isTouchEnabled() ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
    });
    
    /* API - Modifier les paramètres système */
    server.on("/api/system/set", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("touchEnabled", true)) {
            request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"touchEnabled parameter required\"}");
            return;
        }
        
        String touchEnabledStr = request->getParam("touchEnabled", true)->value();
        bool touchEnabled = (touchEnabledStr == "true" || touchEnabledStr == "1");
        
        SystemConfig::setTouchEnabled(touchEnabled);
        
        // Recharger les composants pour appliquer le changement
        nidmi_requestReloadPins();
        
        request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Settings updated, components reloaded\"}");
    });

    /* API - Mode TÉLÉCHARGEMENT (bootloader ROM), déclenché par logiciel.
     *
     * En variante usbmidi-on, la carte ne présente aucun port série : esptool ne
     * peut donc pas la réveiller, et la manip BOOT/RESET sur deux boutons de deux
     * millimètres n'a jamais abouti. Sans ce chemin, l'OTA est le SEUL moyen de
     * flasher — ce qui interdit à jamais de changer la table de partitions (elle
     * vit à 0x8000, hors des slots applicatifs) et de récupérer une table
     * corrompue. C'est un point de défaillance unique.
     *
     * Le drapeau force_download_boot vit en RTC : il survit au reset logiciel et
     * il est EFFACÉ PAR UNE COUPURE D'ALIMENTATION. Le pire cas est donc de
     * débrancher/rebrancher pour revenir à un démarrage normal — on ne peut pas
     * s'enfermer dehors. */
    server.on("/api/system/download", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "application/json",
            "{\"status\":\"ok\",\"message\":\"Download mode: la carte redemarre en "
            "bootloader ROM. Elle ne repondra plus en HTTP. Pour revenir: couper "
            "l'alimentation.\"}");
        // Différé côté loop() : un delay() ici bloquerait async_tcp, donc la
        // réponse ne partirait jamais (constaté au premier essai).
        nidmi_requestDownloadMode();
    });

    /* API - Reset logiciel (comme appuyer sur le bouton reset) */
    server.on("/api/system/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Reboot scheduled\"}");
        // Reboot différé côté loop (pour laisser partir la réponse HTTP)
        nidmi_requestReboot();
    });
}
