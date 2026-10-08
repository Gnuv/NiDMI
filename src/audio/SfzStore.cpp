// SfzStore — see SfzStore.h. The store (PSRAM, deduplicated samples) and the
// engine glue (propose/adopt, the play-list bank pattern); the DSP itself is
// sfz16.c, untouched.
#include "SfzStore.h"
#include "sfz16.h"
#include "SampleStore.h"
#include "../config/SdCard.h"
#include "../server/WebDebugConsole.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace SfzStore {

// ── the pool ────────────────────────────────────────────────────────────────
constexpr int    CHEMIN_MAX = 96;
constexpr int    SONS_MAX   = 64;          // distinct sample files, all instruments
constexpr int    TEXTE_MAX  = 64 * 1024;   // a .sfz text bigger than this is refused
constexpr size_t CHUNK      = 16384;       // SD reads yield between chunks
constexpr uint32_t RETRAIT_MARGIN_MS = 150; // drain margin before PSRAM is freed

struct SonSfz   { char chemin[CHEMIN_MAX]; int16_t* pcm; uint32_t octets;
                  uint32_t trames; uint8_t canaux; uint32_t freq; bool pris; };
struct InstrSfz { char chemin[CHEMIN_MAX]; sfz16_instrument* inst; bool pris;
                  uint32_t libererApres; };  // 0: kept; else the free deadline

static SonSfz   _sons[SONS_MAX];
static InstrSfz _instrs[SIMULTANES];
static size_t   _octets = 0;
static String   _erreur = "";               // the last refusal

// ── the engine: static, INTERNAL RAM, the audio task's alone ───────────────
static sfz16_engine    _eng;                // ~3 ko, voices inside
static float           _rien = 0.0f;
static volatile int32_t _proposition = -1;  // -1 none, -2 retire, else _instrs index
static volatile bool    _armeur = false;    // the cue named sfz=
static volatile bool    _notesArmees = false; // adopted side of _armeur
static volatile int8_t  _actif = -1;        // _instrs index the engine plays
static uint32_t         _gen = 0;
static volatile uint32_t _notesRecues = 0;   // diagnostic: how far the notes travel (§208)
static volatile uint32_t _blocsRendus = 0;    // the audio loop's heartbeat (§208)
static volatile uint32_t _evenementsVus = 0; // events popped from the queue (§208)

// ── helpers (loader side) ───────────────────────────────────────────────────

static int _sonDe(const char* chemin) {
  for (int i = 0; i < SONS_MAX; i++)
    if (_sons[i].pris && !strcmp(_sons[i].chemin, chemin)) return i;
  return -1;
}

static int _instrDe(const char* chemin) {
  for (int j = 0; j < SIMULTANES; j++)
    if (_instrs[j].pris && !strcmp(_instrs[j].chemin, chemin))
      return j;                                  // even one retiring: it is re-adopted below
  return -1;
}

/* A sample no kept instrument refers to anymore: its PSRAM goes back. Called
 * after a failed load (the half-built instrument owns nothing yet) and after
 * an instrument is freed. */
static void _nettoyerSonsOrphelins() {
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
    _octets -= _sons[i].octets;
    _sons[i].pris = false;
    _sons[i].pcm = nullptr;
  }
}

/* Loads one WAV into the pool (or finds it there). The pool keeps the
 * metadata a second instrument reusing the file needs. False with `raison`
 * (SampleStore::wavHeader words it for bad WAVs). */
static bool _chargerSon(const char* chemin, String& raison, SonSfz** sortie) {
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
    f.close(); return false;
  }
  int16_t* pcm = (int16_t*)heap_caps_malloc(octetsData, MALLOC_CAP_SPIRAM);
  if (!pcm) { raison = "PSRAM insuffisante"; f.close(); return false; }
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
  int k = -1;
  for (int i = 0; i < SONS_MAX; i++) if (!_sons[i].pris) { k = i; break; }
  if (k < 0) {                                    // pool full: this kit is too big
    heap_caps_free(pcm);
    raison = "trop de samples distincts (" + String(SONS_MAX) + " au plus)";
    return false;
  }
  strlcpy(_sons[k].chemin, chemin, sizeof(_sons[k].chemin));
  _sons[k].pcm = pcm; _sons[k].octets = octetsData;
  _sons[k].trames = octetsData / (2 * canaux);
  _sons[k].canaux = (uint8_t)canaux;
  _sons[k].freq = freq ? freq : 48000;
  _sons[k].pris = true;
  _octets += octetsData;
  *sortie = &_sons[k];
  return true;
}

/* Loads `chemin` (relative to /sfz) as an instrument slot. The parse fills
 * regions and the sample table (paths relative to the .sfz's folder, already
 * resolved by the kernel with default_path); each sample is read into the
 * pool; link refuses a missing one BY NAME. */
static int _charger(const char* chemin, String& raison) {
  int j = -1;
  for (int i = 0; i < SIMULTANES; i++)
    if (!_instrs[i].pris) { j = i; break; }
  if (j < 0) {
    raison = "trop d'instruments SFZ charges (" + String(SIMULTANES) + " au plus)";
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
  if (!texte) { raison = "PSRAM insuffisante"; f.close(); return -1; }
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
  if (!inst) { heap_caps_free(texte); raison = "PSRAM insuffisante"; return -1; }

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
    if (!_chargerSon(cheminSon.c_str(), raison, &son)) {
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
  _instrs[j].pris = true;
  _instrs[j].libererApres = 0;
  NIDMI_WEB_LOG("[sfz] %s charge : %d regions, %d samples, %u Ko de PCM en PSRAM",
                chemin, inst->region_count, inst->sample_count, (unsigned)(_octets / 1024));
  return j;
}

// ── loader side: propose / housekeeping ─────────────────────────────────────

bool proposer(const char* chemin, String& raison) {
  _armeur = chemin && chemin[0];
  if (!_armeur) { _proposition = -2; return true; }   // silence, nothing to load
  int j = _instrDe(chemin);
  if (j < 0) {
    j = _charger(chemin, raison);
    if (j < 0) { _erreur = raison; return false; }
  } else {
    _instrs[j].libererApres = 0;                 // retiring, then asked again: kept
  }
  _erreur = "";
  __sync_synchronize();          /* the flag crosses cores: published with the proposal */
  _proposition = j;
  return true;
}

void entretenir(const String& employesCsv) {
  const uint32_t maintenant = millis();
  for (int j = 0; j < SIMULTANES; j++) {
    if (!_instrs[j].pris) continue;
    if (_instrs[j].libererApres) {
      /* The drain margin has passed and the engine no longer plays it: free. */
      if (maintenant > _instrs[j].libererApres && _actif != j) {
        NIDMI_WEB_LOG("[sfz] decharge %s", _instrs[j].chemin);
        heap_caps_free(_instrs[j].inst);
        _instrs[j].pris = false;
        _instrs[j].inst = nullptr;
        _nettoyerSonsOrphelins();
      }
      continue;
    }
    /* Still employed? The csv carries every sfz= the cue list names. */
    if (employesCsv.indexOf(String(_instrs[j].chemin)) < 0 && _actif != j)
      _instrs[j].libererApres = maintenant + RETRAIT_MARGIN_MS;
  }
}

// ── audio task ──────────────────────────────────────────────────────────────

void adopter() {
  /* THE FORGOTTEN INIT (§208, found by the voice photograph: inc_int was the
   * 1e9 clamp catching 44100/0 — a static engine starts zeroed, and nothing
   * had ever set its rate). The host tests all called sfz16_engine_init;
   * the card's loader never did. */
  static bool engInit = false;
  if (!engInit) { sfz16_engine_init(&_eng, 48000.0f); engInit = true; }
  const int32_t p = _proposition;
  if (p == -1) return;
  _proposition = -1;
  __sync_synchronize();          /* ...and read the flag only after seeing the proposal */
  if (p == -2) {
    if (_eng.inst) sfz16_set_instrument(&_eng, nullptr);
    _actif = -1;
    if (_notesArmees) _gen++;
    _notesArmees = false;
    return;
  }
  sfz16_set_instrument(&_eng, _instrs[p].inst);   // fast-releases against the old one
  _actif = p;
  _notesArmees = _armeur;
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
  if (!_eng.inst) return false;
  memset(g, 0, trames * sizeof(float));
  memset(d, 0, trames * sizeof(float));
  sfz16_render(&_eng, g, d, trames);
  return true;
}

// ── status ──────────────────────────────────────────────────────────────────

String lister(const char* dossier) {
  File d = SdCard::open(dossier);
  if (!d) return String("{\"erreur\":\"introuvable : ") + dossier + "\"}";
  if (!d.isDirectory()) return String("{\"erreur\":\"pas un dossier : ") + dossier + "\"}";
  String j = "[";
  int n = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (n++) j += ",";
    j += String("{\"nom\":\"") + f.name() + "\",\"octets\":" + String((unsigned)f.size())
       + ",\"dossier\":" + (f.isDirectory() ? "true" : "false") + "}";
    if (n >= 48) break;
  }
  return j + "]";
}

bool    charge()        { return _actif >= 0 || _proposition >= 0; }
String  chemin()        { return (_actif >= 0) ? String(_instrs[_actif].chemin) : String(""); }
int     regions()       { return (_actif >= 0 && _instrs[_actif].inst) ? _instrs[_actif].inst->region_count : 0; }
int     samples()       { return (_actif >= 0 && _instrs[_actif].inst) ? _instrs[_actif].inst->sample_count : 0; }
size_t  octets()        { return _octets; }
int     voixActives()   { return sfz16_active_voices(&_eng); }
uint32_t generation()   { return _gen; }
uint32_t notesRecues()  { return _notesRecues; }
/* DIAGNOSTIC (§208) : le moteur tient-il un instrument, et que vaut sa
 * premiere region ? Dit par la carte, pas devine. */
bool moteurInstValide() {
  if (!_eng.inst || _actif < 0) return false;
  const sfz16_instrument* in = _eng.inst;
  if (in->region_count < 1) return false;
  return true;
}
String regionPremiere() {
  if (_actif < 0 || !_instrs[_actif].inst || _instrs[_actif].inst->region_count < 1)
    return String("{}");
  const sfz16_region* r = &_instrs[_actif].inst->regions[0];
  return String("{\"lo\":") + r->lokey + ",\"hi\":" + r->hikey
       + ",\"lovel\":" + r->lovel + ",\"hivel\":" + r->hivel
       + ",\"sample_index\":" + r->sample_index + ",\"cc\":" + r->cc_count + "}";
}
void blocRendu()       { _blocsRendus++; }
void evenementVu()     { _evenementsVus++; }
uint32_t blocsRendus()    { return _blocsRendus; }
uint32_t evenementsVus()  { return _evenementsVus; }
String  erreur()        { return _erreur; }

}  // namespace SfzStore
