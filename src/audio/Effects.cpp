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

FaustLowpass   lowpass;                     // 144 B: internal RAM, built once
FaustAmbience* reverb = nullptr;            // 18 KB: PSRAM, the first time a cue asks
bool           lowpassReady = false;
FAUSTFLOAT *lpCutoff = nullptr, *lpQ = nullptr;
FAUSTFLOAT *rvWet = nullptr, *rvDecay = nullptr, *rvDamp = nullptr;

volatile bool lpOn = false, rvOn = false;
volatile uint8_t shedLevel = 0;             // 1: reverb dropped, 2: low-pass dropped too
bool shedSaid = false;

/* THE BLOCKS THE LOW-PASS FILTERS, in two copies: the writer fills the one the
 * audio task does not read, then switches. */
uint32_t       blocks[2][LOWPASS_BLOCKS_MAX];
volatile uint8_t nBlocks[2] = {0, 0};
volatile uint8_t active = 0;

float tmpL[MAX_FRAMES], tmpR[MAX_FRAMES];   // audio-path scratch, internal RAM

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

void ensureLowpass() {
  if (lowpassReady) return;
  lowpass.init((int)AudioEngine::SAMPLE_RATE);
  bind(&lowpass, "cutoff", lpCutoff);
  bind(&lowpass, "q", lpQ);
  lowpassReady = true;
}

bool ensureReverb(String& reason) {
  if (reverb) return true;
  void* raw = heap_caps_malloc(sizeof(FaustAmbience), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!raw) { reason = String("reverb: ") + sizeof(FaustAmbience) + " B of PSRAM not available"; return false; }
  FaustAmbience* r = new (raw) FaustAmbience();
  r->init((int)AudioEngine::SAMPLE_RATE);
  bind(r, "wet", rvWet);
  bind(r, "decay", rvDecay);
  bind(r, "damp", rvDamp);
  reverb = r;
  return true;
}

void setBlocks(const String& list) {
  const uint8_t w = active ^ 1;
  uint8_t n = 0;
  int at = 0;
  while (at < (int)list.length() && n < LOWPASS_BLOCKS_MAX) {
    int end = list.indexOf(',', at);
    if (end < 0) end = list.length();
    const uint32_t b = (uint32_t)list.substring(at, end).toInt();
    if (b) blocks[w][n++] = b;
    at = end + 1;
  }
  nBlocks[w] = n;
  active = w;
}

inline void put(FAUSTFLOAT* zone, float v, float lo, float hi) {
  if (zone && v == v) *zone = v < lo ? lo : (v > hi ? hi : v);   // v == v: not NaN
}

/* The keys present in `params`, applied. `cue`: a cue speaks for the whole
 * state — an absent effect turns off; live keys change only what they name. */
bool apply(const String& params, bool cue, String& reason) {
  String v;
  const bool wantsLp = (v = valueOf(params, "lowpass")).length() ? v.toFloat() >= 0.5f : (cue ? false : lpOn);
  const bool wantsRv = (v = valueOf(params, "reverb")).length()  ? v.toFloat() >= 0.5f : (cue ? false : rvOn);
  ensureLowpass();
  if ((v = valueOf(params, "lpcutoff")).length()) put(lpCutoff, v.toFloat(), 20.0f, 20000.0f);
  if ((v = valueOf(params, "lpq")).length())      put(lpQ, v.toFloat(), 0.5f, 20.0f);
  if (cue || valueOf(params, "lpblocs").length()) setBlocks(valueOf(params, "lpblocs"));
  if (wantsLp && !lpOn) lowpass.instanceClear();     // not running: the audio task does not read it
  lpOn = wantsLp && shedLevel < 2;
  if (wantsRv) {
    if (!ensureReverb(reason)) { rvOn = false; return false; }
    if ((v = valueOf(params, "rvwet")).length())   put(rvWet, v.toFloat(), 0.0f, 1.0f);
    if ((v = valueOf(params, "rvdecay")).length()) put(rvDecay, v.toFloat(), 0.1f, 4.0f);
    if ((v = valueOf(params, "rvdamp")).length())  put(rvDamp, v.toFloat(), 0.0f, 0.9f);
    if (!rvOn) reverb->instanceClear();
  }
  rvOn = wantsRv && shedLevel < 1;
  return true;
}

}  // namespace

bool configureFromCue(const String& params, String& reason) {
  shedLevel = 0;              // a new cue: what the guard dropped may come back
  shedSaid = false;
  return apply(params, true, reason);
}

void off() { lpOn = false; rvOn = false; setBlocks(String()); }

bool applyKeys(const String& params, String& reason) { return apply(params, false, reason); }

bool setParam(const char* name, float value) {
  ensureLowpass();
  if      (!strcmp(name, "lp_cutoff")) put(lpCutoff, value, 20.0f, 20000.0f);
  else if (!strcmp(name, "lp_q"))      put(lpQ, value, 0.5f, 20.0f);
  else if (!strcmp(name, "rv_wet"))    put(rvWet, value, 0.0f, 1.0f);
  else if (!strcmp(name, "rv_decay"))  put(rvDecay, value, 0.1f, 4.0f);
  else if (!strcmp(name, "rv_damp"))   put(rvDamp, value, 0.0f, 0.9f);
  else return false;
  return true;
}

bool filters(uint32_t bloc) {
  if (!lpOn || !bloc) return false;
  const uint8_t a = active, n = nBlocks[a];
  for (uint8_t i = 0; i < n; i++) if (blocks[a][i] == bloc) return true;
  return false;
}

bool lowpassOn() { return lpOn; }

void process(float* mainL, float* mainR, float* busL, float* busR, size_t frames) {
  if (frames > MAX_FRAMES) return;
  if (lpOn) {
    float* in[2]  = {busL, busR};
    float* out[2] = {tmpL, tmpR};
    lowpass.compute((int)frames, in, out);
    for (size_t i = 0; i < frames; i++) { mainL[i] += tmpL[i]; mainR[i] += tmpR[i]; }
  } else {
    for (size_t i = 0; i < frames; i++) { mainL[i] += busL[i]; mainR[i] += busR[i]; }
  }
  if (rvOn && reverb) {
    float* in[2]  = {mainL, mainR};
    float* out[2] = {tmpL, tmpR};
    reverb->compute((int)frames, in, out);
    memcpy(mainL, tmpL, frames * sizeof(float));
    memcpy(mainR, tmpR, frames * sizeof(float));
  }
}

void shed() {
  if (!rvOn && !lpOn) return;                 // nothing of ours to drop: the overload is elsewhere
  if (shedLevel < 2) shedLevel = shedLevel + 1;
  if (shedLevel >= 1) rvOn = false;
  if (shedLevel >= 2) lpOn = false;
  if (!shedSaid) {
    shedSaid = true;
    Serial.printf("[audio] core 1 over budget: %s dropped until the next cue\n",
                  shedLevel >= 2 ? "the reverb and the low-pass" : "the reverb");
  }
}

String status() {
  char s[360];
  const uint8_t a = active;
  String list;
  for (uint8_t i = 0; i < nBlocks[a]; i++) list += (i ? "," : "") + String((unsigned long)blocks[a][i]);
  snprintf(s, sizeof s,
           "{\"lowpass\":{\"on\":%s,\"cutoff\":%.1f,\"q\":%.2f,\"blocs\":[%s]},"
           "\"reverb\":{\"on\":%s,\"wet\":%.3f,\"decay\":%.2f,\"damp\":%.2f,\"bytes\":%u},\"shed\":%u}",
           lpOn ? "true" : "false", lpCutoff ? *lpCutoff : 0.0f, lpQ ? *lpQ : 0.0f, list.c_str(),
           rvOn ? "true" : "false", rvWet ? *rvWet : 0.0f, rvDecay ? *rvDecay : 0.0f, rvDamp ? *rvDamp : 0.0f,
           reverb ? (unsigned)sizeof(FaustAmbience) : 0u, (unsigned)shedLevel);
  return String(s);
}

}  // namespace Effects
