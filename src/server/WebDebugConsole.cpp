#include "WebDebugConsole.h"
#include "ServerCore.h"
#include "../diag/JournalAvant.h"
#include <cstdarg>
#include <cstdio>

#if NIDMI_WEB_DEBUG_CONSOLE

#include <AsyncWebSocket.h>
#include <cstdarg>
#include <cstring>
#include <esp_heap_caps.h>

static AsyncWebSocket* g_ws = nullptr;
/* Qui est abonne : le registre des onglets le sait, un onglet a la fois
 * (MESURES §200). Ici, il n'y avait qu'un drapeau pour tous. */

enum : uint16_t {
    kRingLines = 48,
    kLineCap = 200,
};

/* L'historique en PSRAM, pris au premier besoin : 9,6 Ko qu'un tableau
 * statique retirait au tas interne en permanence (MESURES §152). Le premier
 * appel peut venir de n'importe quelle tache, deux a la fois : l'adresse se
 * pose par echange atomique, et le perdant rend son tampon. Sans PSRAM, la
 * ligne ne va qu'au port serie. */
typedef char LigneRing[kLineCap];
static LigneRing* g_ring = nullptr;
static uint16_t g_start = 0;
static uint16_t g_size = 0;

static LigneRing* ring() {
    LigneRing* r = __atomic_load_n(&g_ring, __ATOMIC_ACQUIRE);
    if (r) return r;
    LigneRing* neuf = (LigneRing*)heap_caps_calloc(kRingLines, sizeof(LigneRing), MALLOC_CAP_SPIRAM);
    if (!neuf) return nullptr;
    LigneRing* attendu = nullptr;
    if (!__atomic_compare_exchange_n(&g_ring, &attendu, neuf, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        heap_caps_free(neuf);
        return attendu;
    }
    return neuf;
}

static void ring_push(const char* line) {
    LigneRing* r = ring();
    if (!r) return;
    uint16_t pos;
    if (g_size < kRingLines) {
        pos = (g_start + g_size) % kRingLines;
        ++g_size;
    } else {
        pos = g_start;
        g_start = (g_start + 1) % kRingLines;
    }
    strncpy(r[pos], line, kLineCap - 1);
    r[pos][kLineCap - 1] = '\0';
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
 * deconnecte) et la boucle principale draine, en respectant canSend().
 *
 * UNE FILE D'ATTENTE, PAS UNE PLACE (MESURES §200). Il n'y avait qu'une place :
 * chaque abonnement prenait celle du precedent, dont le rejeu s'arretait la —
 * sans la fin (DEBUG_CONSOLE_STATE:1), que l'app attend pour reprendre les lignes.
 * Apres un redemarrage, tous les onglets se reconnectent ensemble : un seul
 * retrouvait sa console. Chacun attend maintenant son tour, et recoit tout.
 * Seize places, comme le registre des onglets ; la tete est celle qu'on sert. Ecrite par
 * async_tcp (l'abonnement), lue et avancee par loopTask (la pompe) : sous une
 * section critique, quelques instructions, jamais un envoi dedans. */
static constexpr uint8_t kAttenteMax = 16;
static uint32_t g_attente[kAttenteMax] = {};
static uint8_t  g_nAttente   = 0;
static uint16_t g_flushIndex = 0;     // la ligne ou en est la tete
static portMUX_TYPE g_muxAttente = portMUX_INITIALIZER_UNLOCKED;

/* Retirer cet onglet de la file (servi, parti ou desabonne). */
static void retirerDeLAttente(uint32_t id) {
    taskENTER_CRITICAL(&g_muxAttente);
    for (uint8_t i = 0; i < g_nAttente; i++) {
        if (g_attente[i] != id) continue;
        for (uint8_t j = i + 1; j < g_nAttente; j++) g_attente[j - 1] = g_attente[j];
        g_nAttente--;
        if (i == 0) g_flushIndex = 0;   // la tete suivante commence au debut
        break;
    }
    taskEXIT_CRITICAL(&g_muxAttente);
}

void nidmi_web_debug_init(AsyncWebSocket* ws) {
    g_ws = ws;
    ring();   // pris ici, au demarrage : jamais par une tache temps reel au premier journal
}

bool nidmi_web_debug_is_supported() {
    return true;
}

/* Par le registre des onglets (ServerCore.h, « LES ONGLETS ») : g_ws->client()
 * parcourait la liste de la bibliotheque pendant qu'async_tcp la modifiait
 * (MESURES §171). */
void nidmi_web_debug_pump() {
    if (!g_ws) return;
    taskENTER_CRITICAL(&g_muxAttente);
    const uint32_t id  = g_nAttente ? g_attente[0] : 0;
    const uint16_t idx = g_flushIndex;
    taskEXIT_CRITICAL(&g_muxAttente);
    if (!id) return;
    /* D'abord la vie precedente (§162) : ce qu'un redemarrage a efface de
     * l'historique, la memoire RTC l'a garde. */
    const uint16_t avant = JournalAvant::nbLignesRejeu();
    if (idx >= avant + g_size) {
        // l'accuse ferme le rattrapage ; file pleine (0) : on repassera
        if (nidmi_ws_envoyer_a(id, "DEBUG_CONSOLE_STATE:1") != 0) retirerDeLAttente(id);
        return;
    }
    const char* ligne = nullptr;
    if (idx < avant) {
        ligne = JournalAvant::ligneRejeu((uint8_t)idx);
    } else {
        LigneRing* r = ring();
        if (r) ligne = r[(g_start + idx - avant) % kRingLines];
    }
    char msg[kLineCap + 16];
    snprintf(msg, sizeof(msg), "DEBUG_LOG:%s", ligne ? ligne : "");
    const int r = nidmi_ws_envoyer_a(id, msg);
    if (r < 0) { retirerDeLAttente(id); return; }   // parti : au suivant
    if (r > 0) {                                     // 0 : sa file est pleine, on repassera
        taskENTER_CRITICAL(&g_muxAttente);
        if (g_nAttente && g_attente[0] == id) g_flushIndex++;   // toujours lui en tete ?
        taskEXIT_CRITICAL(&g_muxAttente);
    }
}

void nidmi_web_debug_handle_ws_text(AsyncWebSocketClient* client, const String& message) {
    if (!client) return;
    const uint32_t id = client->id();
    if (message == "DEBUG_CONSOLE:1") {
        nidmi_ws_abonner(id, NIDMI_ABO_CONSOLE, true);
        /* On NOTE, on n'emet pas : la boucle principale s'en charge. Deja en
         * attente : sa place reste la sienne. */
        taskENTER_CRITICAL(&g_muxAttente);
        bool deja = false;
        for (uint8_t i = 0; i < g_nAttente; i++) if (g_attente[i] == id) deja = true;
        if (!deja && g_nAttente < kAttenteMax) {
            if (g_nAttente == 0) g_flushIndex = 0;
            g_attente[g_nAttente++] = id;
        }
        taskEXIT_CRITICAL(&g_muxAttente);
    } else if (message == "DEBUG_CONSOLE:0") {
        nidmi_ws_abonner(id, NIDMI_ABO_CONSOLE, false);   // lui seul : les autres gardent la leur
        retirerDeLAttente(id);
    }
}

void nidmi_web_debug_append_line(const char* line) {
    if (!line || !line[0]) {
        return;
    }
    ring_push(line);                       // TOUJOURS : c'est l'historique
    JournalAvant::noter(line);             // et ce qui survivra a un redemarrage (§162)
    if (!g_ws || !nidmi_ws_abonne(NIDMI_ABO_CONSOLE)) {
        return;                            // aucun onglet n'a la console ouverte
    }
    /* NIDMI_WEB_LOG est appele depuis N'IMPORTE QUELLE tache — MidiTask
     * comprise. On ne touche donc pas a la socket ici : on POUSSE, loopTask
     * draine (ServerCore.h). File pleine = le client ne suit pas, la ligne est
     * jetee — elle reste dans le tampon circulaire, donc un client qui s'abonne
     * ensuite la retrouvera : on ne perd que l'instant, pas la trace. C'est CE
     * chemin, combine au flot de textAll, qui fermait la socket a 26 ms
     * (MESURES §84). */
    char msg[kLineCap + 16];
    snprintf(msg, sizeof(msg), "DEBUG_LOG:%s", line);
    nidmi_ws_pousser_aux_abonnes(NIDMI_ABO_CONSOLE, msg);   // aux seuls abonnes (§200)
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
