#include "APICommon.h"
#include "../components/ComponentRegistry.h"
#include "../components/ComponentTypes.h"
#include "../components/motion/Lis3dhDef.h"
#include "../managers/ComponentManager.h"
#include "../Globals.h"
#include <set>

/**
 * @file ComponentsAPI.cpp
 * @brief API pour exposer les définitions de composants au frontend
 * 
 * Routes:
 * - GET /api/components/definitions : Liste toutes les définitions de composants
 * - GET /api/components/used-gpios : Retourne les GPIOs utilisés par les composants configurés
 */

/**
 * @brief Initialise les routes API pour les composants
 * @param server Serveur web ESPAsyncWebServer
 */
void setupComponentsAPI(AsyncWebServer& server) {
    
    /**
     * GET /api/components/definitions
     * Retourne la liste des composants disponibles avec leurs métadonnées
     */
    /* ── Empreinte des definitions ────────────────────────────────────────
     * L'app garde les definitions en cache local (elles sont compilees dans le
     * firmware et ne bougent pas), et les redemander a chaque ouverture coutait
     * trois secondes — 43 pages d'une definition chacune (MESURES.md §30).
     *
     * La cle du cache etait fw_version. Mauvaise cle : « git describe --dirty »
     * rend la MEME chaine pour deux builds sales au meme commit, si bien que
     * deux firmwares differents partageaient un cache pendant le developpement.
     * On expose donc une empreinte de CE QUE VALENT les definitions, pas de la
     * version du source dont elles viennent.
     *
     * FNV-1a 32 bits sur le JSON de toutes les pages, calcule une seule fois et
     * garde : ~43 rendus dans un tampon pris le temps du calcul (en PSRAM, voir
     * nidmi_tampon_reponse), quelques dizaines de ms, et seulement si quelqu'un
     * demande. */
    server.on("/api/components/empreinte", HTTP_GET, [](AsyncWebServerRequest* request) {
        static char cache[16] = {0};
        if (!cache[0]) {
            constexpr size_t kTampon = 10240;
            std::shared_ptr<char> tampon = nidmi_tampon_reponse(kTampon);
            if (!tampon) {
                request->send(503, "application/json", "{\"error\":\"memoire\"}");
                return;
            }
            uint32_t h = 2166136261u;                 // FNV-1a, valeur de depart
            const int total = (int)ComponentRegistry::count();
            for (int p = 0; p < total; p++) {
                const int n = ComponentRegistry::toJsonArrayPage(tampon.get(), kTampon, p, 1);
                for (int i = 0; i < n; i++) { h ^= (uint8_t)tampon.get()[i]; h *= 16777619u; }
            }
            // Le NOMBRE compte aussi : deux jeux differents pourraient, a la
            // marge, donner le meme condense d'octets.
            h ^= (uint32_t)total; h *= 16777619u;
            snprintf(cache, sizeof(cache), "%08x", (unsigned)h);
        }
        String j = String("{\"empreinte\":\"") + cache + "\",\"total\":"
                 + (int)ComponentRegistry::count() + "}";
        request->send(200, "application/json", j);
    });

    server.on("/api/components/definitions", HTTP_GET, [](AsyncWebServerRequest* request) {
#ifdef NIDMI_COMPONENT_DEFS_PAGINATION
        Serial.printf("[API] REQ definitions heap=%d\n", (int)ESP.getFreeHeap());
        int page = 0, limit = 5;
        if (request->hasParam("page")) {
            page = request->getParam("page")->value().toInt();
            if (page < 0) page = 0;
        }
        /* Ignore le paramètre limit du client : on force 1 composant par page.
         * Raison : sur C3 avec ~29 KB heap libre, une copie String de 8 KB échoue
         * silencieusement quand le heap est fragmenté → JSON vide → boucle infinie.
         * Avec limit=1, le JSON max est ~5 KB (un seul composant). */
        (void)limit;
        limit = 1;

        const int totalCount = static_cast<int>(ComponentRegistry::count());
        int totalPages = totalCount; // 1 composant par page

        /* 10 Ko : après optimisation JSON (clés courtes + options inline), lis3dh
         * (le composant le plus lourd) tient en ~7-8 Ko. Le tampon appartient a
         * CETTE reponse et la suit jusqu'au dernier envoi, en PSRAM (voir
         * nidmi_tampon_reponse). Il etait statique et commun : 10 Ko de RAM
         * interne en permanence, et relu APRES le retour du gestionnaire quand
         * une page depasse le tampon d'envoi TCP — une deuxieme requete aurait
         * pu le reecrire entre-temps (jamais vu : 0 page fausse sur 220 a 8
         * requetes en parallele, MESURES §152). */
        constexpr size_t kTampon = 10240;
        std::shared_ptr<char> tampon = nidmi_tampon_reponse(kTampon);
        if (!tampon) {
            request->send(503, "application/json", "{\"error\":\"memoire\"}");
            return;
        }
        int written = ComponentRegistry::toJsonArrayPage(tampon.get(), kTampon, page, limit);

        if (written > 2 && written < (int)kTampon - 2) {
            Serial.printf("[API] page=%d/%d written=%d heap=%d\n",
                page, totalPages-1, written, (int)ESP.getFreeHeap());
            AsyncWebServerResponse* response =
                nidmi_reponse_tampon(request, "application/json", tampon, (size_t)written);
            response->addHeader("X-Total-Count", String(totalCount));
            response->addHeader("X-Total-Pages", String(totalPages));
            response->addHeader("X-Current-Page", String(page));
            response->addHeader("X-Per-Page", String(limit));
            request->send(response);
        } else {
            Serial.printf("[API] ERREUR page=%d written=%d heap=%d\n",
                page, written, (int)ESP.getFreeHeap());
            request->send(500, "application/json", "{\"error\":\"Serialization failed\"}");
        }
#else
        /* Mode normal (sans pagination)
         * 32 Ko pour les définitions avec MIDI params, formFields, etc. Chaque
         * composant peut prendre ~500-2000 bytes selon sa complexité. Tampon
         * propre a la reponse, en PSRAM (voir nidmi_tampon_reponse). */
        constexpr size_t kTampon = 32768;
        std::shared_ptr<char> tampon = nidmi_tampon_reponse(kTampon);
        if (!tampon) {
            request->send(503, "application/json", "{\"error\":\"memoire\"}");
            return;
        }
        char* jsonBuffer = tampon.get();
        jsonBuffer[0] = '\0';

        int written = ComponentRegistry::toJsonArray(jsonBuffer, kTampon);

        /* Sécurité : s'assurer que written correspond au contenu réel (strlen).
         * Protège contre un compteur written désynchronisé du buffer réel
         * (par ex. si snprintf a tronqué mais que toJson n'a pas détecté). */
        size_t actualLen = strlen(jsonBuffer);
        if (written > 0 && actualLen > 0 && actualLen < kTampon) {
            if ((size_t)written != actualLen) {
                Serial.printf("[ComponentsAPI] WARNING: written=%d != strlen=%zu, utilisation strlen\n", written, actualLen);
            }
            /* Envoyer avec la longueur réelle (strlen), pour ne pas envoyer de garbage */
            request->send(nidmi_reponse_tampon(request, "application/json", tampon, actualLen));
        } else {
            Serial.printf("[ComponentsAPI] WARNING: Serialization failed (written=%d, strlen=%zu, bufSize=%zu)\n",
                         written, actualLen, kTampon);
            request->send(500, "application/json", "{\"error\":\"Buffer too small for component definitions\"}");
        }
#endif
    });
    
    /**
     * GET /api/components/count
     * Retourne le nombre de composants enregistrés
     */
    server.on("/api/components/count", HTTP_GET, [](AsyncWebServerRequest* request) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "{\"count\":%zu}", ComponentRegistry::count());
        request->send(200, "application/json", buffer);
    });
    
    /**
     * GET /api/components/used-gpios
     * Retourne la liste des GPIOs utilisés par tous les composants configurés
     * 
     * Réponse:
     * {
     *   "gpios": [1, 2, 3, 4, 5, ...],
     *   "details": [
     *     {"gpio": 1, "componentId": "potentiometer", "pinLabel": "A0"},
     *     {"gpio": 2, "componentId": "mux", "pinId": "s0", "muxId": 0},
     *     ...
     *   ]
     * }
     */
    server.on("/api/components/used-gpios", HTTP_GET, [](AsyncWebServerRequest* request) {

        // Créer un set des GPIOs utilisés
        std::set<uint8_t> usedGpios;
        
        // 1. Ajouter les GPIOs des composants simples + GPIOs spécifiques (CS pour SPI IMU)
        uint8_t componentCount = g_componentManager.getComponentCount();
        for (uint8_t i = 0; i < componentCount; i++) {
            const ComponentConfig* cfg = g_componentManager.getConfig(i);
            if (cfg) {
                usedGpios.insert(cfg->gpio);
                if (cfg->type == ComponentType::IMU && cfg->specificConfig.imu) {
                    uint8_t cs = cfg->specificConfig.imu->cs_gpio;
                    if (cs != 255) usedGpios.insert(cs);
                }
            }
        }
        
        // 2. Ajouter les GPIOs des MUX (SIG + S0-S3 + EN)
        uint8_t muxCount = g_componentManager.getMuxCount();
        for (uint8_t i = 0; i < muxCount; i++) {
            const MuxConfig* muxCfg = g_componentManager.getMuxConfig(i);
            if (muxCfg && muxCfg->enabled) {
                usedGpios.insert(muxCfg->sig_pin);
                usedGpios.insert(muxCfg->s0);
                usedGpios.insert(muxCfg->s1);
                usedGpios.insert(muxCfg->s2);
                usedGpios.insert(muxCfg->s3);
                if (muxCfg->en_pin != 255) {
                    usedGpios.insert(muxCfg->en_pin);
                }
            }
        }
        
        // Générer le JSON : quelques dizaines de broches au plus, une petite
        // chaine suffit (un tampon statique de 2 Ko l'occupait en permanence).
        String json = "{\"gpios\":[";
        bool first = true;
        for (uint8_t gpio : usedGpios) {
            if (!first) json += ',';
            first = false;
            json += String((int)gpio);
        }
        json += "]}";
        request->send(200, "application/json", json);
    });
}
