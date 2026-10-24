// src/audio/ProcessCosts.cpp — what each process costs on this card (MESURES §231). See the header.
#include "ProcessCosts.h"
#include "../config/EcrituresDifferees.h"
#include "../diag/SurveillantFlash.h"
#include <Preferences.h>
#include <esp_heap_caps.h>

#if defined(__has_include)
#  if __has_include("../nidmi_fw_version.h")
#    include "../nidmi_fw_version.h"
#  endif
#endif
#ifndef NIDMI_FW_VERSION
#define NIDMI_FW_VERSION "dev"
#endif

namespace ProcessCosts {
namespace {

constexpr const char* NVS_SPACE = "nidmi-costs";
constexpr const char* NVS_KEY   = "v";
constexpr uint32_t SAMPLE_RATE  = 48000;
constexpr uint8_t  STAGE        = 32;     // a block's measurements: 8 voices × 2 slots, 2 kits, 8 effects
constexpr uint32_t POST_MS      = 60000;

/* One kind's meter. Each has ONE writer — core 1's kinds the audio task, the Plaits engines the
 * synth task —, read by the loop and the web: 32-bit aligned fields, never torn. */
struct Meter {
  uint32_t n = 0;          // measurements folded in
  float    mean = 0.0f;    // cycles per sample
  float    p99 = 0.0f;
};
struct Staged { uint8_t kind; float cps; };

Meter*   meters = nullptr;             // PSRAM: KINDS of them
Staged*  stage = nullptr;              // PSRAM: the audio task's block
uint8_t  staged = 0;
uint32_t flashAtBegin = 0;
uint32_t postedTotal = 0;              // the measurements the last post (or the NVS) held

/* The mean over the last ~128 measurements; the 99th percentile by a stochastic estimate — up
 * by 0.99 step when a measurement is above it, down by 0.01 step otherwise: it settles where
 * 1 % of them are above. The step follows the mean (1/256 of it). */
void fold(Meter& m, float x) {
  if (m.n == 0) { m.mean = x; m.p99 = x; }
  else {
    m.mean += (x - m.mean) * (1.0f / 128.0f);
    const float step = m.mean * (1.0f / 256.0f);
    m.p99 += (x > m.p99) ? step * 0.99f : -step * 0.01f;
    if (m.p99 < m.mean) m.p99 = m.mean;
  }
  if (m.n < 0xFFFFFFFFu) m.n++;
}

uint32_t total() {
  uint32_t t = 0;
  for (uint8_t k = 0; k < KINDS; k++) t += meters[k].n;
  return t;
}

/* The NVS form: the firmware's version, then "kind n mean p99" per kind measured. */
String serialize() {
  String s;
  s.reserve(256);
  s += NIDMI_FW_VERSION; s += '\n';
  for (uint8_t k = 0; k < KINDS; k++) {
    const Meter m = meters[k];
    if (!m.n) continue;
    char line[48];
    snprintf(line, sizeof line, "%u %lu %.1f %.1f\n", (unsigned)k, (unsigned long)m.n, m.mean, m.p99);
    s += line;
  }
  return s;
}

void load() {
  Preferences p;
  if (!p.begin(NVS_SPACE, true)) return;           // never written: nothing measured yet
  const String v = p.getString(NVS_KEY, "");
  p.end();
  const int nl = v.indexOf('\n');
  if (nl < 0 || v.substring(0, nl) != NIDMI_FW_VERSION) return;   // another firmware: measured again
  const char* c = v.c_str() + nl + 1;
  unsigned k; unsigned long n; float mean, p99; int used;
  while (sscanf(c, "%u %lu %f %f\n%n", &k, &n, &mean, &p99, &used) == 4) {
    if (k < KINDS && n && mean >= 0.0f && p99 >= mean) { meters[k].n = n; meters[k].mean = mean; meters[k].p99 = p99; }
    c += used;
  }
  postedTotal = total();
}

void meterJson(String& j, const Meter& m) {
  char b[80];
  if (!m.n) snprintf(b, sizeof b, "\"measures\":0");
  else snprintf(b, sizeof b, "\"measures\":%lu,\"mean\":%.1f,\"p99\":%.1f", (unsigned long)m.n, m.mean, m.p99);
  j += b;
}

}  // namespace

void begin() {
  if (meters) return;
  meters = (Meter*)heap_caps_calloc(KINDS, sizeof(Meter), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  stage  = (Staged*)heap_caps_calloc(STAGE, sizeof(Staged), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!meters || !stage) {
    heap_caps_free(meters); heap_caps_free(stage); meters = nullptr; stage = nullptr;
    Serial.println("[audio] process costs: no PSRAM, not measured");
    return;
  }
  load();
}

void beginBlock() {
  staged = 0;
  flashAtBegin = SurveillantFlash::cyclesSinceBoot();
}

void note(uint8_t kind, uint32_t cycles, uint32_t frames) {
  if (!stage || kind >= KINDS || !frames || staged >= STAGE) return;
  stage[staged].kind = kind;
  stage[staged].cps = (float)cycles / (float)frames;
  staged++;
}

void endBlock() {
  if (!meters) return;
  /* The flash stopped the cores during the block: what it measured is not what the processes
   * cost. */
  if (SurveillantFlash::cyclesSinceBoot() != flashAtBegin) { staged = 0; return; }
  for (uint8_t i = 0; i < staged; i++) fold(meters[stage[i].kind], stage[i].cps);
  staged = 0;
}

void noteSynth(int engine, uint32_t cycles, uint32_t frames) {
  if (!meters || engine < 0 || engine >= 24 || !frames) return;
  fold(meters[PLAITS + engine], (float)cycles / (float)frames);
}

void loop() {
  static uint32_t last = 0;
  if (!meters || millis() - last < POST_MS) return;
  last = millis();
  const uint32_t t = total();
  if (t == postedTotal) return;                     // nothing played since
  postedTotal = t;
  Differe::nvsQuietString(NVS_SPACE, NVS_KEY, serialize());
}

String json() {
  const uint32_t budget = (uint32_t)((uint64_t)getCpuFrequencyMhz() * 1000000ULL / SAMPLE_RATE);
  String j;
  j.reserve(1024);
  j += "{\"budget\":" + String(budget) + ",\"firmware\":\"" NIDMI_FW_VERSION "\",\"processes\":{";
  if (meters) {
    struct Simple { const char* id; uint8_t kind; uint8_t core; const char* per; };
    static const Simple simple[] = {
      {"low-pass", LOW_PASS, 1, "effect"}, {"ambience", AMBIENCE, 1, "effect"},
      {"play-sf", PLAY_SF, 1, "voice"}, {"play-list", PLAY_LIST, 1, "voice"},
      {"play-sfz", PLAY_SFZ, 1, "voice"},
    };
    for (const Simple& s : simple) {
      j += "\""; j += s.id; j += "\":{\"core\":"; j += String(s.core); j += ",\"per\":\""; j += s.per; j += "\",";
      meterJson(j, meters[s.kind]);
      if (s.kind == PLAY_LIST) { j += ",\"stream\":{"; meterJson(j, meters[PLAY_LIST_STREAM]); j += "}"; }
      j += "},";
    }
    j += "\"plaits\":{\"core\":0,\"per\":\"voice\",\"engines\":{";
    bool first = true;
    for (uint8_t e = 0; e < 24; e++) {
      if (!meters[PLAITS + e].n) continue;
      if (!first) j += ",";
      first = false;
      j += "\""; j += String(e); j += "\":{";
      meterJson(j, meters[PLAITS + e]);
      j += "}";
    }
    j += "}}";
  }
  j += "}}";
  return j;
}

}  // namespace ProcessCosts
