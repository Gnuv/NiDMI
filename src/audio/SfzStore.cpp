// SfzStore — see SfzStore.h. The store (PSRAM, deduplicated samples) and the
// engine glue (deposit / serve / adopt); the DSP itself is sfz16.c.
//
// WHO TOUCHES WHAT (the one-task-per-part rule of the project):
//   - the POOL (_sons, _instrs, _octets, _nbPris) belongs to the sd-card task:
//     servir() is its only writer. Other tasks read single words of it for
//     the status, never a structure they would follow for long;
//   - the WISH (_veut, _veutTicket, _servi, _erreur) is written by whoever
//     deposits (any task) and by the sd-card task when it concludes, always
//     under _mux (a spinlock held for a few instructions, never around I/O);
//   - the HANDOFF to the audio task is _proposition (-1 none, -2 silence, else
//     an _instrs index), written under _mux and taken with an atomic exchange;
//   - the ENGINE (_eng) belongs to the audio task.
#include "SfzStore.h"
#include "sfz16.h"
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

// ── the wish, and the handoff to the audio task ─────────────────────────────
static portMUX_TYPE     _mux = portMUX_INITIALIZER_UNLOCKED;
static char             _veut[CHEMIN_MAX] = "";   // what the cue asked ("" = silence)
static uint32_t         _veutTicket = 0;     // bumps on every deposit
static uint32_t         _servi = 0;          // the last ticket the task concluded
static bool             _veutOk = false;     // ... and it succeeded
static char             _erreur[160] = "";   // the last refusal
static char* volatile   _employes = nullptr; // the cue list's sfz= paths, malloc'd, taken by the task
static volatile int32_t _proposition = -1;   // -1 none, -2 silence, else an _instrs index
static char*            _csvMemo = nullptr;  // the last employed list (sd-card task's own copy)

// ── the engine: static, INTERNAL RAM, the audio task's alone ───────────────
static sfz16_engine    _eng;                // ~3 ko, voices inside
static volatile bool    _notesArmees = false; // the current cue routes notes here
static volatile int8_t  _actif = -1;        // _instrs index the engine plays
static uint32_t         _gen = 0;
static volatile uint32_t _notesRecues = 0;   // diagnostic: how far the notes travel (§208)
static volatile uint32_t _blocsRendus = 0;    // the audio loop's heartbeat (§208)
static volatile uint32_t _evenementsVus = 0; // events popped from the queue (§208)

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
static void _marquer(const char* csv) {
  char voeu[CHEMIN_MAX];
  portENTER_CRITICAL(&_mux); strlcpy(voeu, _veut, sizeof voeu); portEXIT_CRITICAL(&_mux);
  const int32_t prop = _proposition;
  for (int j = 0; j < SIMULTANES; j++) {
    if (!_instrs[j].pris || _instrs[j].libererApres) continue;
    if (_nomme(csv, _instrs[j].chemin) || !strcmp(voeu, _instrs[j].chemin)) continue;
    if (_actif == j || prop == j) continue;
    _instrs[j].libererApres = _echeance();
  }
}

/* Frees the retiring instruments whose drain margin has passed. True if any. */
static bool _liberer() {
  bool libere = false;
  for (int j = 0; j < SIMULTANES; j++) {
    if (!_instrs[j].pris || !_instrs[j].libererApres || !_echu(_instrs[j].libererApres)) continue;
    if (_actif == j || _proposition == j) { _instrs[j].libererApres = 0; continue; }
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

bool proposer(const char* chemin, String& raison) {
  if (!chemin) chemin = "";
  if (!chemin[0]) {                              // silence: nothing to read, no task needed
    portENTER_CRITICAL(&_mux);
    _veut[0] = 0; _veutTicket++; _servi = _veutTicket; _veutOk = true; _erreur[0] = 0;
    _proposition = -2;
    portEXIT_CRITICAL(&_mux);
    return true;
  }
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
  dejaSert = (_servi == _veutTicket && _veutOk && !strcmp(_veut, chemin));
  if (!dejaSert) { strlcpy(_veut, chemin, sizeof _veut); _veutTicket++; }
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

/* Takes the wish if one is waiting. */
static bool _prendreVoeu(char* sortie, uint32_t& ticket) {
  bool attend;
  portENTER_CRITICAL(&_mux);
  attend = (_servi != _veutTicket);
  if (attend) { strlcpy(sortie, _veut, CHEMIN_MAX); ticket = _veutTicket; }
  portEXIT_CRITICAL(&_mux);
  return attend;
}

/* Concludes ticket `ticket`: an instrument (j >= 0) goes to the audio task, or
 * a refusal is kept. Only if the wish did not change meanwhile — a newer one
 * is served next. `definitif` false: the wish stays pending (no card yet). */
static bool _conclure(uint32_t ticket, int j, const String& raison, bool definitif) {
  bool courant;
  portENTER_CRITICAL(&_mux);
  courant = (ticket == _veutTicket);
  if (courant) {
    if (j >= 0) { _proposition = j; _erreur[0] = 0; _servi = ticket; _veutOk = true; }
    else {
      strlcpy(_erreur, raison.c_str(), sizeof _erreur);
      if (definitif) { _servi = ticket; _veutOk = false; }
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

  char voeu[CHEMIN_MAX]; uint32_t ticket;
  while (_prendreVoeu(voeu, ticket)) {
    if (!SdCard::mounted()) {                    // wait for the card: SdCard calls us again when it is back
      _conclure(ticket, -1, "carte SD absente ou perdue : le chargement reprendra a son retour", false);
      break;
    }
    if (_instrDe(voeu) < 0) _annoncer("charge", voeu);
    String raison; Manque manque = RIEN;
    const uint32_t t0 = millis();
    const int j = _obtenir(voeu, raison, manque);
    if (j >= 0) _dernierMs = millis() - t0;
    const bool courant = _conclure(ticket, j, raison, true);
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
  for (int t = 0; t < 20 && _proposition != -1; t++) vTaskDelay(pdMS_TO_TICKS(5));
  vTaskDelay(pdMS_TO_TICKS(5));                  // the audio task sets _actif right after taking it
  if (_csvMemo) _marquer(_csvMemo);
  _attendreLesRetraits();                        // frees what the housekeeping marked (a few ms)
}

// ── audio task ──────────────────────────────────────────────────────────────

void adopter() {
  /* THE FORGOTTEN INIT (§208, found by the voice photograph: inc_int was the
   * 1e9 clamp catching 44100/0 — a static engine starts zeroed, and nothing
   * had ever set its rate). The host tests all called sfz16_engine_init;
   * the card's loader never did. */
  static bool engInit = false;
  if (!engInit) { sfz16_engine_init(&_eng, 48000.0f); engInit = true; }
  /* Taken AND cleared in one step: a proposition deposited between a read and
   * a clear would be lost. */
  const int32_t p = __atomic_exchange_n(&_proposition, (int32_t)-1, __ATOMIC_ACQ_REL);
  if (p == -1) return;
  if (p == -2) {
    if (_eng.inst) sfz16_set_instrument(&_eng, nullptr);
    _actif = -1;
    if (_notesArmees) _gen++;
    _notesArmees = false;
    return;
  }
  if (p == _actif && _eng.inst == _instrs[p].inst) {   // the same kit again: its voices are not touched
    _notesArmees = true;
    return;
  }
  sfz16_set_instrument(&_eng, _instrs[p].inst);   // fast-releases against the old one
  _actif = (int8_t)p;
  _notesArmees = true;
  _gen++;
}

bool notesArmees() { return _notesArmees; }

void noteOn(uint8_t note, uint8_t velo) {
  if (!velo) return;
  _notesRecues++;
  sfz16_note_on(&_eng, note, velo);
}
void noteOff(uint8_t note)             { sfz16_note_off(&_eng, note); }

bool rendreBloc(float* g, float* d, uint32_t trames) {
  /* A NULL instrument still has voices to drain (sfz16_needs_render): skipping
   * the render left them active for good, holding pointers into an instrument
   * freed 150 ms later — reproduced on the card: a ringing crash, the
   * instrument unloaded, "voix: 1" forever. */
  if (!sfz16_needs_render(&_eng)) return false;
  memset(g, 0, trames * sizeof(float));
  memset(d, 0, trames * sizeof(float));
  sfz16_render(&_eng, g, d, trames);
  return true;
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
           + ",\"dernier_ms\":" + String((unsigned)_dernierMs) + ",\"instruments\":[";
  bool premier = true;
  for (int i = 0; i < SIMULTANES; i++) {
    if (!_instrs[i].pris) continue;
    if (!premier) j += ",";
    premier = false;
    j += String("{\"chemin\":\"") + echapper(_instrs[i].chemin) + "\",\"actif\":"
       + (_actif == i ? "true" : "false") + ",\"retire\":" + (_instrs[i].libererApres ? "true" : "false") + "}";
  }
  return j + "]}";
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
static const InstrSfz* _jouant() {
  const int a = _actif;
  return (a >= 0 && _instrs[a].pris && _instrs[a].inst) ? &_instrs[a] : nullptr;
}

bool    charge()        { return _actif >= 0 || _proposition >= 0; }
bool    chargement()    { bool b; portENTER_CRITICAL(&_mux); b = (_servi != _veutTicket); portEXIT_CRITICAL(&_mux); return b; }
int     enMemoire()     { return _nbPris; }
String  demande()       { char s[CHEMIN_MAX]; portENTER_CRITICAL(&_mux); strlcpy(s, _veut, sizeof s); portEXIT_CRITICAL(&_mux); return String(s); }
String  chemin()        { const InstrSfz* i = _jouant(); return i ? String(i->chemin) : String(""); }
int     regions()       { const InstrSfz* i = _jouant(); return i ? i->inst->region_count : 0; }
int     samples()       { const InstrSfz* i = _jouant(); return i ? i->inst->sample_count : 0; }
size_t  octets()        { return _octets; }
int     voixActives()   { return sfz16_active_voices(&_eng); }
uint32_t generation()   { return _gen; }
uint32_t notesRecues()  { return _notesRecues; }
/* DIAGNOSTIC (§208) : le moteur tient-il un instrument, et que vaut sa
 * premiere region ? Dit par la carte, pas devine. */
bool moteurInstValide() {
  const sfz16_instrument* in = _eng.inst;
  return in && _actif >= 0 && in->region_count >= 1;
}
String regionPremiere() {
  const InstrSfz* i = _jouant();
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
