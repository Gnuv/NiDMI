#include "APICommon.h"
#include "../server/ServerCallbacks.h"   // nidmi_requestReboot
#include "../server/WebDebugConsole.h"   // NIDMI_WEB_LOG
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_flash_partitions.h>   // ESP_BOOTLOADER_OFFSET, ESP_PARTITION_TABLE_OFFSET / _MAX_LEN
#include <esp_app_format.h>         // l'en-tete d'une image ESP

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

/*
 * L'IMAGE, DANS L'AUTRE SENS (MESURES §192) : GET /api/system/image?partie=…
 * rend ce qu'il faut pour CLONER la carte. L'app l'emporte dans un .instrument
 * « avec le firmware » ; la future app de preparation l'ecrira par l'USB sur une
 * carte neuve (amorce, table, firmware), puis rechargera l'instrument.
 *   partie=amorce      le chargeur d'amorcage (0x0), sa longueur exacte
 *   partie=partitions  la table de partitions (0x8000), 3 Ko
 *   partie=firmware    le firmware QUI TOURNE (app0 ou app1), sa longueur exacte
 * En-tetes : X-Nidmi-Adresse (ou il est), X-Nidmi-Emplacement (app0/app1).
 *
 * Lire la flash suspend le cache par morceaux (un par envoi TCP) : un geste de
 * preparation, comme l'OTA. Le cout pour le son est mesure au §192.
 */

/* L'amorce et la table ne sont pas dans la table : IDF 5.5 sait les declarer
 * (types BOOTLOADER, PARTITION_TABLE). Declarees une fois, a la premiere
 * demande — quelques dizaines d'octets, gardes. */
static const esp_partition_t* zoneFixe(esp_partition_type_t type, esp_partition_subtype_t sous,
                                       size_t adresse, size_t taille, const char* nom) {
    const esp_partition_t* p = esp_partition_find_first(type, sous, nullptr);
    if (p) return p;
    if (esp_partition_register_external(nullptr, adresse, taille, nom, type, sous, &p) != ESP_OK) return nullptr;
    return p;
}

/* La longueur d'une image ESP (esp_app_format.h) : un en-tete de 24 octets, des
 * segments (8 octets d'en-tete + leurs donnees), un octet de somme de controle
 * qui complete a 16, puis l'empreinte SHA-256 si l'en-tete l'annonce.
 * esp_image_get_metadata() le ferait, mais Arduino ne la lie pas. 0 : illisible. */
static size_t longueurImage(const esp_partition_t* p) {
    esp_image_header_t h;
    if (esp_partition_read(p, 0, &h, sizeof h) != ESP_OK || h.magic != ESP_IMAGE_HEADER_MAGIC
        || h.segment_count == 0 || h.segment_count > ESP_IMAGE_MAX_SEGMENTS) return 0;
    size_t pos = sizeof h;
    for (uint8_t i = 0; i < h.segment_count; i++) {
        esp_image_segment_header_t seg;
        if (esp_partition_read(p, pos, &seg, sizeof seg) != ESP_OK) return 0;
        pos += sizeof seg + seg.data_len;
        if (pos > p->size) return 0;
    }
    pos = (pos + 1 + 15) & ~(size_t)15;     // la somme de controle complete a 16
    if (h.hash_appended == 1) pos += 32;    // SHA-256
    return pos <= p->size ? pos : 0;
}

void setupOtaAPI(AsyncWebServer& server) {
    server.on("/api/system/image", HTTP_GET, [](AsyncWebServerRequest *request) {
        const String partie = request->hasParam("partie") ? request->getParam("partie")->value() : String();
        const esp_partition_t* p = nullptr;
        size_t n = 0;
        String fichier;
        if (partie == "firmware") {
            p = esp_ota_get_running_partition();
            n = p ? longueurImage(p) : 0;
            fichier = "firmware.bin";
        } else if (partie == "amorce") {
            p = zoneFixe(ESP_PARTITION_TYPE_BOOTLOADER, ESP_PARTITION_SUBTYPE_BOOTLOADER_PRIMARY,
                         ESP_BOOTLOADER_OFFSET, ESP_PARTITION_TABLE_OFFSET - ESP_BOOTLOADER_OFFSET, "amorce");
            n = p ? longueurImage(p) : 0;
            fichier = "bootloader.bin";
        } else if (partie == "partitions") {
            p = zoneFixe(ESP_PARTITION_TYPE_PARTITION_TABLE, ESP_PARTITION_SUBTYPE_PARTITION_TABLE_PRIMARY,
                         ESP_PARTITION_TABLE_OFFSET, 0x1000, "table");
            n = p ? ESP_PARTITION_TABLE_MAX_LEN : 0;
            fichier = "partitions.bin";
        } else {
            request->send(400, "application/json",
                          "{\"status\":\"error\",\"message\":\"partie=amorce, partitions ou firmware\"}");
            return;
        }
        if (!p || !n) {
            request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"image illisible\"}");
            return;
        }
        AsyncWebServerResponse* r = request->beginResponse("application/octet-stream", n,
            [p, n](uint8_t* out, size_t maxLen, size_t index) -> size_t {
                const size_t k = (n - index < maxLen) ? (n - index) : maxLen;
                return esp_partition_read(p, index, out, k) == ESP_OK ? k : 0;
            });
        r->addHeader("Content-Disposition", "attachment; filename=\"" + fichier + "\"");
        r->addHeader("Cache-Control", "no-store");
        r->addHeader("X-Nidmi-Adresse", "0x" + String((unsigned)p->address, HEX));
        if (partie == "firmware") r->addHeader("X-Nidmi-Emplacement", p->label);
        request->send(r);
    });

    server.on("/api/ota", HTTP_POST,
        // onRequest : appelé une fois tout le corps reçu (uploads complets seulement)
        [](AsyncWebServerRequest *request) {
            /* La marge de pile de la tache web a la fin de l'OTA : le chemin le
             * plus profond qu'elle connaisse (ecriture flash, verification de
             * l'image), et le seul qu'on ne puisse pas relire apres coup — la
             * carte redemarre. Le flasheur l'affiche. MESURES §152. */
            const unsigned marge = (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
            if (s_otaOk && Update.isFinished() && !Update.hasError()) {
                request->send(200, "application/json",
                              "{\"status\":\"ok\",\"reboot\":true,\"marge_pile\":" + String(marge) + "}");
                NIDMI_WEB_LOG("[OTA] Image validée, redémarrage...");
                nidmi_requestReboot((String("flash · ") + request->client()->remoteIP().toString()).c_str());
            } else {
                String err = Update.hasError() ? String(Update.errorString())
                                               : String("upload incomplet ou invalide");
                if (Update.isRunning()) Update.abort();
                request->send(500, "application/json",
                              "{\"status\":\"error\",\"error\":\"" + err + "\",\"marge_pile\":" + String(marge) + "}");
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
