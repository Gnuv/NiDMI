/* THE EFFECTS OF THE CARD — see Effects.h. -O3 like every DSP kernel here
 * (sfz16.c, DspBench.cpp): the regime they were measured in (MESURES §209). */
#pragma GCC optimize ("O3")
#include "Effects.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <new>
#include "AudioEngine.h"
#include "ProcessCosts.h"
#include "faust/FaustLowpass.h"
#include "faust/FaustAmbience.h"

namespace Effects {
namespace {

constexpr size_t  MAX_FRAMES = 128;
enum Kind : uint8_t { NONE = 0, LOWPASS = 1, REVERB = 2 };
constexpr uint8_t NO_GROUP = 0xFF;          // fxmix absent: no effect of the MIX zone

/* The settings of each kind, in the order a cue gives them, with their ranges (those of the
 * Faust sliders) and their defaults. */
struct Setting { const char* name; float def, lo, hi; };
const Setting LOWPASS_SETTINGS[2] = {{"lp_cutoff", 800.0f, 20.0f, 20000.0f}, {"lp_q", 0.707f, 0.5f, 20.0f}};
const Setting REVERB_SETTINGS[3]  = {{"rv_wet", 0.25f, 0.0f, 1.0f}, {"rv_decay", 1.2f, 0.1f, 4.0f},
                                     {"rv_damp", 0.35f, 0.0f, 0.9f}};

/* ONE EFFECT OF A CHAIN: its kind, its block, its group, its settings (low-pass: cutoff, q;
 * reverb: wet, decay, damp), and the unit whose DSP runs it. */
struct Entry {
  uint8_t  kind = NONE, group = 0, unit = 0;
  uint32_t bloc = 0;
  float    p[3] = {0.0f, 0.0f, 0.0f};
};
struct Chain {
  uint8_t n = 0, mixGroup = NO_GROUP, refused = 0;
  Entry   e[PER_SLOT];
};

/* THE DSP OF ONE PLACE: a low-pass and/or a reverb, in PSRAM, allocated by a writer the first
 * time a chain binds that kind to it, never freed. `kind`/`bloc`: what the audio task ran there
 * last — another block starts from silence. */
struct Unit {
  FaustLowpass*  lp = nullptr;
  FAUSTFLOAT    *lpCutoff = nullptr, *lpQ = nullptr;
  FaustAmbience* rv = nullptr;
  FAUSTFLOAT    *rvWet = nullptr, *rvDecay = nullptr, *rvDamp = nullptr;
  uint8_t  kind = NONE;
  uint32_t bloc = 0;
};

struct Slot {
  Chain             posted;          // the writers', under `mux`
  volatile uint32_t postedSeq = 0;
  Chain             running;         // the audio task's, adopted at the start of a block
  uint32_t          runningSeq = 0;
  Unit              unit[PER_SLOT];
};
Slot slots[SLOTS];

portMUX_TYPE      mux = portMUX_INITIALIZER_UNLOCKED;   // a chain is copied whole, under it
StaticSemaphore_t writersBuffer;
SemaphoreHandle_t writers = nullptr;                    // the writers that allocate, one at a time

volatile uint8_t shedLevel = 0;             // 1: reverbs dropped, 2: low-passes dropped too
bool shedSaid = false;

float tmpL[MAX_FRAMES], tmpR[MAX_FRAMES];   // audio-path scratch, internal RAM (the slots in turn)

inline Slot* slotOf(uint8_t s) { return s < SLOTS ? &slots[s] : nullptr; }

String valueOf(const String& params, const char* key) {
  const String k = String(key) + "=";
  int at = 0;
  while (at < (int)params.length()) {
    int end = params.indexOf(';', at);
    if (end < 0) end = params.length();
    String kv = params.substring(at, end);
    kv.trim();
    if (kv.startsWith(k)) return kv.substring(k.length());
    at = end + 1;
  }
  return String();
}

inline float clampTo(float v, const Setting& s) { return v != v ? s.def : (v < s.lo ? s.lo : (v > s.hi ? s.hi : v)); }

/* "low-pass:229:2:1200:0.707" or "ambience:230:3:0.25:1.2:0.35". False: not an effect of ours. */
bool parseEntry(const String& item, Entry& x) {
  String f[6];
  uint8_t n = 0;
  for (int at = 0; n < 6;) {
    const int end = item.indexOf(':', at);
    f[n++] = item.substring(at, end < 0 ? item.length() : end);
    if (end < 0) break;
    at = end + 1;
  }
  const Setting* set = nullptr;
  uint8_t count = 0;
  if (f[0] == "low-pass")      { x.kind = LOWPASS; set = LOWPASS_SETTINGS; count = 2; }
  else if (f[0] == "ambience") { x.kind = REVERB;  set = REVERB_SETTINGS;  count = 3; }
  else return false;
  x.bloc  = (uint32_t)f[1].toInt();
  x.group = (uint8_t)constrain(f[2].toInt(), 0, (long)AudioEngine::GROUP_MAX);
  for (uint8_t i = 0; i < count; i++)
    x.p[i] = (3 + i < n && f[3 + i].length()) ? clampTo(f[3 + i].toFloat(), set[i]) : set[i].def;
  return true;
}

/* The chain a cue (or the route) says: `fx` in signal order, `fxmix` the MIX zone's group. */
void parse(const String& params, Chain& c) {
  c = Chain();
  const String mix = valueOf(params, "fxmix");
  c.mixGroup = mix.length() ? (uint8_t)constrain(mix.toInt(), 0, (long)AudioEngine::GROUP_MAX) : NO_GROUP;
  const String list = valueOf(params, "fx");
  for (int at = 0; at < (int)list.length();) {
    int end = list.indexOf(',', at);
    if (end < 0) end = list.length();
    String item = list.substring(at, end);
    item.trim();
    if (item.length()) {
      Entry x;
      if (c.n < PER_SLOT && parseEntry(item, x)) c.e[c.n++] = x;
      else c.refused++;
    }
    at = end + 1;
  }
}

void bind(FaustDsp* d, const char* label, FAUSTFLOAT*& zone) {
  FaustParams p;
  d->buildUserInterface(&p);
  zone = p.find(label);
}

template <class T> T* allocate() {
  void* raw = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!raw) return nullptr;
  T* d = new (raw) T();
  d->init((int)AudioEngine::SAMPLE_RATE);
  return d;
}

/* The DSP of `kind` in that unit, allocated if it has none yet (a writer: never the audio task). */
bool ensure(Unit& u, uint8_t kind, String& reason) {
  if (kind == LOWPASS && !u.lp) {
    FaustLowpass* d = allocate<FaustLowpass>();
    if (!d) { reason = String("low-pass: ") + sizeof(FaustLowpass) + " B of PSRAM not available"; return false; }
    bind(d, "cutoff", u.lpCutoff);
    bind(d, "q", u.lpQ);
    u.lp = d;
  } else if (kind == REVERB && !u.rv) {
    FaustAmbience* d = allocate<FaustAmbience>();
    if (!d) { reason = String("reverb: ") + sizeof(FaustAmbience) + " B of PSRAM not available"; return false; }
    bind(d, "wet", u.rvWet);
    bind(d, "decay", u.rvDecay);
    bind(d, "damp", u.rvDamp);
    u.rv = d;
  }
  return true;
}

inline bool hasDsp(const Unit& u, uint8_t kind) { return kind == LOWPASS ? u.lp != nullptr : u.rv != nullptr; }

void lockWriters() {
  if (!writers) {
    portENTER_CRITICAL(&mux);
    if (!writers) writers = xSemaphoreCreateMutexStatic(&writersBuffer);
    portEXIT_CRITICAL(&mux);
  }
  xSemaphoreTake(writers, portMAX_DELAY);
}
void unlockWriters() { xSemaphoreGive(writers); }

/* THE CHAIN POSTED: each effect bound to a unit — the one that ran its block, so that it keeps
 * its state (a reverb's tail), else a free one — its DSP allocated, then the chain published
 * whole. An effect whose PSRAM is refused is left out, and said. */
bool post(Slot& s, Chain& c, String& reason) {
  lockWriters();
  Chain prev;
  portENTER_CRITICAL(&mux);
  prev = s.posted;
  portEXIT_CRITICAL(&mux);
  bool used[PER_SLOT] = {}, bound[PER_SLOT] = {};
  for (uint8_t j = 0; j < c.n; j++)
    for (uint8_t k = 0; k < prev.n; k++) {
      const Entry& was = prev.e[k];
      if (was.kind == c.e[j].kind && was.bloc == c.e[j].bloc && !used[was.unit]) {
        c.e[j].unit = was.unit;
        used[was.unit] = bound[j] = true;
        break;
      }
    }
  for (uint8_t j = 0; j < c.n; j++) {
    if (bound[j]) continue;
    int8_t pick = -1;
    for (uint8_t u = 0; u < PER_SLOT && pick < 0; u++) if (!used[u] && hasDsp(s.unit[u], c.e[j].kind)) pick = u;
    for (uint8_t u = 0; u < PER_SLOT && pick < 0; u++) if (!used[u]) pick = u;
    c.e[j].unit = (uint8_t)pick;            // PER_SLOT units for at most PER_SLOT effects: one is free
    used[pick] = true;
  }
  bool ok = true;
  uint8_t kept = 0;
  for (uint8_t j = 0; j < c.n; j++) {
    if (!ensure(s.unit[c.e[j].unit], c.e[j].kind, reason)) { ok = false; c.refused++; continue; }
    c.e[kept++] = c.e[j];
  }
  c.n = kept;
  portENTER_CRITICAL(&mux);
  s.posted = c;
  s.postedSeq = s.postedSeq + 1;
  portEXIT_CRITICAL(&mux);
  unlockWriters();
  return ok;
}

bool setChain(uint8_t slot, const String& params, String& reason) {
  Slot* s = slotOf(slot);
  if (!s) return false;
  Chain c;
  parse(params, c);
  const uint8_t said = c.refused;
  const bool ok = post(*s, c, reason);
  if (said) {
    if (reason.length()) reason += "; ";
    reason += String(said) + " effect(s) refused: a cue holds " + PER_SLOT + ", of the kinds low-pass and ambience";
  }
  return ok && !said;
}

inline bool runs(uint8_t kind) { return kind == LOWPASS ? shedLevel < 2 : (kind == REVERB && shedLevel < 1); }

/* One effect on a sum, in place — and what it cost (§231). */
void run(Unit& u, uint8_t kind, float* l, float* r, size_t frames) {
  float* in[2]  = {l, r};
  float* out[2] = {tmpL, tmpR};
  const uint32_t t = ESP.getCycleCount();
  if (kind == LOWPASS && u.lp)     u.lp->compute((int)frames, in, out);
  else if (kind == REVERB && u.rv) u.rv->compute((int)frames, in, out);
  else return;
  memcpy(l, tmpL, frames * sizeof(float));
  memcpy(r, tmpR, frames * sizeof(float));
  ProcessCosts::note(kind == LOWPASS ? ProcessCosts::LOW_PASS : ProcessCosts::AMBIENCE,
                     ESP.getCycleCount() - t, (uint32_t)frames);
}

}  // namespace

bool configureFromCue(uint8_t slot, const String& params, String& reason) {
  shedLevel = 0;              // a new cue: what the guard dropped may come back
  shedSaid = false;
  return setChain(slot, params, reason);
}

bool applyKeys(uint8_t slot, const String& params, String& reason) { return setChain(slot, params, reason); }

void off(uint8_t slot) {
  Slot* s = slotOf(slot);
  if (!s) return;
  portENTER_CRITICAL(&mux);
  s->posted = Chain();
  s->postedSeq = s->postedSeq + 1;
  portEXIT_CRITICAL(&mux);
}

bool setParam(const char* name, float value, uint8_t mask, uint8_t group) {
  if (!name) return false;
  uint8_t kind = NONE, i = 0;
  const Setting* set = nullptr;
  for (uint8_t k = 0; k < 2; k++) if (!strcmp(name, LOWPASS_SETTINGS[k].name)) { kind = LOWPASS; i = k; set = &LOWPASS_SETTINGS[k]; }
  for (uint8_t k = 0; k < 3; k++) if (!strcmp(name, REVERB_SETTINGS[k].name))  { kind = REVERB;  i = k; set = &REVERB_SETTINGS[k]; }
  if (!set) return false;
  const float v = clampTo(value, *set);
  portENTER_CRITICAL(&mux);
  for (uint8_t s = 0; s < SLOTS; s++) {
    if (!(mask & (1u << s))) continue;
    Chain& c = slots[s].posted;
    bool any = false;
    for (uint8_t j = 0; j < c.n; j++)
      if (c.e[j].kind == kind && (group == 0 || c.e[j].group == group)) { c.e[j].p[i] = v; any = true; }
    if (any) slots[s].postedSeq = slots[s].postedSeq + 1;
  }
  portEXIT_CRITICAL(&mux);
  return true;
}

bool isParam(const char* name) {
  if (!name) return false;
  for (const Setting& s : LOWPASS_SETTINGS) if (!strcmp(name, s.name)) return true;
  for (const Setting& s : REVERB_SETTINGS)  if (!strcmp(name, s.name)) return true;
  return false;
}

bool inOctaves(const char* name) { return name && !strcmp(name, "lp_cutoff"); }

void beginBlock(uint8_t slot) {
  Slot* s = slotOf(slot);
  if (!s || s->postedSeq == s->runningSeq) return;
  portENTER_CRITICAL(&mux);
  s->running = s->posted;
  s->runningSeq = s->postedSeq;
  portEXIT_CRITICAL(&mux);
  bool held[PER_SLOT] = {};
  for (uint8_t j = 0; j < s->running.n; j++) {
    const Entry& x = s->running.e[j];
    Unit& u = s->unit[x.unit];
    held[x.unit] = true;
    if (u.kind != x.kind || u.bloc != x.bloc) {          // another block here: from silence
      if (x.kind == LOWPASS && u.lp) u.lp->instanceClear();
      if (x.kind == REVERB && u.rv)  u.rv->instanceClear();
      u.kind = x.kind;
      u.bloc = x.bloc;
    }
    if (x.kind == LOWPASS && u.lp) {
      if (u.lpCutoff) *u.lpCutoff = x.p[0];
      if (u.lpQ)      *u.lpQ = x.p[1];
    } else if (x.kind == REVERB && u.rv) {
      if (u.rvWet)   *u.rvWet = x.p[0];
      if (u.rvDecay) *u.rvDecay = x.p[1];
      if (u.rvDamp)  *u.rvDamp = x.p[2];
    }
  }
  for (uint8_t u = 0; u < PER_SLOT; u++)
    if (!held[u]) { s->unit[u].kind = NONE; s->unit[u].bloc = 0; }   // a block that comes back starts from silence
}

uint32_t groupMask(uint8_t slot) {
  const Slot* s = slotOf(slot);
  if (!s) return 0;
  uint32_t m = 0;
  for (uint8_t j = 0; j < s->running.n; j++) {
    const Entry& x = s->running.e[j];
    if (x.group != s->running.mixGroup && x.group <= AudioEngine::GROUP_MAX && runs(x.kind)) m |= 1u << x.group;
  }
  return m;
}

void processGroup(uint8_t slot, uint8_t group, float* l, float* r, size_t frames) {
  Slot* s = slotOf(slot);
  if (!s || frames > MAX_FRAMES || group == s->running.mixGroup) return;
  for (uint8_t j = 0; j < s->running.n; j++) {
    const Entry& x = s->running.e[j];
    if (x.group == group && runs(x.kind)) run(s->unit[x.unit], x.kind, l, r, frames);
  }
}

void processMix(uint8_t slot, float* l, float* r, size_t frames) {
  Slot* s = slotOf(slot);
  if (!s || frames > MAX_FRAMES || s->running.mixGroup == NO_GROUP) return;
  for (uint8_t j = 0; j < s->running.n; j++) {
    const Entry& x = s->running.e[j];
    if (x.group == s->running.mixGroup && runs(x.kind)) run(s->unit[x.unit], x.kind, l, r, frames);
  }
}

void clear(uint8_t slot) {
  Slot* s = slotOf(slot);
  if (!s) return;
  for (Unit& u : s->unit) {
    if (u.lp) u.lp->instanceClear();
    if (u.rv) u.rv->instanceClear();
    u.kind = NONE;
    u.bloc = 0;
  }
}

void shed() {
  bool any = false;
  for (const Slot& s : slots)
    for (uint8_t j = 0; j < s.running.n; j++) any = any || runs(s.running.e[j].kind);
  if (!any) return;                           // nothing of ours to drop: the overload is elsewhere
  if (shedLevel < 2) shedLevel = shedLevel + 1;
  if (!shedSaid) {
    shedSaid = true;
    Serial.printf("[audio] core 1 over budget: %s dropped until the next cue\n",
                  shedLevel >= 2 ? "the reverbs and the low-passes" : "the reverbs");
  }
}

uint32_t psramBytes() {
  uint32_t n = 0;
  for (const Slot& s : slots)
    for (const Unit& u : s.unit) {
      if (u.lp) n += (uint32_t)sizeof(FaustLowpass);
      if (u.rv) n += (uint32_t)sizeof(FaustAmbience);
    }
  return n;
}

String status(uint8_t slot) {
  const Slot* s = slotOf(slot);
  if (!s) return "null";
  Chain c;
  portENTER_CRITICAL(&mux);
  c = s->posted;
  portEXIT_CRITICAL(&mux);
  String j = "{\"chain\":[";
  char b[160];
  for (uint8_t i = 0; i < c.n; i++) {
    const Entry& x = c.e[i];
    if (x.kind == LOWPASS)
      snprintf(b, sizeof b, "%s{\"kind\":\"low-pass\",\"bloc\":%lu,\"group\":%u,\"cutoff\":%.1f,\"q\":%.3f}",
               i ? "," : "", (unsigned long)x.bloc, (unsigned)x.group, x.p[0], x.p[1]);
    else
      snprintf(b, sizeof b, "%s{\"kind\":\"ambience\",\"bloc\":%lu,\"group\":%u,\"wet\":%.3f,\"decay\":%.2f,\"damp\":%.2f}",
               i ? "," : "", (unsigned long)x.bloc, (unsigned)x.group, x.p[0], x.p[1], x.p[2]);
    j += b;
  }
  uint32_t bytes = 0;
  for (const Unit& u : s->unit) bytes += (u.lp ? sizeof(FaustLowpass) : 0) + (u.rv ? sizeof(FaustAmbience) : 0);
  snprintf(b, sizeof b, "],\"mix\":%s,\"max\":%u,\"refused\":%u,\"shed\":%u,\"bytes\":%lu}",
           c.mixGroup == NO_GROUP ? "null" : String(c.mixGroup).c_str(), (unsigned)PER_SLOT,
           (unsigned)c.refused, (unsigned)shedLevel, (unsigned long)bytes);
  j += b;
  return j;
}

}  // namespace Effects
