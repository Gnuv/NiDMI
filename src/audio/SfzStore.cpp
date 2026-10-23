// SfzStore — see SfzStore.h. The store (PSRAM, deduplicated samples) and the
// engine glue (deposit / serve / adopt); the DSP itself is sfz16.c.
//
// WHO TOUCHES WHAT (the one-task-per-part rule of the project):
//   - the POOL (_sons, _instrs, _octets, _nbPris) belongs to the sd-card task:
//     servir() is its only writer. Other tasks read single words of it for
//     the status, never a structure they would follow for long;
//   - the WISH of each engine (_voeux, _erreur) is written by whoever deposits
//     (any task) and by the sd-card task when it concludes, always under _mux (a
//     spinlock held for a few instructions, never around I/O);
//   - the HANDOFF to the audio task is _proposition[engine] (-1 none, -2 silence,
//     else an _instrs index), written under _mux and taken with an atomic exchange;
//   - the ENGINES (_engs, two since §212) belong to the audio task.
#include "SfzStore.h"
#include "sfz16.h"
#include <strings.h>                    // strcasecmp
#include "SampleStore.h"
#include "../config/SdCard.h"
#include "../server/ServerCore.h"       // nidmi_ws_pousser: the board ANNOUNCES what it loads
#include "../server/WebDebugConsole.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace SfzStore {

// ── the pool ────────────────────────────────────────────────────────────────
constexpr int    SONS_MAX   = 64;          // distinct sample files, all instruments
constexpr int    TEXTE_MAX  = 64 * 1024;   // a .sfz text bigger than this is refused
constexpr size_t CHUNK      = 16384;       // SD reads yield between chunks
constexpr uint32_t RETRAIT_MARGIN_MS = 150; // drain margin before PSRAM is freed

// "/sfz/" + the .sfz's folder + "/" + the kernel's longest sample path (160): never cut.
struct SonSfz   { char chemin[5 + CHEMIN_MAX + 1 + 160 + 1]; int16_t* pcm; uint32_t octets;
                  uint32_t trames; uint8_t canaux; uint32_t freq; bool pris; };
struct InstrSfz { char chemin[CHEMIN_MAX]; sfz16_instrument* inst; bool pris;
                  uint32_t libererApres; };  // 0: kept; else the free deadline (millis)

static SonSfz*  _sons = nullptr;            // SONS_MAX entries, in PSRAM: ~19 ko that must not be .bss
static InstrSfz _instrs[SIMULTANES];
static volatile uint32_t _octets = 0;
static volatile int      _nbPris = 0;       // instruments held (retiring included)
static volatile uint32_t _dernierMs = 0;    // the last load, from the wish taken to the instrument ready

// ── the wishes, and the handoff to the audio task — one per engine (§212) ────
static portMUX_TYPE     _mux = portMUX_INITIALIZER_UNLOCKED;
struct Voeu {
  char     veut[CHEMIN_MAX];                 // what the cue asked ("" = silence)
  uint32_t ticket;                           // bumps on every deposit
  uint32_t servi;                            // the last ticket the task concluded
  bool     ok;                               // ... and it succeeded
};
static Voeu             _voeux[MOTEURS] = {};
static char             _erreur[160] = "";   // the last refusal
static char* volatile   _employes = nullptr; // the cue list's sfz= paths, malloc'd, taken by the task
static volatile int32_t _proposition[MOTEURS] = {-1, -1};   // -1 none, -2 silence, else an _instrs index
static char*            _csvMemo = nullptr;  // the last employed list (sd-card task's own copy)

// ── the engines, the audio task's alone ─────────────────────────────────────
static sfz16_engine     _eng0;               // ~3 ko, voices inside: INTERNAL RAM
static sfz16_engine* volatile _engs[MOTEURS] = {&_eng0, nullptr};   // [1] in PSRAM, the first time a cue asks
static volatile bool    _notesArmees[MOTEURS] = {false, false};    // a cue routes notes to it
static volatile int8_t  _actif[MOTEURS] = {-1, -1};                // _instrs index the engine plays
static uint32_t         _gen = 0;
static volatile uint32_t _notesRecues = 0;   // diagnostic: how far the notes travel (§208)
static volatile uint32_t _blocsRendus = 0;    // the audio loop's heartbeat (§208)
static volatile uint32_t _evenementsVus = 0; // events popped from the queue (§208)
static volatile uint32_t _blocId[MOTEURS] = {0, 0};      // the app's block that asked for the kit (0: none)
static volatile float    _gain[MOTEURS] = {1.0f, 1.0f};  // the target gain, any task writes it
static float             _gainLisse[MOTEURS] = {1.0f, 1.0f};   // the audio task's smoothed copy
static volatile uint8_t  _ccEntrant[MOTEURS][128];   // the Control Change tables, written by the MIDI side (§212: per engine)
static volatile uint32_t _ccRecus = 0;       // diagnostic: CC that reached the table

// ── what a note and a block COST, measured on the audio task itself (§208) ───
// noteOn and rendreBloc both run there, one after the other, at the head of every block. A burst of notes pays
// sfz16_note_on in series, then its new voices read their samples cold: both are timed in CPU cycles, and the
// blocks are sorted by how many notes started in the last three of them. The block's budget is FRAMES x 5000
// cycles (240 MHz / 48 kHz); the firmware counts a block late only past 20 ms, after the notes were applied.
struct CostClass { uint32_t n, kcyc, max, tranche[5]; };  // blocks; sum of the kernel's cycles / 1024; its worst; by 1/2, 1, 2, 4 budgets
struct CostSlow   { uint32_t total, kernel, notes; uint8_t fresh, recent, voices; };
static CostClass _costClass[4];                  // notes started in the last 3 blocks: 0, 1-4, 5-15, 16 or more
static uint32_t   _costOnN = 0, _costOnKcyc = 0, _costOnMax = 0;   // sfz16_note_on calls: count, sum / 1024, worst
static uint32_t   _costBurstMax = 0, _costBurstNotes = 0;               // the worst note-on total paid in front of ONE block, and its notes
static uint32_t   _costBlockOn = 0;             // cycles spent in note-ons since the last render
static uint8_t    _costBlockNotes = 0;          // notes since the last render
static uint8_t    _costRecents[2] = {0, 0};    // notes of the two blocks before
static uint32_t   _costKernel = 0;              // the kernel's cycles in the block just rendered
static uint32_t   _costNotesCyc = 0;            // ... and the note-on cycles paid in front of it
static uint8_t    _costFresh = 0, _costRecent = 0;
static uint32_t   _costTotalN = 0, _costTotalMax = 0, _costTotalOver = 0;  // whole renders: count, worst, past one budget
static CostSlow   _costSlow[8];               // the last blocks that took more than a budget, whole render
static uint32_t   _costNSlow = 0;

// ── helpers (sd-card task) ──────────────────────────────────────────────────

enum Manque { RIEN = 0, PLACE, BUDGET };    // why a load failed, when MORE ROOM could cure it

static int _sonDe(const char* chemin) {
  if (!_sons) return -1;
  for (int i = 0; i < SONS_MAX; i++)
    if (_sons[i].pris && !strcmp(_sons[i].chemin, chemin)) return i;
  return -1;
}

static int _instrDe(const char* chemin) {
  for (int j = 0; j < SIMULTANES; j++)
    if (_instrs[j].pris && !strcmp(_instrs[j].chemin, chemin))
      return j;                                  // even one retiring: it is re-adopted
  return -1;
}

static void _compterPris() {
  int n = 0;
  for (int j = 0; j < SIMULTANES; j++) if (_instrs[j].pris) n++;
  _nbPris = n;
}

/* A sample no kept instrument refers to anymore: its PSRAM goes back. Called
 * after a failed load (the half-built instrument owns nothing yet) and after
 * an instrument is freed. */
static void _nettoyerSonsOrphelins() {
  if (!_sons) return;
  for (int i = 0; i < SONS_MAX; i++) {
    if (!_sons[i].pris) continue;
    bool employe = false;
    for (int j = 0; j < SIMULTANES && !employe; j++)
      if (_instrs[j].pris) {
        const sfz16_instrument* in = _instrs[j].inst;
        for (int s = 0; s < in->sample_count; s++)
          if (in->samples[s].data == _sons[i].pcm) { employe = true; break; }
      }
    if (employe) continue;
    heap_caps_free(_sons[i].pcm);
    _octets = _octets - _sons[i].octets;
    _sons[i].pris = false;
    _sons[i].pcm = nullptr;
  }
}

/* Loads one WAV into the pool (or finds it there). The pool keeps the
 * metadata a second instrument reusing the file needs. False with `raison`
 * (SampleStore::wavHeader words it for bad WAVs). */
static bool _chargerSon(const char* chemin, String& raison, Manque& manque, SonSfz** sortie) {
  const int existant = _sonDe(chemin);
  if (existant >= 0) { *sortie = &_sons[existant]; return true; }

  File f = SdCard::open(chemin);
  if (!f) { raison = "sample introuvable sur la SD : " + String(chemin); return false; }
  uint16_t canaux; uint32_t freq, octetsData;
  if (!SampleStore::wavHeader(f, canaux, freq, octetsData, raison)) { f.close(); return false; }
  if (_octets + octetsData > BUDGET_PSRAM) {
    raison = "budget PSRAM depasse (" + String(BUDGET_PSRAM / 1024) + " Ko) : il reste "
             + String((BUDGET_PSRAM - _octets) / 1024) + " Ko, " + chemin
             + " en voudrait " + String(octetsData / 1024);
    manque = BUDGET;
    f.close(); return false;
  }
  int k = -1;
  for (int i = 0; i < SONS_MAX; i++) if (!_sons[i].pris) { k = i; break; }
  if (k < 0) {                                    // pool full: this kit is too big
    raison = "trop de samples distincts (" + String(SONS_MAX) + " au plus)";
    manque = PLACE;
    f.close(); return false;
  }
  int16_t* pcm = (int16_t*)heap_caps_malloc(octetsData, MALLOC_CAP_SPIRAM);
  if (!pcm) { raison = "PSRAM insuffisante"; manque = BUDGET; f.close(); return false; }
  size_t lus = 0;
  while (lus < octetsData) {                      // chunked, like SampleStore::_lire
    const size_t pas = (octetsData - lus < CHUNK) ? octetsData - lus : CHUNK;
    const size_t n = f.read((uint8_t*)pcm + lus, pas);
    if (!n) break;
    lus += n;
    vTaskDelay(1);                                // the reading task must not starve IDLE
  }
  f.close();
  if (lus != octetsData) {
    heap_caps_free(pcm);
    raison = "lecture incomplete : " + String(chemin);
    return false;
  }
  strlcpy(_sons[k].chemin, chemin, sizeof(_sons[k].chemin));
  _sons[k].pcm = pcm; _sons[k].octets = octetsData;
  _sons[k].trames = octetsData / (2 * canaux);
  _sons[k].canaux = (uint8_t)canaux;
  _sons[k].freq = freq ? freq : 48000;
  _sons[k].pris = true;
  _octets = _octets + octetsData;
  *sortie = &_sons[k];
  return true;
}

/* Loads `chemin` (relative to /sfz) as an instrument slot. The parse fills
 * regions and the sample table (paths relative to the .sfz's folder, already
 * resolved by the kernel with default_path); each sample is read into the
 * pool; link refuses a missing one BY NAME. */
static int _charger(const char* chemin, String& raison, Manque& manque) {
  if (!_sons) {
    _sons = (SonSfz*)heap_caps_calloc(SONS_MAX, sizeof(SonSfz), MALLOC_CAP_SPIRAM);
    if (!_sons) { raison = "PSRAM insuffisante"; manque = BUDGET; return -1; }
  }
  int j = -1;
  for (int i = 0; i < SIMULTANES; i++)
    if (!_instrs[i].pris) { j = i; break; }
  if (j < 0) {
    raison = "trop d'instruments SFZ charges (" + String(SIMULTANES) + " au plus)";
    manque = PLACE;
    return -1;
  }

  const String fichier = String(DOSSIER) + "/" + chemin;
  const String dossier = fichier.substring(0, fichier.lastIndexOf('/'));
  File f = SdCard::open(fichier.c_str());
  if (!f) { raison = "introuvable sur la SD : " + fichier; return -1; }
  const size_t taille = f.size();
  if (taille == 0 || taille > TEXTE_MAX) {
    raison = "fichier .sfz de " + String(taille) + " o : vide ou trop gros ("
             + String(TEXTE_MAX / 1024) + " Ko au plus)";
    f.close(); return -1;
  }
  char* texte = (char*)heap_caps_malloc(taille + 1, MALLOC_CAP_SPIRAM);
  if (!texte) { raison = "PSRAM insuffisante"; manque = BUDGET; f.close(); return -1; }
  size_t lus = 0;
  while (lus < taille) {
    const size_t n = f.read((uint8_t*)texte + lus, taille - lus);
    if (!n) break;
    lus += n;
  }
  f.close();
  texte[lus < taille ? lus : taille] = 0;

  sfz16_instrument* inst = (sfz16_instrument*)heap_caps_calloc(1, sizeof(sfz16_instrument),
                                                               MALLOC_CAP_SPIRAM);
  if (!inst) { heap_caps_free(texte); raison = "PSRAM insuffisante"; manque = BUDGET; return -1; }

  sfz16_report rep; char err[160] = "";
  if (sfz16_parse(inst, texte, strlen(texte), &rep, err, sizeof(err)) != 0) {
    heap_caps_free(inst); heap_caps_free(texte);
    raison = err[0] ? String(err) : String("parse impossible");
    return -1;
  }
  heap_caps_free(texte);
  if (rep.unknown_opcodes)
    NIDMI_WEB_LOG("[sfz] %s : %d opcode(s) inconnu(s) — le premier : %s",
                  chemin, rep.unknown_opcodes, rep.first_unknown);

  for (int i = 0; i < inst->sample_count; i++) {
    SonSfz* son = nullptr;
    const String cheminSon = dossier + "/" + inst->samples[i].path;
    if (!_chargerSon(cheminSon.c_str(), raison, manque, &son)) {
      heap_caps_free(inst);
      _nettoyerSonsOrphelins();                   // a failed load owns nothing
      return -1;                                  // the refusal names the file
    }
    inst->samples[i].data = son->pcm;
    inst->samples[i].frames = son->trames;
    inst->samples[i].channels = son->canaux;
    inst->samples[i].sample_rate = (float)son->freq;
  }
  if (sfz16_link(inst, err, sizeof(err)) != 0) {
    heap_caps_free(inst);
    _nettoyerSonsOrphelins();
    raison = err;
    return -1;
  }

  strlcpy(_instrs[j].chemin, chemin, sizeof(_instrs[j].chemin));
  _instrs[j].inst = inst;
  _instrs[j].libererApres = 0;
  _instrs[j].pris = true;
  _compterPris();
  NIDMI_WEB_LOG("[sfz] %s charge : %d regions, %d samples, %u Ko de PCM en PSRAM (pile libre mini %u o)",
                chemin, inst->region_count, inst->sample_count, (unsigned)(_octets / 1024),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  return j;
}

// ── housekeeping: which instruments does the cue list still name? ───────────

static bool _nomme(const char* csv, const char* chemin) {
  const size_t n = strlen(chemin);
  for (const char* p = csv; *p;) {
    const char* fin = strchr(p, ',');
    const size_t len = fin ? (size_t)(fin - p) : strlen(p);
    if (len == n && !strncmp(p, chemin, n)) return true;
    if (!fin) break;
    p = fin + 1;
  }
  return false;
}

/* The deadline is never 0 (0 means "kept"). Time is compared as a signed
 * difference: millis() wraps every 49 days. */
static uint32_t _echeance() { return (millis() + RETRAIT_MARGIN_MS) | 1; }
static bool     _echu(uint32_t t) { return (int32_t)(millis() - t) > 0; }

/* Marks the instruments NO cue names (and that are neither playing, nor asked
 * for, nor about to be adopted): they are freed RETRAIT_MARGIN_MS later. */
/* An instrument an engine plays, or is about to adopt (§212: either engine). */
static bool _tenu(int j) {
  for (uint8_t m = 0; m < MOTEURS; m++) if (_actif[m] == j || _proposition[m] == j) return true;
  return false;
}

static void _marquer(const char* csv) {
  char voeux[MOTEURS][CHEMIN_MAX];
  portENTER_CRITICAL(&_mux);
  for (uint8_t m = 0; m < MOTEURS; m++) strlcpy(voeux[m], _voeux[m].veut, CHEMIN_MAX);
  portEXIT_CRITICAL(&_mux);
  for (int j = 0; j < SIMULTANES; j++) {
    if (!_instrs[j].pris || _instrs[j].libererApres) continue;
    if (_nomme(csv, _instrs[j].chemin)) continue;
    bool voulu = false;
    for (uint8_t m = 0; m < MOTEURS; m++) voulu = voulu || !strcmp(voeux[m], _instrs[j].chemin);
    if (voulu || _tenu(j)) continue;
    _instrs[j].libererApres = _echeance();
  }
}

/* Frees the retiring instruments whose drain margin has passed. True if any. */
static bool _liberer() {
  bool libere = false;
  for (int j = 0; j < SIMULTANES; j++) {
    if (!_instrs[j].pris || !_instrs[j].libererApres || !_echu(_instrs[j].libererApres)) continue;
    if (_tenu(j)) { _instrs[j].libererApres = 0; continue; }
    NIDMI_WEB_LOG("[sfz] decharge %s", _instrs[j].chemin);
    heap_caps_free(_instrs[j].inst);
    _instrs[j].inst = nullptr;
    _instrs[j].pris = false;
    libere = true;
  }
  if (libere) { _compterPris(); _nettoyerSonsOrphelins(); }
  return libere;
}

static bool _ilEnReste() {
  for (int j = 0; j < SIMULTANES; j++) if (_instrs[j].pris && _instrs[j].libererApres) return true;
  return false;
}

/* Waits (a few ms: the margin is 150) for the retiring instruments to be
 * freeable, then frees them. True if any room came back. */
static bool _attendreLesRetraits() {
  bool libere = false;
  for (int tour = 0; tour < 40; tour++) {
    if (_liberer()) libere = true;
    if (!_ilEnReste()) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return libere;
}

/* The instrument for `voeu`: already held (even retiring — it is kept again),
 * or loaded now. When the load failed for lack of room and some instrument is
 * on its way out, wait for it and try once more. */
static int _obtenir(const char* voeu, String& raison, Manque& manque) {
  const int deja = _instrDe(voeu);
  if (deja >= 0) { _instrs[deja].libererApres = 0; return deja; }
  int j = _charger(voeu, raison, manque);
  if (j < 0 && manque != RIEN && _attendreLesRetraits()) {
    raison = ""; manque = RIEN;
    j = _charger(voeu, raison, manque);
  }
  return j;
}

// ── the wish (any task) ─────────────────────────────────────────────────────

static void _poserErreur(const String& s) {
  portENTER_CRITICAL(&_mux);
  strlcpy(_erreur, s.c_str(), sizeof _erreur);
  portEXIT_CRITICAL(&_mux);
}

static void _annoncer(const char* etat, const char* chemin) {
  char trame[CHEMIN_MAX + 24];
  snprintf(trame, sizeof trame, "NIDMI_SFZ:%s:%s", etat, chemin);
  nidmi_ws_pousser(trame);
}

/* Engine 1 in PSRAM, the first time a cue asks for it (§212): ~3 ko the internal RAM
 * does not carry for a second cue it may never play. Inited here, published last. */
static bool _assurerMoteur(uint8_t m, String& raison) {
  if (m >= MOTEURS) { raison = "moteur SFZ inconnu"; return false; }
  if (_engs[m]) return true;
  sfz16_engine* e = (sfz16_engine*)heap_caps_calloc(1, sizeof(sfz16_engine), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!e) { raison = String("moteur SFZ ") + m + " : " + sizeof(sfz16_engine) + " o de PSRAM indisponibles"; return false; }
  sfz16_engine_init(e, 48000.0f);
  __sync_synchronize();
  _engs[m] = e;
  return true;
}

bool proposer(uint8_t m, const char* chemin, String& raison) {
  if (!chemin) chemin = "";
  if (m >= MOTEURS) { raison = "moteur SFZ inconnu"; return false; }
  if (!chemin[0]) {                              // silence: nothing to read, no task needed
    portENTER_CRITICAL(&_mux);
    Voeu& v = _voeux[m];
    v.veut[0] = 0; v.ticket++; v.servi = v.ticket; v.ok = true; _erreur[0] = 0;
    _proposition[m] = -2;
    portEXIT_CRITICAL(&_mux);
    return true;
  }
  if (!_assurerMoteur(m, raison)) { _poserErreur(raison); return false; }
  if (strlen(chemin) >= (size_t)CHEMIN_MAX) {
    raison = "chemin SFZ trop long (" + String(CHEMIN_MAX - 1) + " caracteres au plus)";
    _poserErreur(raison);
    return false;
  }
  if (strstr(chemin, "..")) {
    raison = "chemin SFZ non permis (\"..\") : il se lit sous " + String(DOSSIER);
    _poserErreur(raison);
    return false;
  }
  bool dejaSert = false;
  portENTER_CRITICAL(&_mux);
  /* The same instrument asked again after it was served (consecutive cues of
   * one kit): nothing to deposit, and the engine is left alone. A refused one
   * IS asked again — the card may have come back. */
  Voeu& v = _voeux[m];
  dejaSert = (v.servi == v.ticket && v.ok && !strcmp(v.veut, chemin));
  if (!dejaSert) { strlcpy(v.veut, chemin, sizeof v.veut); v.ticket++; }
  portEXIT_CRITICAL(&_mux);
  if (!dejaSert) SdCard::requestSfz();
  return true;
}

void entretenir(const String& employesCsv) {
  char* copie = (char*)heap_caps_malloc(employesCsv.length() + 1, MALLOC_CAP_SPIRAM);
  if (!copie) return;                            // missed: the next cue does it again
  memcpy(copie, employesCsv.c_str(), employesCsv.length() + 1);
  char* ancienne = __atomic_exchange_n(&_employes, copie, __ATOMIC_ACQ_REL);
  if (ancienne) heap_caps_free(ancienne);        // a newer list replaces one not taken yet
  SdCard::requestSfz();
}

// ── the sd-card task ────────────────────────────────────────────────────────

/* Takes a wish if one is waiting — the first engine that has one. */
static bool _prendreVoeu(uint8_t& m, char* sortie, uint32_t& ticket) {
  bool attend = false;
  portENTER_CRITICAL(&_mux);
  for (uint8_t k = 0; k < MOTEURS && !attend; k++) {
    const Voeu& v = _voeux[k];
    if (v.servi == v.ticket) continue;
    attend = true; m = k;
    strlcpy(sortie, v.veut, CHEMIN_MAX); ticket = v.ticket;
  }
  portEXIT_CRITICAL(&_mux);
  return attend;
}

/* Concludes ticket `ticket`: an instrument (j >= 0) goes to the audio task, or
 * a refusal is kept. Only if the wish did not change meanwhile — a newer one
 * is served next. `definitif` false: the wish stays pending (no card yet). */
static bool _conclure(uint8_t m, uint32_t ticket, int j, const String& raison, bool definitif) {
  bool courant;
  portENTER_CRITICAL(&_mux);
  Voeu& v = _voeux[m];
  courant = (ticket == v.ticket);
  if (courant) {
    if (j >= 0) { _proposition[m] = j; _erreur[0] = 0; v.servi = ticket; v.ok = true; }
    else {
      strlcpy(_erreur, raison.c_str(), sizeof _erreur);
      if (definitif) { v.servi = ticket; v.ok = false; }
    }
  }
  portEXIT_CRITICAL(&_mux);
  return courant;
}

void servir() {
  char* emp = __atomic_exchange_n(&_employes, (char*)nullptr, __ATOMIC_ACQ_REL);
  if (emp) {
    if (_csvMemo) heap_caps_free(_csvMemo);
    _csvMemo = emp;                              // kept: the wish below retires with it too
    _marquer(_csvMemo);
  }

  char voeu[CHEMIN_MAX]; uint32_t ticket; uint8_t m = 0;
  while (_prendreVoeu(m, voeu, ticket)) {
    if (!SdCard::mounted()) {                    // wait for the card: SdCard calls us again when it is back
      _conclure(m, ticket, -1, "carte SD absente ou perdue : le chargement reprendra a son retour", false);
      break;
    }
    if (_instrDe(voeu) < 0) _annoncer("charge", voeu);
    String raison; Manque manque = RIEN;
    const uint32_t t0 = millis();
    const int j = _obtenir(voeu, raison, manque);
    if (j >= 0) _dernierMs = millis() - t0;
    const bool courant = _conclure(m, ticket, j, raison, true);
    if (j >= 0) {
      NIDMI_WEB_LOG("[sfz] %s pret en %lu ms%s", voeu, (unsigned long)(millis() - t0),
                    courant ? "" : " (remplace entre-temps : garde en reserve)");
      if (courant) _annoncer("pret", voeu);
    } else {
      NIDMI_WEB_LOG("[sfz] %s refuse : %s", voeu, raison.c_str());
      if (courant) _annoncer("refus", voeu);
    }
  }
  /* The housekeeping skips the PLAYING instrument, and the audio task adopts a
   * proposition (a new kit, or silence) within a block (2.5 ms): once it did,
   * the kit it replaced is retirable. Wait for the adoption, then sort again
   * with the last cue list — otherwise it would stay until the next cue. */
  for (int t = 0; t < 20 && (_proposition[0] != -1 || _proposition[1] != -1); t++) vTaskDelay(pdMS_TO_TICKS(5));
  vTaskDelay(pdMS_TO_TICKS(5));                  // the audio task sets _actif right after taking it
  if (_csvMemo) _marquer(_csvMemo);
  _attendreLesRetraits();                        // frees what the housekeeping marked (a few ms)
}

// ── audio task ──────────────────────────────────────────────────────────────

static void _adopter(uint8_t m) {
  sfz16_engine* eng = _engs[m];
  if (!eng) return;
  /* THE FORGOTTEN INIT (§208, found by the voice photograph: inc_int was the
   * 1e9 clamp catching 44100/0 — a static engine starts zeroed, and nothing
   * had ever set its rate). The host tests all called sfz16_engine_init;
   * the card's loader never did. (Engine 1 is inited where it is allocated.) */
  static bool eng0Init = false;
  if (m == 0 && !eng0Init) { sfz16_engine_init(eng, 48000.0f); eng0Init = true; }
  /* Taken AND cleared in one step: a proposition deposited between a read and
   * a clear would be lost. */
  const int32_t p = __atomic_exchange_n(&_proposition[m], (int32_t)-1, __ATOMIC_ACQ_REL);
  if (p == -1) return;
  if (p == -2) {
    if (eng->inst) sfz16_set_instrument(eng, nullptr);
    _actif[m] = -1;
    if (_notesArmees[m]) _gen++;
    _notesArmees[m] = false;
    return;
  }
  if (p == _actif[m] && eng->inst == _instrs[p].inst) {   // the same kit again: its voices are not touched
    _notesArmees[m] = true;
    return;
  }
  sfz16_set_instrument(eng, _instrs[p].inst);   // fast-releases against the old one
  _actif[m] = (int8_t)p;
  _notesArmees[m] = true;
  _gen++;
}

void adopter() { for (uint8_t m = 0; m < MOTEURS; m++) _adopter(m); }

bool notesArmees(uint8_t m) { return m < MOTEURS && _notesArmees[m]; }

static inline float _borne(float gain) { return (gain < 0.0f) ? 0.0f : (gain > 4.0f ? 4.0f : gain); }

void fixerBloc(uint8_t m, uint32_t bloc, float gain) {
  if (m >= MOTEURS) return;
  _blocId[m] = bloc;
  _gain[m] = _borne(gain);
}

uint8_t fixerGainBloc(uint32_t bloc, float gain) {
  if (!bloc) return 0;
  uint8_t n = 0;
  for (uint8_t m = 0; m < MOTEURS; m++) if (_blocId[m] == bloc) { _gain[m] = _borne(gain); n++; }
  return n ? 1 : 0;
}

uint32_t blocId(uint8_t m)    { return m < MOTEURS ? _blocId[m] : 0; }
float    gainCible(uint8_t m) { return m < MOTEURS ? _gain[m] : 0.0f; }

float gainLisse(uint8_t m) {
  if (m >= MOTEURS) return 0.0f;
  _gainLisse[m] += (_gain[m] - _gainLisse[m]) * 0.25f;     // ~10 ms to get there at 2.5 ms a block
  if (fabsf(_gain[m] - _gainLisse[m]) < 0.0005f) _gainLisse[m] = _gain[m];
  return _gainLisse[m];
}

void controle(uint8_t m, uint8_t cc, uint8_t valeur) {
  if (m >= MOTEURS) return;
  _ccEntrant[m][cc & 127] = valeur;
  _ccRecus++;
}

void noteOn(uint8_t m, uint8_t note, uint8_t velo) {
  sfz16_engine* eng = (m < MOTEURS) ? _engs[m] : nullptr;
  if (!velo || !eng) return;
  _notesRecues++;
  /* The CC state the regions' locc/hicc gates read: copied now, the one moment
   * they do (a CC that came before this note, in the order the MIDI arrived —
   * it was stored before the note was queued). */
  for (int i = 0; i < 128; i++) eng->cc[i] = _ccEntrant[m][i];
  const uint32_t c0 = ESP.getCycleCount();
  sfz16_note_on(eng, note, velo);
  const uint32_t dc = ESP.getCycleCount() - c0;
  _costOnN++; _costOnKcyc += dc >> 10; if (dc > _costOnMax) _costOnMax = dc;
  _costBlockOn += dc; if (_costBlockNotes < 255) _costBlockNotes++;
}
void noteOff(uint8_t m, uint8_t note) { sfz16_engine* e = (m < MOTEURS) ? _engs[m] : nullptr; if (e) sfz16_note_off(e, note); }
/* Its cue slot is emptied, its output already silent (§212): the voices stop NOW — a release
 * tail left in the engine would ring in the next cue that takes it. */
void toutEteindre(uint8_t m) {
  sfz16_engine* e = (m < MOTEURS) ? _engs[m] : nullptr;
  if (!e) return;
  for (int i = 0; i < SFZ16_MAX_VOICES; i++) e->voices[i].active = 0;
}

bool rendreBloc(uint8_t m, float* g, float* d, uint32_t trames) {
  sfz16_engine* eng = (m < MOTEURS) ? _engs[m] : nullptr;
  if (!eng) return false;
  /* A NULL instrument still has voices to drain (sfz16_needs_render): skipping
   * the render left them active for good, holding pointers into an instrument
   * freed 150 ms later — reproduced on the card: a ringing crash, the
   * instrument unloaded, "voix: 1" forever. */
  const uint8_t fresh = _costBlockNotes;
  const uint32_t recents = (uint32_t)fresh + _costRecents[0] + _costRecents[1];   // this block and the two before
  _costRecents[1] = _costRecents[0]; _costRecents[0] = fresh;
  if (_costBlockOn > _costBurstMax) { _costBurstMax = _costBlockOn; _costBurstNotes = fresh; }
  _costNotesCyc = _costBlockOn;
  _costBlockOn = 0; _costBlockNotes = 0;
  if (!sfz16_needs_render(eng)) { _costKernel = 0; return false; }
  const uint32_t c0 = ESP.getCycleCount();
  memset(g, 0, trames * sizeof(float));
  memset(d, 0, trames * sizeof(float));
  sfz16_render(eng, g, d, trames);
  const uint32_t dc = ESP.getCycleCount() - c0;
  const uint32_t budget = trames * 5000u;
  CostClass& c = _costClass[recents == 0 ? 0 : recents < 5 ? 1 : recents < 16 ? 2 : 3];
  c.n++; c.kcyc += dc >> 10; if (dc > c.max) c.max = dc;
  c.tranche[dc < budget / 2 ? 0 : dc < budget ? 1 : dc < 2 * budget ? 2 : dc < 4 * budget ? 3 : 4]++;
  _costKernel = dc; _costRecent = (uint8_t)(recents > 255 ? 255 : recents); _costFresh = fresh;
  return true;
}

void costBlock(uint32_t cyclesTotal) {
  _costTotalN++;
  if (cyclesTotal > _costTotalMax) _costTotalMax = cyclesTotal;
  if (cyclesTotal > 120u * 5000u) {                 // past one budget (FRAMES = 120): remember who it was
    _costTotalOver++;
    CostSlow& l = _costSlow[_costNSlow++ & 7];
    l.total = cyclesTotal; l.kernel = _costKernel; l.notes = _costNotesCyc;
    l.fresh = _costFresh; l.recent = _costRecent; l.voices = (uint8_t)(voixActives(0) + voixActives(1));
  }
}

void resetCost() {
  memset(_costClass, 0, sizeof _costClass); memset(_costSlow, 0, sizeof _costSlow);
  _costOnN = _costOnKcyc = _costOnMax = _costBurstMax = _costBurstNotes = 0;
  _costTotalN = _costTotalMax = _costTotalOver = _costNSlow = 0;
}

String cost() {
  String j = String("{\"budget\":") + String((unsigned)(120u * 5000u)) + ",\"note_on\":{\"n\":" + String((unsigned)_costOnN)
           + ",\"avg\":" + String((unsigned)(_costOnN ? (uint64_t)_costOnKcyc * 1024u / _costOnN : 0))
           + ",\"max\":" + String((unsigned)_costOnMax) + "},\"burst\":{\"max\":" + String((unsigned)_costBurstMax)
           + ",\"notes\":" + String((unsigned)_costBurstNotes) + "},\"classes\":[";
  for (int k = 0; k < 4; k++) {
    const CostClass& c = _costClass[k];
    j += String(k ? "," : "") + "{\"n\":" + String((unsigned)c.n) + ",\"avg\":" + String((unsigned)(c.n ? (uint64_t)c.kcyc * 1024u / c.n : 0))
       + ",\"max\":" + String((unsigned)c.max) + ",\"buckets\":[" + String((unsigned)c.tranche[0]);
    for (int t = 1; t < 5; t++) j += "," + String((unsigned)c.tranche[t]);
    j += "]}";
  }
  j += String("],\"total\":{\"n\":") + String((unsigned)_costTotalN) + ",\"max\":" + String((unsigned)_costTotalMax)
     + ",\"over_budget\":" + String((unsigned)_costTotalOver) + "},\"slow\":[";
  const uint32_t nl = _costNSlow < 8 ? _costNSlow : 8;
  for (uint32_t i = 0; i < nl; i++) {
    const CostSlow& l = _costSlow[(_costNSlow - nl + i) & 7];
    j += String(i ? "," : "") + "{\"total\":" + String((unsigned)l.total) + ",\"kernel\":" + String((unsigned)l.kernel)
       + ",\"notes\":" + String((unsigned)l.notes) + ",\"fresh\":" + String((unsigned)l.fresh)
       + ",\"recent\":" + String((unsigned)l.recent) + ",\"voices\":" + String((unsigned)l.voices) + "}";
  }
  return j + "]}";
}

// ── status ──────────────────────────────────────────────────────────────────

String lister(const char* dossier) {
  File d = SdCard::openForWeb(dossier);          // the web never walks a card in doubt
  if (!d) return String("{\"erreur\":\"introuvable : ") + echapper(dossier) + "\"}";
  if (!d.isDirectory()) return String("{\"erreur\":\"pas un dossier : ") + echapper(dossier) + "\"}";
  String j = "[";
  int n = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (n++) j += ",";
    j += String("{\"nom\":\"") + echapper(f.name()) + "\",\"octets\":" + String((unsigned)f.size())
       + ",\"dossier\":" + (f.isDirectory() ? "true" : "false") + "}";
    if (n >= 48) break;
  }
  return j + "]";
}

/* What the pool holds, for the bench and for the day the app asks "what does
 * this cue cost": single words only (the sd-card task may be mid-load). */
String diagnostic() {
  String j = String("{\"dossier\":\"") + DOSSIER + "\",\"budget\":" + String((unsigned)BUDGET_PSRAM)
           + ",\"octets\":" + String((unsigned)_octets) + ",\"en_memoire\":" + String(_nbPris)
           + ",\"dernier_ms\":" + String((unsigned)_dernierMs) + ",\"cc_recus\":" + String((unsigned)_ccRecus)
           + ",\"instruments\":[";
  bool premier = true;
  for (int i = 0; i < SIMULTANES; i++) {
    if (!_instrs[i].pris) continue;
    if (!premier) j += ",";
    premier = false;
    j += String("{\"chemin\":\"") + echapper(_instrs[i].chemin) + "\",\"actif\":"
       + (_tenu(i) ? "true" : "false") + ",\"retire\":" + (_instrs[i].libererApres ? "true" : "false") + "}";
  }
  /* The Control Changes held (the ones that are not 0): what a locc/hicc gate sees. */
  j += "],\"cc\":{";
  bool premierCc = true;
  for (int i = 0; i < 128; i++) {                // engine 0's; engine 1 under "cc1"
    if (!_ccEntrant[0][i]) continue;
    if (!premierCc) j += ",";
    premierCc = false;
    j += String("\"") + i + "\":" + (unsigned)_ccEntrant[0][i];
  }
  j += "},\"cc1\":{";
  premierCc = true;
  for (int i = 0; i < 128; i++) {
    if (!_ccEntrant[1][i]) continue;
    if (!premierCc) j += ",";
    premierCc = false;
    j += String("\"") + i + "\":" + (unsigned)_ccEntrant[1][i];
  }
  return j + "}}";
}

static bool _finit(const char* nom, const char* ext) {
  const size_t n = strlen(nom), e = strlen(ext);
  return n >= e && !strcasecmp(nom + n - e, ext);
}

/* THE KITS OF /sfz, read as DIRECTORY ENTRIES (SdCard::listForWeb, §216): nothing is opened. Opening
 * every file to weigh it cost ~7 ms each, the "._" twins a Mac leaves beside every file included, and
 * the list ran out of its 900 ms before the last kit. A kit is a folder holding .sfz files; its WAV
 * files may sit one folder below (samples/), their weight is their size minus a 44-byte header (a
 * majorant of the PSRAM they take). Names starting with '.' are skipped: ._kit.sfz, .DS_Store,
 * .Spotlight-V100. */
namespace {
struct KitWalk {
  String   sfz[8];
  uint32_t sizes[8] = {0};
  int      ns = 0, wavCount = 0, seen = 0, depth = 0;
  uint32_t wavBytes = 0, t0 = 0;
  bool     truncated = false;
  String   folder;
};
bool visitKit(const char* name, bool dir, uint32_t size, void* ctx) {
  KitWalk& k = *(KitWalk*)ctx;
  if (++k.seen > 800 || millis() - k.t0 > 900) { k.truncated = true; return false; }
  if (name[0] == '.') return true;
  if (dir) {
    if (k.depth < 1) {
      const String back = k.folder;
      k.folder = back + "/" + name;
      k.depth++;
      SdCard::listForWeb(k.folder.c_str(), visitKit, ctx);
      k.depth--;
      k.folder = back;
    }
    return !k.truncated;
  }
  if (_finit(name, ".wav")) { k.wavBytes += (size > 44) ? size - 44 : 0; k.wavCount++; }
  else if (k.depth == 0 && _finit(name, ".sfz") && k.ns < 8) { k.sfz[k.ns] = name; k.sizes[k.ns] = size; k.ns++; }
  return true;
}
struct RootWalk {
  String*  json;
  int      kits = 0, seen = 0;
  uint32_t t0 = 0;
  bool     truncated = false;
};
bool visitRoot(const char* name, bool dir, uint32_t size, void* ctx) {
  RootWalk& r = *(RootWalk*)ctx;
  if (name[0] == '.') return true;             // hidden entries are not kits (nor folders of kits)
  if (++r.seen > 64 || r.kits >= 64 || millis() - r.t0 > 900) { r.truncated = true; return false; }
  String& j = *r.json;
  if (!dir) {                                  // a .sfz at the root of /sfz: no folder, no WAV to weigh
    if (_finit(name, ".sfz")) {
      if (r.kits++) j += ",";
      j += String("{\"path\":\"") + echapper(name) + "\",\"bytes\":" + String((unsigned)size)
         + ",\"wav_bytes\":0,\"wav_count\":0}";
    }
    return true;
  }
  KitWalk k;
  k.t0 = r.t0;
  k.folder = String(DOSSIER) + "/" + name;
  SdCard::listForWeb(k.folder.c_str(), visitKit, &k);
  for (int i = 0; i < k.ns && r.kits < 64; i++) {
    if (r.kits++) j += ",";
    j += String("{\"path\":\"") + echapper(String(name) + "/" + k.sfz[i]) + "\",\"bytes\":"
       + String((unsigned)k.sizes[i]) + ",\"wav_bytes\":" + String((unsigned)k.wavBytes)
       + ",\"wav_count\":" + String(k.wavCount) + "}";
  }
  if (k.truncated) { r.truncated = true; return false; }
  return true;
}
}  // namespace

String listerKits() {
  String j = String("{\"sd\":\"") + SdCard::state() + "\",\"folder\":\"" + DOSSIER
           + "\",\"budget\":" + String((unsigned)BUDGET_PSRAM) + ",\"kits\":[";
  RootWalk r;
  r.json = &j;
  r.t0 = millis();
  SdCard::listForWeb(DOSSIER, visitRoot, &r);
  return j + "],\"truncated\":" + (r.truncated ? "true" : "false") + "}";
}

String echapper(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

/* The status reads single words of the pool. A slot being read is the playing
 * one: it is freed only after it stopped being (and RETRAIT_MARGIN_MS more),
 * which is long next to a status call. */
static const InstrSfz* _jouant(uint8_t m) {
  const int a = (m < MOTEURS) ? _actif[m] : -1;
  return (a >= 0 && _instrs[a].pris && _instrs[a].inst) ? &_instrs[a] : nullptr;
}

bool    charge(uint8_t m)  { return m < MOTEURS && (_actif[m] >= 0 || _proposition[m] >= 0); }
bool    actif() {
  for (uint8_t m = 0; m < MOTEURS; m++) if (_engs[m] && sfz16_needs_render(_engs[m])) return true;
  return false;
}
bool    chargement() {
  bool b = false;
  portENTER_CRITICAL(&_mux);
  for (uint8_t m = 0; m < MOTEURS; m++) b = b || (_voeux[m].servi != _voeux[m].ticket);
  portEXIT_CRITICAL(&_mux);
  return b;
}
int     enMemoire()        { return _nbPris; }
String  demande(uint8_t m) {
  if (m >= MOTEURS) return String();
  char s[CHEMIN_MAX]; portENTER_CRITICAL(&_mux); strlcpy(s, _voeux[m].veut, sizeof s); portEXIT_CRITICAL(&_mux);
  return String(s);
}
String  chemin(uint8_t m)  { const InstrSfz* i = _jouant(m); return i ? String(i->chemin) : String(""); }
int     regions(uint8_t m) { const InstrSfz* i = _jouant(m); return i ? i->inst->region_count : 0; }
int     samples(uint8_t m) { const InstrSfz* i = _jouant(m); return i ? i->inst->sample_count : 0; }
size_t  octets()           { return _octets; }
int     voixActives(uint8_t m) { return (m < MOTEURS && _engs[m]) ? sfz16_active_voices(_engs[m]) : 0; }
uint32_t generation()   { return _gen; }
uint32_t notesRecues()  { return _notesRecues; }
/* DIAGNOSTIC (§208) : le moteur tient-il un instrument, et que vaut sa
 * premiere region ? Dit par la carte, pas devine. */
bool moteurInstValide(uint8_t m) {
  const sfz16_instrument* in = (m < MOTEURS && _engs[m]) ? _engs[m]->inst : nullptr;
  return in && _actif[m] >= 0 && in->region_count >= 1;
}
String regionPremiere(uint8_t m) {
  const InstrSfz* i = _jouant(m);
  if (!i || i->inst->region_count < 1) return String("{}");
  const sfz16_region* r = &i->inst->regions[0];
  return String("{\"lo\":") + r->lokey + ",\"hi\":" + r->hikey
       + ",\"lovel\":" + r->lovel + ",\"hivel\":" + r->hivel
       + ",\"sample_index\":" + r->sample_index + ",\"cc\":" + r->cc_count + "}";
}
void blocRendu()       { _blocsRendus++; }
void evenementVu()     { _evenementsVus++; }
uint32_t blocsRendus()    { return _blocsRendus; }
uint32_t evenementsVus()  { return _evenementsVus; }
String  erreur()        { char s[sizeof _erreur]; portENTER_CRITICAL(&_mux); strlcpy(s, _erreur, sizeof s); portEXIT_CRITICAL(&_mux); return String(s); }

}  // namespace SfzStore
