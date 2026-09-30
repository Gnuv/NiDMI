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
#include <stdarg.h>

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

/* ── UNE REPONSE ECRITE EN PSRAM ─────────────────────────────────────────
 * Le tas interne est le reservoir qui decide (§150) : une reponse qui grandit
 * par reallocations y laisserait des trous. Un tampon PSRAM, rempli une fois,
 * part tel quel (nidmi_reponse_tampon). `deborde` : le tampon etait trop petit —
 * la reponse est a refuser, pas a tronquer. */
struct Ecrit {
    std::shared_ptr<char> t;
    size_t cap = 0, n = 0;
    bool deborde = false;
    void brut(const char* s, size_t k) {
        if (deborde || n + k >= cap) { deborde = true; return; }
        memcpy(t.get() + n, s, k); n += k;
    }
    void ajouter(const char* s) { brut(s, strlen(s)); }
    void formater(const char* fmt, ...) {
        if (deborde) return;
        va_list a; va_start(a, fmt);
        const int k = vsnprintf(t.get() + n, cap - n, fmt, a);
        va_end(a);
        if (k < 0 || n + (size_t)k >= cap) { deborde = true; return; }
        n += (size_t)k;
    }
    void chaine(const char* s) {                       // une chaine JSON, echappee
        ajouter("\"");
        for (; *s; s++) {
            if (*s == '"' || *s == '\\') { const char e[2] = {'\\', *s}; brut(e, 2); }
            else if ((uint8_t)*s < 0x20) formater("\\u%04x", (unsigned)(uint8_t)*s);
            else brut(s, 1);
        }
        ajouter("\"");
    }
};

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
