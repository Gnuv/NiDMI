#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <Preferences.h>
#include "../nidmi_debug.h"
#include "../Globals.h"
#include "../config/ConfigCache.h"






#include <esp_heap_caps.h>
#include <memory>

/* ── UN TAMPON DE REPONSE QUI VIT JUSQU'AU DERNIER ENVOI ─────────────────
 * Une reponse longue part par morceaux, APRES le retour du gestionnaire. Son
 * tampon doit donc vivre jusqu'au dernier morceau — et n'appartenir qu'a elle :
 * un tampon statique commun peut etre reecrit par la requete suivante pendant
 * que la precedente part encore. Et il se prend en PSRAM : le tas interne est le
 * reservoir qui decide (MESURES §150), la PSRAM a des megaoctets libres. A
 * defaut de PSRAM, la RAM interne. Libere par la reponse elle-meme. */
inline std::shared_ptr<char> nidmi_tampon_reponse(size_t taille) {
    char* p = (char*)heap_caps_malloc(taille, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (char*)heap_caps_malloc(taille, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) return nullptr;
    return std::shared_ptr<char>(p, [](char* q) { heap_caps_free(q); });
}

/** La reponse qui lit `n` octets de ce tampon, sans copie intermediaire. */
inline AsyncWebServerResponse* nidmi_reponse_tampon(AsyncWebServerRequest* request, const char* type,
                                                    std::shared_ptr<char> tampon, size_t n) {
    return request->beginResponse(type, n, [tampon, n](uint8_t* out, size_t maxLen, size_t index) -> size_t {
        const size_t k = (n - index < maxLen) ? (n - index) : maxLen;
        memcpy(out, tampon.get() + index, k);
        return k;
    });
}

/* ── UNE CHAINE DANS DU JSON, ECHAPPEE EN ENTIER ─────────────────────────
 * Pas seulement « \ » et « " » : un script de mapping tient sur plusieurs
 * lignes, et un saut de ligne BRUT dans une chaine JSON est invalide — la
 * config partait telle quelle en NVS, /api/pins/list la recrachait, JSON.parse
 * echouait cote app et TOUTE la zone d'inventaire I/O restait vide. Un mot de
 * passe WiFi, lui, peut contenir n'importe quel caractere imprimable. D'ou les
 * caracteres de controle aussi. */
inline String nidmi_json_chaine(const String& v) {
    String out;
    out.reserve(v.length() + 8);
    for (unsigned i = 0; i < v.length(); i++) {
        const char c = v[i];
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char u[8];
                    snprintf(u, sizeof u, "\\u%04x", (unsigned)(unsigned char)c);
                    out += u;
                } else out += c;
        }
    }
    return out;
}
