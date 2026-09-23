#include "APICommon.h"
#include "../server/ServerCallbacks.h"   // nidmi_requestReboot
#include "../server/WebDebugConsole.h"   // NIDMI_WEB_LOG
#include <Update.h>

/*
 * OTA embarqué : POST /api/ota reçoit l'image firmware (le .bin "app", PAS le
 * .merged.bin) en CORPS BRUT (application/octet-stream), l'écrit dans le slot
 * OTA libre via la lib Update, puis reboote en différé pour booter dessus.
 *
 * Intégrité : on envoie le binaire brut (et non du multipart) pour que le
 * Content-Length = taille exacte du firmware. On passe cette taille à
 * Update.begin(total) ; si l'upload est tronqué (coupure réseau), la dernière
 * frame (index+len == total) n'arrive jamais -> Update.end() n'est pas validé
 * -> la partition n'est PAS marquée bootable -> la carte reste sur le firmware
 * actuel (pas de brick). C'est le correctif après un upload tronqué qui avait
 * booté une image partielle.
 *
 * Prérequis : table de partitions avec 2 slots app (build --ota /
 * nidmi_s3_ota_dual_littlefs). Sans slot OTA, Update.begin() échoue.
 * Progression suivie côté navigateur (XHR upload).
 *
 * Sécurité : endpoint non authentifié (usage atelier sur réseau isolé / AP).
 * À protéger (token/mot de passe) si exposé sur un réseau ouvert.
 */
static bool s_otaOk = false;

void setupOtaAPI(AsyncWebServer& server) {
    server.on("/api/ota", HTTP_POST,
        // onRequest : appelé une fois tout le corps reçu (uploads complets seulement)
        [](AsyncWebServerRequest *request) {
            if (s_otaOk && Update.isFinished() && !Update.hasError()) {
                request->send(200, "application/json", "{\"status\":\"ok\",\"reboot\":true}");
                NIDMI_WEB_LOG("[OTA] Image validée, redémarrage...");
                nidmi_requestReboot((String("flash · ") + request->client()->remoteIP().toString()).c_str());
            } else {
                String err = Update.hasError() ? String(Update.errorString())
                                               : String("upload incomplet ou invalide");
                if (Update.isRunning()) Update.abort();
                request->send(500, "application/json",
                              "{\"status\":\"error\",\"error\":\"" + err + "\"}");
                NIDMI_WEB_LOG("[OTA] Echec: %s", err.c_str());
            }
            s_otaOk = false;
        },
        NULL,  // pas de handler multipart : on lit le corps brut ci-dessous
        // onBody : fragments du corps brut ; total = Content-Length = taille firmware
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                s_otaOk = false;
                Serial.printf("[OTA] Debut upload: %u octets attendus\n", (unsigned)total);
                NIDMI_WEB_LOG("[OTA] Debut upload: %u octets", (unsigned)total);
                if (total == 0 || !Update.begin(total, U_FLASH)) {
                    Update.printError(Serial);
                    return;
                }
            }
            if (Update.isRunning() && len) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                    return;
                }
            }
            // Dernière frame uniquement si on a bien reçu tout le corps annoncé
            if (total > 0 && index + len == total) {
                if (Update.end(true)) {
                    s_otaOk = true;
                    Serial.printf("[OTA] Termine: %u octets\n", (unsigned)total);
                } else {
                    Update.printError(Serial);
                }
            }
        }
    );
}
