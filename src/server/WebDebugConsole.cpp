#include "WebDebugConsole.h"
#include "ServerCore.h"
#include <cstdarg>
#include <cstdio>

#if NIDMI_WEB_DEBUG_CONSOLE

#include <AsyncWebSocket.h>
#include <cstdarg>
#include <cstring>

static AsyncWebSocket* g_ws = nullptr;
static bool g_subscribe = false;

enum : uint16_t {
    kRingLines = 48,
    kLineCap = 200,
};

static char g_ring[kRingLines][kLineCap];
static uint16_t g_start = 0;
static uint16_t g_size = 0;

static void ring_push(const char* line) {
    uint16_t pos;
    if (g_size < kRingLines) {
        pos = (g_start + g_size) % kRingLines;
        ++g_size;
    } else {
        pos = g_start;
        g_start = (g_start + 1) % kRingLines;
    }
    strncpy(g_ring[pos], line, kLineCap - 1);
    g_ring[pos][kLineCap - 1] = '\0';
}

static void send_debug_log(AsyncWebSocketClient* client, const char* line) {
    if (!line) {
        return;
    }
    char msg[kLineCap + 16];
    snprintf(msg, sizeof(msg), "DEBUG_LOG:%s", line);
    if (client) {
        client->text(msg);
    }
}

/* LE RATTRAPAGE D'HISTORIQUE SE FAIT HORS DU RAPPEL, UNE LIGNE A LA FOIS.
 *
 * Il se faisait ICI, dans le rappel WS_EVT_DATA, en poussant les 48 lignes du
 * tampon d'un coup. Deux fautes, et la connexion en mourait a tous les coups :
 *   - emettre vers un client PENDANT qu'on traite sa propre trame corrompt son
 *     etat dans ESPAsyncWebServer ;
 *   - 48 messages d'affilee debordent la file du client, que la bibliotheque
 *     ferme alors sans ceremonie.
 * Symptome : le socket se fermait 71 ms apres « DEBUG_CONSOLE:1 », code 1006,
 * sans trame de fermeture. Sans l'abonnement il vivait tres bien — c'etait donc
 * l'abonnement lui-meme qui tuait la connexion, et la console restait vide
 * alors que la carte parlait.
 *
 * On memorise donc l'ID du client (jamais son POINTEUR, qui pend des qu'il se
 * deconnecte) et la boucle principale draine, en respectant canSend(). */
static uint32_t g_flushClient = 0;
static uint16_t g_flushIndex  = 0;

void nidmi_web_debug_init(AsyncWebSocket* ws) {
    g_ws = ws;
}

bool nidmi_web_debug_is_supported() {
    return true;
}

void nidmi_web_debug_pump() {
    if (!g_flushClient || !g_ws) return;
    AsyncWebSocketClient* c = g_ws->client(g_flushClient);
    if (!c || c->status() != WS_CONNECTED) { g_flushClient = 0; return; }
    if (!c->canSend()) return;                 // file pleine : on repassera
    if (g_flushIndex >= g_size) {
        c->text("DEBUG_CONSOLE_STATE:1");      // l'accuse ferme le rattrapage
        g_flushClient = 0;
        return;
    }
    send_debug_log(c, g_ring[(g_start + g_flushIndex) % kRingLines]);
    ++g_flushIndex;
}

void nidmi_web_debug_handle_ws_text(AsyncWebSocketClient* client, const String& message) {
    if (message == "DEBUG_CONSOLE:1") {
        g_subscribe = true;
        /* On NOTE, on n'emet pas : la boucle principale s'en charge. */
        g_flushClient = client ? client->id() : 0;
        g_flushIndex  = 0;
    } else if (message == "DEBUG_CONSOLE:0") {
        g_subscribe = false;
        g_flushClient = 0;
    }
}

void nidmi_web_debug_append_line(const char* line) {
    if (!line || !line[0]) {
        return;
    }
    ring_push(line);                       // TOUJOURS : c'est l'historique
    if (!g_subscribe || !g_ws) {
        return;
    }
    /* Meme regle que print()/graph() : si le client ne suit pas, on jette la
     * ligne plutot que de l'empiler. Elle reste dans le tampon circulaire, donc
     * un client qui s'abonne ensuite la retrouvera — on ne perd que l'instant,
     * pas la trace. C'est CE chemin, combine au flot de textAll, qui fermait la
     * socket a 26 ms (MESURES §84). */
    if (!nidmi_ws_peut_emettre(*g_ws)) {
        return;
    }
    char msg[kLineCap + 16];
    snprintf(msg, sizeof(msg), "DEBUG_LOG:%s", line);
    g_ws->textAll(msg);
}

#endif

// Log "tee" : toujours compilé (Serial sur toutes les cartes, + console si S3).
void nidmi_web_debug_log(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';

    Serial.println(line);
#if NIDMI_WEB_DEBUG_CONSOLE
    nidmi_web_debug_append_line(line);
#endif
}
