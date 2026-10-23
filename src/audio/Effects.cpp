/* THE EFFECTS OF THE CARD — see Effects.h. -O3 like every DSP kernel here
 * (sfz16.c, DspBench.cpp): the regime they were measured in (MESURES §209). */
#pragma GCC optimize ("O3")
#include "Effects.h"

#include <esp_heap_caps.h>
#include <new>
#include "AudioEngine.h"
#include "faust/FaustLowpass.h"
#include "faust/FaustAmbience.h"

namespace Effects {
namespace {

constexpr size_t MAX_FRAMES = 128;

/* THE EFFECTS OF ONE CUE SLOT (§212): its low-pass, its reverb, and the blocks the low-pass
 * filters. Written by the cue that arrives in that slot (the slot is free: the audio task does
 * not process it), read by the audio task. */
struct Slot {
  FaustLowpass   lowpass;                   // 144 B: internal RAM, built once
  FaustAmbience* reverb = nullptr;          // 18 KB: PSRAM, the first time a cue asks
  bool           lowpassReady = false;
  FAUSTFLOAT *lpCutoff = nullptr, *lpQ = nullptr;
  FAUSTFLOAT *rvWet = nullptr, *rvDecay = nullptr, *rvDamp = nullptr;
  volatile bool lpOn = false, rvOn = false;
  /* THE BLOCKS THE LOW-PASS FILTERS, in two copies: the writer fills the one the
   * audio task does not read, then switches. */
  uint32_t       blocks[2][LOWPASS_BLOCKS_MAX];
  volatile uint8_t nBlocks[2] = {0, 0};
  volatile uint8_t active = 0;
};
Slot slots[SLOTS];

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

void bind(FaustDsp* d, const char* label, FAUSTFLOAT*& zone) {
  FaustParams p;
  d->buildUserInterface(&p);
  zone = p.find(label);
}

void ensureLowpass(Slot& e) {
  if (e.lowpassReady) return;
  e.lowpass.init((int)AudioEngine::SAMPLE_RATE);
  bind(&e.lowpass, "cutoff", e.lpCutoff);
  bind(&e.lowpass, "q", e.lpQ);
  e.lowpassReady = true;
}

bool ensureReverb(Slot& e, String& reason) {
  if (e.reverb) return true;
  void* raw = heap_caps_malloc(sizeof(FaustAmbience), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!raw) { reason = String("reverb: ") + sizeof(FaustAmbience) + " B of PSRAM not available"; return false; }
  FaustAmbience* r = new (raw) FaustAmbience();
  r->init((int)AudioEngine::SAMPLE_RATE);
  bind(r, "wet", e.rvWet);
  bind(r, "decay", e.rvDecay);
  bind(r, "damp", e.rvDamp);
  e.reverb = r;
  return true;
}

void setBlocks(Slot& e, const String& list) {
  const uint8_t w = e.active ^ 1;
  uint8_t n = 0;
  int at = 0;
  while (at < (int)list.length() && n < LOWPASS_BLOCKS_MAX) {
    int end = list.indexOf(',', at);
    if (end < 0) end = list.length();
    const uint32_t b = (uint32_t)list.substring(at, end).toInt();
    if (b) e.blocks[w][n++] = b;
    at = end + 1;
  }
  e.nBlocks[w] = n;
  e.active = w;
}

inline void put(FAUSTFLOAT* zone, float v, float lo, float hi) {
  if (zone && v == v) *zone = v < lo ? lo : (v > hi ? hi : v);   // v == v: not NaN
}

/* The keys present in `params`, applied. `cue`: a cue speaks for the whole
 * state — an absent effect turns off; live keys change only what they name. */
bool apply(Slot& e, const String& params, bool cue, String& reason) {
  String v;
  const bool wantsLp = (v = valueOf(params, "lowpass")).length() ? v.toFloat() >= 0.5f : (cue ? false : e.lpOn);
  const bool wantsRv = (v = valueOf(params, "reverb")).length()  ? v.toFloat() >= 0.5f : (cue ? false : e.rvOn);
  ensureLowpass(e);
  if ((v = valueOf(params, "lpcutoff")).length()) put(e.lpCutoff, v.toFloat(), 20.0f, 20000.0f);
  if ((v = valueOf(params, "lpq")).length())      put(e.lpQ, v.toFloat(), 0.5f, 20.0f);
  if (cue || valueOf(params, "lpblocs").length()) setBlocks(e, valueOf(params, "lpblocs"));
  if (wantsLp && !e.lpOn) e.lowpass.instanceClear();     // not running: the audio task does not read it
  e.lpOn = wantsLp && shedLevel < 2;
  if (wantsRv) {
    if (!ensureReverb(e, reason)) { e.rvOn = false; return false; }
    if ((v = valueOf(params, "rvwet")).length())   put(e.rvWet, v.toFloat(), 0.0f, 1.0f);
    if ((v = valueOf(params, "rvdecay")).length()) put(e.rvDecay, v.toFloat(), 0.1f, 4.0f);
    if ((v = valueOf(params, "rvdamp")).length())  put(e.rvDamp, v.toFloat(), 0.0f, 0.9f);
    if (!e.rvOn) e.reverb->instanceClear();
  }
  e.rvOn = wantsRv && shedLevel < 1;
  return true;
}

}  // namespace

bool configureFromCue(uint8_t slot, const String& params, String& reason) {
  Slot* e = slotOf(slot);
  if (!e) return false;
  shedLevel = 0;              // a new cue: what the guard dropped may come back
  shedSaid = false;
  return apply(*e, params, true, reason);
}

void off(uint8_t slot) {
  Slot* e = slotOf(slot);
  if (!e) return;
  e->lpOn = false; e->rvOn = false; setBlocks(*e, String());
}

bool applyKeys(uint8_t slot, const String& params, String& reason) {
  Slot* e = slotOf(slot);
  return e && apply(*e, params, false, reason);
}

bool setParam(const char* name, float value, uint8_t mask) {
  for (uint8_t s = 0; s < SLOTS; s++) {
    if (!(mask & (1u << s))) continue;
    Slot& e = slots[s];
    ensureLowpass(e);
    if      (!strcmp(name, "lp_cutoff")) put(e.lpCutoff, value, 20.0f, 20000.0f);
    else if (!strcmp(name, "lp_q"))      put(e.lpQ, value, 0.5f, 20.0f);
    else if (!strcmp(name, "rv_wet"))    put(e.rvWet, value, 0.0f, 1.0f);
    else if (!strcmp(name, "rv_decay"))  put(e.rvDecay, value, 0.1f, 4.0f);
    else if (!strcmp(name, "rv_damp"))   put(e.rvDamp, value, 0.0f, 0.9f);
    else return false;
  }
  return true;
}

bool filters(uint8_t slot, uint32_t bloc) {
  const Slot* e = slotOf(slot);
  if (!e || !e->lpOn || !bloc) return false;
  const uint8_t a = e->active, n = e->nBlocks[a];
  for (uint8_t i = 0; i < n; i++) if (e->blocks[a][i] == bloc) return true;
  return false;
}

void process(uint8_t slot, float* mainL, float* mainR, float* busL, float* busR, size_t frames) {
  Slot* e = slotOf(slot);
  if (!e || frames > MAX_FRAMES) return;
  if (e->lpOn) {
    float* in[2]  = {busL, busR};
    float* out[2] = {tmpL, tmpR};
    e->lowpass.compute((int)frames, in, out);
    for (size_t i = 0; i < frames; i++) { mainL[i] += tmpL[i]; mainR[i] += tmpR[i]; }
  } else {
    for (size_t i = 0; i < frames; i++) { mainL[i] += busL[i]; mainR[i] += busR[i]; }
  }
  if (e->rvOn && e->reverb) {
    float* in[2]  = {mainL, mainR};
    float* out[2] = {tmpL, tmpR};
    e->reverb->compute((int)frames, in, out);
    memcpy(mainL, tmpL, frames * sizeof(float));
    memcpy(mainR, tmpR, frames * sizeof(float));
  }
}

void clear(uint8_t slot) {
  Slot* e = slotOf(slot);
  if (!e) return;
  if (e->lowpassReady) e->lowpass.instanceClear();
  if (e->reverb) e->reverb->instanceClear();
}

void shed() {
  bool any = false;
  for (const Slot& e : slots) any = any || e.rvOn || e.lpOn;
  if (!any) return;                           // nothing of ours to drop: the overload is elsewhere
  if (shedLevel < 2) shedLevel = shedLevel + 1;
  for (Slot& e : slots) {
    if (shedLevel >= 1) e.rvOn = false;
    if (shedLevel >= 2) e.lpOn = false;
  }
  if (!shedSaid) {
    shedSaid = true;
    Serial.printf("[audio] core 1 over budget: %s dropped until the next cue\n",
                  shedLevel >= 2 ? "the reverbs and the low-passes" : "the reverbs");
  }
}

uint32_t psramBytes() {
  uint32_t n = 0;
  for (const Slot& e : slots) if (e.reverb) n += (uint32_t)sizeof(FaustAmbience);
  return n;
}

String status(uint8_t slot) {
  const Slot* e = slotOf(slot);
  if (!e) return "null";
  char s[360];
  const uint8_t a = e->active;
  String list;
  for (uint8_t i = 0; i < e->nBlocks[a]; i++) list += (i ? "," : "") + String((unsigned long)e->blocks[a][i]);
  snprintf(s, sizeof s,
           "{\"lowpass\":{\"on\":%s,\"cutoff\":%.1f,\"q\":%.2f,\"blocs\":[%s]},"
           "\"reverb\":{\"on\":%s,\"wet\":%.3f,\"decay\":%.2f,\"damp\":%.2f,\"bytes\":%u},\"shed\":%u}",
           e->lpOn ? "true" : "false", e->lpCutoff ? *e->lpCutoff : 0.0f, e->lpQ ? *e->lpQ : 0.0f, list.c_str(),
           e->rvOn ? "true" : "false", e->rvWet ? *e->rvWet : 0.0f, e->rvDecay ? *e->rvDecay : 0.0f,
           e->rvDamp ? *e->rvDamp : 0.0f, e->reverb ? (unsigned)sizeof(FaustAmbience) : 0u, (unsigned)shedLevel);
  return String(s);
}

}  // namespace Effects
