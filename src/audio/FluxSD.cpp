#include "FluxSD.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <new>

#include "SampleStore.h"
#include "../config/CarteSd.h"
#include "../server/WebDebugConsole.h"

namespace FluxSD {
namespace {

// ── Les tetes ────────────────────────────────────────────────────────────────
struct Tete {
  char           son[SampleStore::NOM_MAX];
  uint32_t       debut   = 0;
  uint32_t       trames  = 0;
  uint32_t       vuMs    = 0;
  int16_t*       pcm     = nullptr;
  volatile uint8_t etat  = 0;           // 0 libre, 1 demandee, 2 prete, 3 en echec
  volatile bool  urgente = false;       // une note l'attend : a lire avant les autres
  volatile bool  anticipee = false;     // demandee pour la cue SUIVANTE (promue des qu'une demande courante la nomme)
  uint32_t       octets  = 0;           // ce qu'elle pese dans le budget (0 : en echec, rien de pris)
};
Tete* _tetes = nullptr;               // TETES_MAX, en PSRAM, pris au premier besoin (voir _prendreTables)

/* La table des tetes change sous plusieurs taches (la cue, le serveur web, la tache de
 * la carte) : un verrou court, jamais tenu pendant une lecture. */
StaticSemaphore_t _tampT;
SemaphoreHandle_t _verrouT = xSemaphoreCreateMutexStatic(&_tampT);
struct VerrouT {
  VerrouT()  { if (_verrouT) xSemaphoreTake(_verrouT, portMAX_DELAY); }
  ~VerrouT() { if (_verrouT) xSemaphoreGive(_verrouT); }
};

String _chemin(const char* son) { return String(CarteSd::DOSSIER) + "/" + son; }

/* Lire la tete i : ouvrir, se placer, lire TETE_TRAMES — dans la tache de la carte. */
bool _chargerTete(uint8_t i) {                       // faux : rien n'a change (la carte n'est pas la)
  Tete& t = _tetes[i];
  if (!CarteSd::monte()) return false;              // la carte n'est pas la : on reste « demandee »
  const int s = SampleStore::indexDe(t.son);
  if (s < 0 || !SampleStore::estFlux((uint8_t)s)) {
    NIDMI_WEB_LOG("[SD] tete de %s : ce n'est pas un son lu en flux", t.son);
    t.octets = 0; t.etat = 3; return true;
  }
  const uint32_t total = (uint32_t)SampleStore::trames((uint8_t)s);
  const uint8_t  ca    = SampleStore::stereo((uint8_t)s) ? 2 : 1;
  if (t.debut >= total) {
    NIDMI_WEB_LOG("[SD] tete de %s : le debut (trame %lu) est au-dela de la fin", t.son, (unsigned long)t.debut);
    t.octets = 0; t.etat = 3; return true;
  }
  const uint32_t n = (total - t.debut < TETE_TRAMES) ? total - t.debut : TETE_TRAMES;
  File f = CarteSd::ouvrir(_chemin(t.son).c_str());
  if (!f) { NIDMI_WEB_LOG("[SD] tete de %s : ouverture impossible", t.son); t.octets = 0; t.etat = 3; return true; }
  int16_t* pcm = (int16_t*)heap_caps_malloc((size_t)n * ca * 2, MALLOC_CAP_SPIRAM);
  if (!pcm) { f.close(); NIDMI_WEB_LOG("[SD] tete de %s : PSRAM insuffisante", t.son); t.octets = 0; t.etat = 3; return true; }
  const uint32_t t0 = millis();
  const size_t   octets = (size_t)n * ca * 2;
  size_t lus = 0;
  if (f.seek(SampleStore::offsetDonnees((uint8_t)s) + (size_t)t.debut * ca * 2)) {
    while (lus < octets) {
      const size_t pas = (octets - lus < 16384) ? octets - lus : 16384;
      const size_t k = f.read((uint8_t*)pcm + lus, pas);
      if (!k) break;
      lus += k;
      vTaskDelay(1);                                  // cede : l'IDLE du coeur 0
    }
  }
  f.close();
  if (lus != octets) {
    heap_caps_free(pcm);
    NIDMI_WEB_LOG("[SD] tete de %s : lecture incomplete (%u / %u o)", t.son, (unsigned)lus, (unsigned)octets);
    t.octets = 0; t.etat = 3; return true;
  }
  t.pcm = pcm; t.trames = n;
  __sync_synchronize();
  t.etat = 2;
  NIDMI_WEB_LOG("[SD] tete de %s @%lu : %lu trames en %lu ms", t.son, (unsigned long)t.debut,
                (unsigned long)n, (unsigned long)(millis() - t0));
  return true;
}

// ── Les flux ─────────────────────────────────────────────────────────────────
struct Flux {
  volatile uint8_t etat = 0;            // 0 libre, 1 actif
  uint8_t          voix = 0;
  uint8_t          canaux = 2;
  char             son[SampleStore::NOM_MAX] = {0};
  uint32_t         offset = 0;          // octets : debut des donnees dans le fichier
  uint32_t         depart = 0, fin = 0; // trames : le tampon se remplit de [depart, fin)
  volatile uint32_t seq = 0;            // ecrits par l'audio
  volatile uint32_t cons = 0;
  volatile uint32_t seqVue = 0;         // ecrits par le lecteur
  volatile uint32_t lo = 0, hi = 0;     // [lo, hi) : les trames valides du tampon
  volatile uint32_t sousAlim = 0, sauts = 0, departs = 0;
  int16_t*         anneau = nullptr;    // ANNEAU_TRAMES x 2 int16, en PSRAM, pris une fois
  // propres au lecteur :
  File             f;
  char             ouvert[SampleStore::NOM_MAX] = {0};
  uint32_t         finLue = 0;          // la fin qu'il vise (copie de `fin` a la derniere demande)
};
Flux* _fl = nullptr;                  // FLUX_MAX, idem
volatile uint32_t _manquesTotal = 0;
volatile uint32_t _notesRetardees = 0, _notesAbandonnees = 0, _attentePireMs = 0;
volatile uint32_t _refus = 0, _refusAnticipes = 0;
volatile uint32_t _lecteurVit = 0;
volatile bool     _arretLecteur = false;

/* LES TABLES SONT EN PSRAM. Quelques centaines d'octets chacune, mais le plus gros bloc
 * contigu de la RAM interne est la ressource rare (RESSOURCES_CARTE.md) : un `.bss` de
 * plus d'un ko lui coutait 1 024 o, mesure. Prises au premier clip en flux — jamais par
 * la tache audio —, sous verrou, et jamais rendues. */
bool _prendreTables() {
  if (_tetes && _fl) return true;
  if (!_verrouT) return false;
  xSemaphoreTake(_verrouT, portMAX_DELAY);
  if (!_tetes) _tetes = (Tete*)heap_caps_calloc(TETES_MAX, sizeof(Tete), MALLOC_CAP_SPIRAM);
  if (!_fl) {
    void* m = heap_caps_calloc(FLUX_MAX, sizeof(Flux), MALLOC_CAP_SPIRAM);
    if (m) _fl = new (m) Flux[FLUX_MAX];            // File a un constructeur : on le lance
  }
  const bool ok = _tetes && _fl;
  xSemaphoreGive(_verrouT);
  return ok;
}

void _lancerLecteur();

/* SERVIR UN FLUX : le placer s'il y a une demande, puis le remplir tant qu'il y a de la
 * place. Un morceau a la fois (MORCEAU_TRAMES) — la tache rend la main entre deux. */
bool _servir(Flux& x) {
  uint32_t s = x.seq;
  if (s != x.seqVue) {                                    // une (re)demande : se placer
    char     son[SampleStore::NOM_MAX];
    strlcpy(son, x.son, sizeof(son));
    const uint32_t dep = x.depart, fin = x.fin, off = x.offset;
    const uint8_t  ca = x.canaux;
    if (x.seq != s) return true;                          // elle a change pendant la copie : au tour suivant
    if (strcmp(son, x.ouvert) || !x.f) {
      if (x.f) x.f.close();
      x.ouvert[0] = 0;
      x.f = CarteSd::ouvrir(_chemin(son).c_str());
      if (x.f) strlcpy(x.ouvert, son, sizeof(x.ouvert));
    }
    x.lo = dep; x.hi = dep; x.finLue = fin;
    if (!x.f || !x.f.seek(off + (size_t)dep * ca * 2)) {
      NIDMI_WEB_LOG("[SD] flux de %s : placement impossible a la trame %lu", son, (unsigned long)dep);
      x.finLue = dep;                                     // rien a lire : silence
    }
    x.departs = x.departs + 1;
    __sync_synchronize();
    x.seqVue = s;
    return true;
  }
  uint32_t hi = x.hi;
  if (hi >= x.finLue || !x.f) return false;               // tout est lu
  const uint32_t cons = x.cons;
  /* LA VOIX NOUS A DEPASSES : elle a lu au-dela de ce que nous avions. On saute en
   * avant plutot que de lire ce qu'elle ne rejouera jamais — du silence entre-temps. */
  if (cons >= x.depart && cons > hi) {
    const uint32_t nv = cons + SAUT_TRAMES;
    if (nv >= x.finLue) { x.hi = x.lo = x.finLue; return false; }
    x.lo = nv; __sync_synchronize(); x.hi = nv;
    if (!x.f.seek(x.offset + (size_t)nv * x.canaux * 2)) { x.finLue = nv; return false; }
    x.sauts = x.sauts + 1;
    hi = nv;
  }
  const uint32_t base   = (cons > x.lo) ? cons : x.lo;
  uint32_t       limite = base + ANNEAU_TRAMES - GARDE_TRAMES;
  if (limite > x.finLue) limite = x.finLue;
  if (hi >= limite) return false;                         // le tampon est plein
  uint32_t n = limite - hi;
  if (n > MORCEAU_TRAMES) n = MORCEAU_TRAMES;
  const uint32_t jusquAuBout = ANNEAU_TRAMES - (hi & (ANNEAU_TRAMES - 1));
  if (n > jusquAuBout) n = jusquAuBout;                   // ne pas enjamber la fin du tampon
  const size_t octets = (size_t)n * x.canaux * 2;
  int16_t* dest = x.anneau + (size_t)(hi & (ANNEAU_TRAMES - 1)) * x.canaux;
  const size_t lus = x.f.read((uint8_t*)dest, octets);
  const uint32_t tr = (uint32_t)(lus / ((size_t)x.canaux * 2));
  __sync_synchronize();
  x.hi = hi + tr;
  if (lus != octets) {                                    // fin du fichier, ou erreur : on s'arrete la
    x.finLue = hi + tr;
    return false;
  }
  return true;
}

void _tacheLecteur(void*) {
  uint32_t inactifDepuis = millis();
  for (;;) {
    if (_arretLecteur || !_fl) break;                      // la carte se demonte : on rend tout
    bool actif = false, travail = false;
    for (uint8_t i = 0; i < FLUX_MAX; i++) {
      Flux& x = _fl[i];
      if (x.etat == 1) { actif = true; if (_servir(x)) travail = true; }
      else if (x.f) { x.f.close(); x.ouvert[0] = 0; }     // un flux rendu : son fichier aussi
    }
    if (actif) inactifDepuis = millis();
    else if (millis() - inactifDepuis > 1500) break;
    vTaskDelay(travail ? 1 : pdMS_TO_TICKS(4));            // jamais de tour a vide
  }
  if (_fl) for (uint8_t i = 0; i < FLUX_MAX; i++) if (_fl[i].f) { _fl[i].f.close(); _fl[i].ouvert[0] = 0; }
  __sync_lock_release(&_lecteurVit);
  bool reste = false;
  if (_fl && !_arretLecteur) for (uint8_t i = 0; i < FLUX_MAX; i++) if (_fl[i].etat == 1) reste = true;
  if (reste) _lancerLecteur();
  vTaskDelete(nullptr);
}

/* La tache du lecteur : coeur 0, priorite 3 — au-dessus de la tache de la carte (1) qui
 * charge et mesure, sous le WiFi, la pile TCP/IP, l'audio et la boucle. Naissante et
 * mourante : pas de pile gardee en RAM interne quand rien ne se lit. */
void _lancerLecteur() {
  if (__sync_lock_test_and_set(&_lecteurVit, 1)) return;
  if (xTaskCreatePinnedToCore(_tacheLecteur, "flux-sd", 6144, nullptr, 3, nullptr, 0) != pdPASS) {
    __sync_lock_release(&_lecteurVit);
    NIDMI_WEB_LOG("[SD] lecteur de flux impossible (memoire)");
  }
}

}  // namespace

// ── Les tetes : API ──────────────────────────────────────────────────────────
int8_t demanderTete(const char* son, uint32_t debut, bool anticipee) {
  if (!_prendreTables()) return TETE_INCONNUE;
  /* Ce que cette tete pesera : lu dans l'emplacement « flux » du magasin (duree, canaux). */
  const int sl = SampleStore::indexDe(son);
  if (sl < 0 || !SampleStore::estFlux((uint8_t)sl)) return TETE_INCONNUE;
  const uint32_t total = (uint32_t)SampleStore::trames((uint8_t)sl);
  if (debut >= total) return TETE_INCONNUE;
  const uint32_t octets = ((total - debut < TETE_TRAMES) ? total - debut : TETE_TRAMES)
                        * (SampleStore::stereo((uint8_t)sl) ? 2u : 1u) * 2u;
  int8_t idx = TETE_INCONNUE;
  bool neuve = false;
  {
    VerrouT v;
    int8_t libre = -1;
    uint32_t pris = 0, anticipes = 0;
    for (int8_t i = 0; i < (int8_t)TETES_MAX; i++) {
      Tete& t = _tetes[i];
      if (t.etat == 0) { if (libre < 0) libre = i; continue; }
      pris += t.octets;
      if (t.anticipee) anticipes += t.octets;
      if (idx == TETE_INCONNUE && t.debut == debut && !strcmp(t.son, son)) {
        t.vuMs = millis();
        if (!anticipee) t.anticipee = false;              // une demande courante la PROMEUT
        idx = i;
      }
    }
    if (idx == TETE_INCONNUE) {
      const bool placeSurLaTable = libre >= 0;
      const bool dansLeBudget = pris + octets <= TETES_BUDGET_OCTETS
                             && (!anticipee || anticipes + octets <= ANTICIPE_BUDGET_OCTETS);
      const bool psramOk = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= PSRAM_RESERVE_OCTETS + octets;
      if (!placeSurLaTable || !dansLeBudget || !psramOk) {
        if (anticipee) _refusAnticipes = _refusAnticipes + 1;
        idx = TETE_BUDGET_PLEIN;
      } else {
        Tete& t = _tetes[libre];
        strlcpy(t.son, son, sizeof(t.son));
        t.debut = debut; t.trames = 0; t.pcm = nullptr; t.vuMs = millis();
        t.octets = octets; t.urgente = false; t.anticipee = anticipee;
        __sync_synchronize();
        t.etat = 1;
        idx = libre; neuve = true;
      }
    }
  }
  if (neuve) CarteSd::chargerTetes();
  return idx;
}

bool teteAnticipee(int8_t i) { return _tetes && i >= 0 && i < (int8_t)TETES_MAX && _tetes[i].etat != 0 && _tetes[i].anticipee; }
bool teteEvincable(int8_t i) { return _tetes && i >= 0 && i < (int8_t)TETES_MAX && (_tetes[i].etat == 2 || _tetes[i].etat == 3); }
void noterRefus() { _refus = _refus + 1; }

bool tetePrete(int8_t i) { return _tetes && i >= 0 && i < (int8_t)TETES_MAX && _tetes[i].etat == 2; }

bool teteVue(int8_t i, TeteVue& v) {
  if (!tetePrete(i)) return false;
  v.pcm = _tetes[i].pcm; v.debut = _tetes[i].debut; v.trames = _tetes[i].trames;
  return true;
}

uint32_t teteAgeMs(int8_t i) {
  if (!_tetes || i < 0 || i >= (int8_t)TETES_MAX || _tetes[i].etat == 0) return 0;
  return millis() - _tetes[i].vuMs;
}
bool teteUtilisee(int8_t i) { return _tetes && i >= 0 && i < (int8_t)TETES_MAX && _tetes[i].etat != 0; }

void libererTete(int8_t i) {
  if (!_tetes || i < 0 || i >= (int8_t)TETES_MAX) return;
  VerrouT v;
  Tete& t = _tetes[i];
  int16_t* pcm = t.pcm;
  t.etat = 0;
  __sync_synchronize();
  t.pcm = nullptr; t.trames = 0; t.octets = 0; t.anticipee = false; t.urgente = false;
  if (pcm) heap_caps_free(pcm);
}

/* Une tete a la fois, L'URGENTE D'ABORD : celle qu'une note attend passe devant les autres
 * (elle attend alors une tete en cours de lecture, au plus, plus la sienne). Une carte absente
 * arrete la boucle — les tetes restent « demandees ». */
void chargerTetes() {
  if (!_tetes) return;
  for (;;) {
    int8_t choix = -1;
    for (uint8_t i = 0; i < TETES_MAX && choix < 0; i++) if (_tetes[i].etat == 1 && _tetes[i].urgente) choix = (int8_t)i;
    for (uint8_t i = 0; i < TETES_MAX && choix < 0; i++) if (_tetes[i].etat == 1 && !_tetes[i].anticipee) choix = (int8_t)i;
    for (uint8_t i = 0; i < TETES_MAX && choix < 0; i++) if (_tetes[i].etat == 1) choix = (int8_t)i;   // les anticipees, en dernier
    if (choix < 0) break;
    if (!_chargerTete((uint8_t)choix)) break;
  }
}

bool teteEnCours(int8_t i) { return _tetes && i >= 0 && i < (int8_t)TETES_MAX && _tetes[i].etat == 1; }
void prioriser(int8_t i)   { if (teteEnCours(i)) _tetes[i].urgente = true; }

/* Les notes qui ont attendu leur tete : combien, la plus longue attente, et celles qu'on a dues
 * abandonner. Ecrits par la tache audio — de simples compteurs, jamais un journal. */
void noterAttente(uint32_t ms) {
  _notesRetardees = _notesRetardees + 1;
  if (ms > _attentePireMs) _attentePireMs = ms;
}
void noterAbandon() { _notesAbandonnees = _notesAbandonnees + 1; }

// ── Les flux : API ───────────────────────────────────────────────────────────
int8_t acquerir(uint8_t voix) {
  if (!_fl) return -1;
  for (int8_t i = 0; i < (int8_t)FLUX_MAX; i++) {
    Flux& x = _fl[i];
    if (x.etat != 0) continue;
    if (!x.anneau) {
      x.anneau = (int16_t*)heap_caps_malloc((size_t)ANNEAU_TRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
      if (!x.anneau) { NIDMI_WEB_LOG("[SD] flux : PSRAM insuffisante pour un tampon"); return -1; }
    }
    x.voix = voix; x.lo = x.hi = 0; x.cons = 0;
    x.sousAlim = 0; x.sauts = 0; x.departs = 0;
    x.seqVue = x.seq;                   // rien a servir tant que `demarrer` n'a pas parle
    __sync_synchronize();
    x.etat = 1;
    return i;
  }
  return -1;
}

bool    possede(int8_t f, uint8_t voix) { return _fl && f >= 0 && f < (int8_t)FLUX_MAX && _fl[f].etat == 1 && _fl[f].voix == voix; }
bool    actif(int8_t f)   { return _fl && f >= 0 && f < (int8_t)FLUX_MAX && _fl[f].etat == 1; }
uint8_t voixDe(int8_t f)  { return (_fl && f >= 0 && f < (int8_t)FLUX_MAX) ? _fl[f].voix : 0; }

void liberer(int8_t f) {
  if (!_fl || f < 0 || f >= (int8_t)FLUX_MAX) return;
  _fl[f].etat = 0;                       // le lecteur ferme son fichier a son prochain tour
}

void demarrer(int8_t f, const char* son, uint32_t depart, uint32_t fin, uint8_t canaux, uint32_t offsetOctets) {
  if (!_fl || f < 0 || f >= (int8_t)FLUX_MAX) return;
  Flux& x = _fl[f];
  strlcpy(x.son, son, sizeof(x.son));
  x.offset = offsetOctets; x.canaux = canaux;
  x.depart = depart; x.fin = fin;
  x.cons = 0;
  __sync_synchronize();
  x.seq = x.seq + 1;
  _lancerLecteur();
}

void redemarrer(int8_t f) {
  if (!_fl || f < 0 || f >= (int8_t)FLUX_MAX) return;
  Flux& x = _fl[f];
  x.cons = 0;
  __sync_synchronize();
  x.seq = x.seq + 1;
  _lancerLecteur();
}

void position(int8_t f, uint32_t trame) {
  if (_fl && f >= 0 && f < (int8_t)FLUX_MAX) _fl[f].cons = trame;
}

bool lire(int8_t f, uint32_t trame, int16_t& g, int16_t& d) {
  if (!_fl || f < 0 || f >= (int8_t)FLUX_MAX) return false;
  const Flux& x = _fl[f];
  if (x.seqVue != x.seq || !x.anneau) return false;
  const uint32_t hi = x.hi, lo = x.lo;
  const uint32_t bas = (hi - lo > ANNEAU_TRAMES) ? hi - ANNEAU_TRAMES : lo;
  if (trame < bas || trame >= hi) return false;
  const int16_t* p = x.anneau + (size_t)(trame & (ANNEAU_TRAMES - 1)) * x.canaux;
  if (x.canaux == 2) { g = p[0]; d = p[1]; } else { g = d = p[0]; }
  return true;
}

/* La carte se demonte : plus aucun flux, plus aucun fichier ouvert. FatFs ne tient pas
 * qu'on demonte sous un fichier ouvert ; le lecteur ferme les siens et s'arrete. Les voix
 * qui jouaient finissent ce que leur tampon contient, puis se taisent. */
void arreterTout() {
  if (_fl) for (uint8_t i = 0; i < FLUX_MAX; i++) _fl[i].etat = 0;
  if (!_lecteurVit) return;
  _arretLecteur = true;
  for (int i = 0; i < 50 && _lecteurVit; i++) vTaskDelay(pdMS_TO_TICKS(10));
  _arretLecteur = false;
}

void manque(int8_t f) {
  if (_fl && f >= 0 && f < (int8_t)FLUX_MAX) _fl[f].sousAlim = _fl[f].sousAlim + 1;
  _manquesTotal = _manquesTotal + 1;
}

String diagnostic() {
  String j = "{\"lecteur\":" + String(_lecteurVit ? "true" : "false")
           + ",\"manques_total\":" + String((unsigned long)_manquesTotal)
           + ",\"notes_retardees\":" + String((unsigned long)_notesRetardees)
           + ",\"attente_pire_ms\":" + String((unsigned long)_attentePireMs)
           + ",\"notes_abandonnees\":" + String((unsigned long)_notesAbandonnees)
           + ",\"tetes_refusees\":" + String((unsigned long)_refus)
           + ",\"anticipees_refusees\":" + String((unsigned long)_refusAnticipes) + ",\"flux\":[";
  bool premier = true;
  for (uint8_t i = 0; _fl && i < FLUX_MAX; i++) {
    const Flux& x = _fl[i];
    if (x.etat != 1) continue;
    if (!premier) j += ",";
    premier = false;
    const uint32_t hi = x.hi, c = x.cons;
    j += "{\"son\":\"" + String(x.son) + "\",\"voix\":" + String(x.voix)
       + ",\"position\":" + String((unsigned long)c) + ",\"avance_trames\":" + String(hi > c ? (unsigned long)(hi - c) : 0UL)
       + ",\"fin\":" + String((unsigned long)x.fin) + ",\"blocs_manques\":" + String((unsigned long)x.sousAlim)
       + ",\"sauts\":" + String((unsigned long)x.sauts) + ",\"departs\":" + String((unsigned long)x.departs) + "}";
  }
  uint32_t pris = 0, ant = 0;
  for (uint8_t i = 0; _tetes && i < TETES_MAX; i++) if (_tetes[i].etat != 0) { pris += _tetes[i].octets; if (_tetes[i].anticipee) ant += _tetes[i].octets; }
  j += "],\"budget_tetes\":{\"utilise\":" + String((unsigned long)pris) + ",\"max\":" + String((unsigned long)TETES_BUDGET_OCTETS)
     + ",\"anticipees\":" + String((unsigned long)ant) + ",\"max_anticipees\":" + String((unsigned long)ANTICIPE_BUDGET_OCTETS) + "},\"tetes\":[";
  premier = true;
  for (uint8_t i = 0; _tetes && i < TETES_MAX; i++) {
    const Tete& t = _tetes[i];
    if (t.etat == 0) continue;
    if (!premier) j += ",";
    premier = false;
    j += "{\"son\":\"" + String(t.son) + "\",\"debut\":" + String((unsigned long)t.debut)
       + ",\"etat\":\"" + String(t.etat == 1 ? "demandee" : t.etat == 2 ? "prete" : "echec")
       + "\",\"trames\":" + String((unsigned long)t.trames) + ",\"octets\":" + String((unsigned long)t.octets)
       + (t.anticipee ? ",\"anticipee\":true" : "") + "}";
  }
  j += "]}";
  return j;
}

}  // namespace FluxSD
