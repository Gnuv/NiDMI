/* THE DSP BENCH — see DspBench.h. Compiled -O3, like sfz16.c: the regime a DSP
 * kernel is measured in (the rest of the firmware is -Os, MESURES §208). */
#pragma GCC optimize ("O3")
#include "DspBench.h"

#if NIDMI_DSP_BENCH

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <new>
#include <PlaitsDSP.h>
#include "AudioEngine.h"
#include "faust/FaustLowpass.h"
#include "faust/FaustFreeverb.h"

namespace DspBench {
namespace {

#ifdef PLAITS_LEGER
constexpr size_t PLAITS_POOL = 1024;    // the light image: the largest engine left is Swarm, 512 B (§17)
#else
constexpr size_t PLAITS_POOL = 16384;
#endif
constexpr size_t MAX_FRAMES = 128;

struct PlaitsVoice {
  plaits::Voice*      voice = nullptr;
  char*               pool  = nullptr;
  plaits::Patch       patch;
  plaits::Modulations mod;
  uint32_t            blocksToNote = 0;
  uint8_t             step = 0;
};

Config        config;                     // written only while `running` is false
PlaitsVoice   voices[2];
FaustLowpass*  lowpass = nullptr;
FaustFreeverb* reverb  = nullptr;
uint32_t      plaitsBytes[2] = {0, 0};
uint32_t      sampleRate = AudioEngine::SAMPLE_RATE;

volatile bool     running = false;
volatile uint32_t blocksSeen = 0;        // every block, running or not: configure() waits on it
volatile bool     resetRequested = false;

// Audio-path scratch, internal RAM.
float bufL[MAX_FRAMES], bufR[MAX_FRAMES], outL[MAX_FRAMES], outR[MAX_FRAMES];
plaits::Voice::Frame plaitsFrames[MAX_FRAMES];

const uint8_t PATTERN[8] = {48, 55, 60, 63, 67, 70, 72, 75};

struct Stage { uint64_t sum = 0; uint32_t worst = 0; };
enum { RENDER, PLAITS, LOWPASS, REVERB, BENCH, TOTAL, STAGES };
const char* const STAGE_NAMES[STAGES] = {"render", "plaits", "lowpass", "reverb", "bench", "total"};
Stage    stages[STAGES];
uint32_t blocksMeasured = 0, framesPerBlock = 0;

inline void account(int s, uint32_t cycles) {
  stages[s].sum += cycles;
  if (cycles > stages[s].worst) stages[s].worst = cycles;
}

void* allocate(size_t bytes, bool psram) {
  return heap_caps_malloc(bytes, psram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                       : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void freeAll() {
  for (auto& v : voices) {
    if (v.voice) { v.voice->~Voice(); heap_caps_free(v.voice); v.voice = nullptr; }
    if (v.pool)  { heap_caps_free(v.pool); v.pool = nullptr; }
  }
  plaitsBytes[0] = plaitsBytes[1] = 0;
  if (lowpass) { lowpass->~FaustLowpass(); heap_caps_free(lowpass); lowpass = nullptr; }
  if (reverb)  { reverb->~FaustFreeverb(); heap_caps_free(reverb);  reverb  = nullptr; }
}

bool setParam(FaustDsp* d, const char* label, float value) {
  FaustParams p;
  d->buildUserInterface(&p);
  FAUSTFLOAT* zone = p.find(label);
  if (zone) *zone = value;
  return zone != nullptr;
}

}  // namespace

void process(int16_t* io, size_t frames, uint32_t renderCycles) {
  blocksSeen = blocksSeen + 1;
  if (!running || frames > MAX_FRAMES) return;
  if (resetRequested) {
    for (auto& s : stages) s = Stage{};
    blocksMeasured = 0;
    resetRequested = false;
  }
  const uint32_t c0 = ESP.getCycleCount();
  constexpr float IN = 1.0f / 32768.0f;
  for (size_t i = 0; i < frames; i++) { bufL[i] = io[i * 2] * IN; bufR[i] = io[i * 2 + 1] * IN; }

  uint32_t cPlaits = 0, cLowpass = 0, cReverb = 0;
  if (config.plaitsVoices) {
    const uint32_t t = ESP.getCycleCount();
    const float g = config.plaitsGain * IN;
    const uint32_t period = (uint32_t)config.notePeriodMs * sampleRate / 1000 / frames + 1;
    for (uint8_t n = 0; n < config.plaitsVoices; n++) {
      PlaitsVoice& v = voices[n];
      if (v.blocksToNote == 0) {
        v.patch.note = PATTERN[v.step++ & 7] - 12 * n;
        v.mod.trigger = 1.0f;
        v.blocksToNote = period;
      }
      v.blocksToNote--;
      for (size_t i = 0; i < frames; i += plaits::kBlockSize) {
        v.voice->Render(v.patch, v.mod, &plaitsFrames[i], plaits::kBlockSize);
        v.mod.trigger = 0.0f;
      }
      for (size_t i = 0; i < frames; i++) {
        const float s = plaitsFrames[i].out * g;
        bufL[i] += s; bufR[i] += s;
      }
    }
    cPlaits = ESP.getCycleCount() - t;
  }
  float* in[2]  = {bufL, bufR};
  float* out[2] = {outL, outR};
  if (lowpass) {
    const uint32_t t = ESP.getCycleCount();
    lowpass->compute((int)frames, in, out);
    cLowpass = ESP.getCycleCount() - t;
    float* s0 = in[0]; float* s1 = in[1]; in[0] = out[0]; in[1] = out[1]; out[0] = s0; out[1] = s1;
  }
  if (reverb) {
    const uint32_t t = ESP.getCycleCount();
    reverb->compute((int)frames, in, out);
    cReverb = ESP.getCycleCount() - t;
    float* s0 = in[0]; float* s1 = in[1]; in[0] = out[0]; in[1] = out[1]; out[0] = s0; out[1] = s1;
  }
  if (config.audible) {
    for (size_t i = 0; i < frames; i++) {
      float l = in[0][i] * 32767.0f, r = in[1][i] * 32767.0f;
      l = l > 32767.0f ? 32767.0f : (l < -32768.0f ? -32768.0f : l);
      r = r > 32767.0f ? 32767.0f : (r < -32768.0f ? -32768.0f : r);
      io[i * 2] = (int16_t)l; io[i * 2 + 1] = (int16_t)r;
    }
  }
  const uint32_t bench = ESP.getCycleCount() - c0;
  account(RENDER, renderCycles);
  account(PLAITS, cPlaits);
  account(LOWPASS, cLowpass);
  account(REVERB, cReverb);
  account(BENCH, bench);
  account(TOTAL, renderCycles + bench);
  blocksMeasured++;
  framesPerBlock = frames;
}

bool configure(const Config& c, String& reason) {
  running = false;
  /* The audio task may be inside process(): wait until it has seen two more
   * blocks. If it is not running at all (no block within 50 ms), nothing reads
   * the objects either. */
  const uint32_t b0 = blocksSeen;
  for (int i = 0; i < 50 && blocksSeen - b0 < 2; i++) vTaskDelay(pdMS_TO_TICKS(1));
  freeAll();
  config = c;
  if (config.plaitsVoices > 2) config.plaitsVoices = 2;
  const AudioEngine::Metriques m = AudioEngine::metriques();
  if (m.sampleRateReel) sampleRate = m.sampleRateReel;

  for (uint8_t n = 0; n < config.plaitsVoices; n++) {
    PlaitsVoice& v = voices[n];
    const uint32_t before = heap_caps_get_free_size(config.plaitsPsram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    v.pool = (char*)allocate(PLAITS_POOL, config.plaitsPsram);
    void* raw = allocate(sizeof(plaits::Voice), config.plaitsPsram);
    if (!v.pool || !raw) {
      if (raw) heap_caps_free(raw);
      reason = String("Plaits voice ") + n + ": " + (sizeof(plaits::Voice) + PLAITS_POOL) +
               " B not available in " + (config.plaitsPsram ? "PSRAM" : "internal RAM");
      freeAll();
      return false;
    }
    v.voice = new (raw) plaits::Voice();
    stmlib::BufferAllocator allocator(v.pool, PLAITS_POOL);
    v.voice->Init(&allocator);
    v.patch = plaits::Patch{};
    v.patch.note = 48.0f;
    v.patch.harmonics = v.patch.timbre = v.patch.morph = 0.5f;
    v.patch.decay = 0.5f; v.patch.lpg_colour = 0.5f;
    v.patch.engine = config.engines[n] % 24;
    v.mod = plaits::Modulations{};
    v.mod.trigger_patched = true;
    v.blocksToNote = n * 7;            // the second voice is off the first's beat
    v.step = n * 3;
    plaitsBytes[n] = before - heap_caps_get_free_size(config.plaitsPsram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
  }
  if (config.lowpass) {
    void* raw = allocate(sizeof(FaustLowpass), false);
    if (!raw) { reason = "low-pass: internal RAM"; freeAll(); return false; }
    lowpass = new (raw) FaustLowpass();
    lowpass->init((int)sampleRate);
    setParam(lowpass, "cutoff", config.cutoff);
    setParam(lowpass, "q", config.q);
  }
  if (config.reverb) {
    void* raw = allocate(sizeof(FaustFreeverb), config.reverbPsram);
    if (!raw) {
      reason = String("freeverb: ") + sizeof(FaustFreeverb) + " B not available in " +
               (config.reverbPsram ? "PSRAM" : "internal RAM");
      freeAll();
      return false;
    }
    reverb = new (raw) FaustFreeverb();
    reverb->init((int)sampleRate);
    setParam(reverb, "Wet", config.wet);
    setParam(reverb, "RoomSize", config.room);
    setParam(reverb, "Damp", config.damp);
  }
  resetRequested = true;
  running = config.plaitsVoices || lowpass || reverb;
  return true;
}

void resetCounters() { resetRequested = true; }

String report() {
  const AudioEngine::Metriques m = AudioEngine::metriques();
  const uint32_t budget = (uint32_t)((uint64_t)getCpuFrequencyMhz() * 1000000ULL / (sampleRate ? sampleRate : 48000));
  char head[640];
  snprintf(head, sizeof head,
           "{\"running\":%s,\"config\":{\"plaits\":%u,\"engines\":[%u,%u],\"plaits_where\":\"%s\","
           "\"lowpass\":%s,\"reverb\":%s,\"reverb_where\":\"%s\",\"audible\":%s,\"note_ms\":%u,"
           "\"cutoff\":%.1f,\"q\":%.2f,\"wet\":%.3f,\"room\":%.3f,\"damp\":%.3f},"
           "\"memory\":{\"plaits_bytes\":[%u,%u],\"lowpass_bytes\":%u,\"reverb_bytes\":%u,"
           "\"internal_free\":%u,\"internal_largest\":%u,\"psram_free\":%u},"
           "\"sample_rate\":%u,\"budget_cycles_per_sample\":%u,\"blocks\":%u,"
           "\"late_blocks_since_boot\":%u,\"engine\":%d,",
           running ? "true" : "false", config.plaitsVoices, config.engines[0], config.engines[1],
           config.plaitsPsram ? "psram" : "internal", lowpass ? "true" : "false", reverb ? "true" : "false",
           config.reverbPsram ? "psram" : "internal", config.audible ? "true" : "false", config.notePeriodMs,
           config.cutoff, config.q, config.wet, config.room, config.damp,
           (unsigned)plaitsBytes[0], (unsigned)plaitsBytes[1],
           lowpass ? (unsigned)sizeof(FaustLowpass) : 0u, reverb ? (unsigned)sizeof(FaustFreeverb) : 0u,
           (unsigned)m.heapLibre, (unsigned)m.heapPlusGrosBloc, (unsigned)m.psramLibre,
           (unsigned)sampleRate, (unsigned)budget, (unsigned)blocksMeasured,
           (unsigned)m.sousAlimentations, m.moteur);
  String s = head;
  s += "\"cycles_per_sample\":{";
  const uint32_t n = blocksMeasured, f = framesPerBlock ? framesPerBlock : 1;
  for (int i = 0; i < STAGES; i++) {
    char part[96];
    const double avg = n ? (double)stages[i].sum / ((double)n * f) : 0.0;
    snprintf(part, sizeof part, "%s\"%s\":{\"avg\":%.1f,\"worst\":%.1f}", i ? "," : "",
             STAGE_NAMES[i], avg, (double)stages[i].worst / f);
    s += part;
  }
  s += "}}";
  return s;
}

}  // namespace DspBench

#endif
