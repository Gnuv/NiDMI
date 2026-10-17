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
#include "faust/FaustAmbience.h"

namespace DspBench {
namespace {

#ifdef PLAITS_LEGER
constexpr size_t PLAITS_POOL = 1024;    // the light image: the largest engine left is Swarm, 512 B (§17)
#else
constexpr size_t PLAITS_POOL = 16384;
#endif
constexpr size_t   MAX_FRAMES = 128;
constexpr uint32_t OVERLOAD_BLOCKS = 20;           // 50 ms over budget in a row: the bench stops itself
constexpr uint32_t WORKER_STACK = 6144;
constexpr uint32_t WORKER_WAIT_MAX_US = 1000;      // core 1 waits this long for the worker, then goes without

struct PlaitsVoice {
  plaits::Voice*      voice = nullptr;
  char*               pool  = nullptr;
  plaits::Patch       patch;
  plaits::Modulations mod;
  uint32_t            blocksToNote = 0;
  uint8_t             step = 0;
};

Config         config;                    // written only while nothing runs
PlaitsVoice    voices[2];
FaustLowpass*  lowpass  = nullptr;
FaustFreeverb* freeverb = nullptr;
FaustAmbience* ambience = nullptr;
uint32_t       plaitsBytes[2] = {0, 0};
uint32_t       sampleRate = AudioEngine::SAMPLE_RATE;
uint32_t       budgetPerSample = 5000;

volatile bool     running = false;
volatile uint32_t blocksSeen = 0;         // every block, running or not: configure() waits on it
volatile bool     resetRequested = false, workerResetRequested = false;
volatile uint32_t overBlocks = 0;
volatile bool     stoppedOverload = false;

// Audio-path scratch, internal RAM.
float bufL[MAX_FRAMES], bufR[MAX_FRAMES], outL[MAX_FRAMES], outR[MAX_FRAMES];
plaits::Voice::Frame plaitsFrames[MAX_FRAMES];

/* THE CORE 0 WORKER. Core 1 consumes slot `slot` (rendered during the previous
 * block) and asks for slot ^ 1; the worker renders it while core 1 goes on. */
TaskHandle_t         worker = nullptr;
float                workBuf[2][MAX_FRAMES];
plaits::Voice::Frame workerFrames[MAX_FRAMES];
volatile uint8_t     workerTarget = 0;
volatile bool        workerFilled[2] = {false, false};
volatile bool        workerBusy = false;
volatile size_t      workerFrameCount = 120;
uint8_t              slot = 0;
bool                 primed = false;
uint32_t             workerLate = 0;

const uint8_t PATTERN[8] = {48, 55, 60, 63, 67, 70, 72, 75};

struct Stage { uint64_t sum = 0; uint32_t worst = 0; };
enum { RENDER, PLAITS, LOWPASS, REVERB, WAIT, BENCH, TOTAL, WORKER, STAGES };
const char* const STAGE_NAMES[STAGES] = {"render", "plaits", "lowpass", "reverb", "wait", "bench", "total", "worker"};
Stage    stages[STAGES];
uint32_t blocksMeasured = 0, workerBlocks = 0, framesPerBlock = 0;

inline void account(int s, uint32_t cycles) {
  stages[s].sum += cycles;
  if (cycles > stages[s].worst) stages[s].worst = cycles;
}

/* The voices of the bench, mixed (mono, both channels) into `outMono` — on
 * core 1 or on the worker, never both: `plaitsCore` decides. */
void renderPlaits(float* outMono, plaits::Voice::Frame* frames, size_t n) {
  for (size_t i = 0; i < n; i++) outMono[i] = 0.0f;
  const float g = config.plaitsGain / 32768.0f;
  const uint32_t period = (uint32_t)config.notePeriodMs * sampleRate / 1000 / n + 1;
  for (uint8_t k = 0; k < config.plaitsVoices; k++) {
    PlaitsVoice& v = voices[k];
    if (v.blocksToNote == 0) {
      v.patch.note = PATTERN[v.step++ & 7] - 12 * k;
      v.mod.trigger = 1.0f;
      v.blocksToNote = period;
    }
    v.blocksToNote--;
    for (size_t i = 0; i < n; i += plaits::kBlockSize) {
      v.voice->Render(v.patch, v.mod, &frames[i], plaits::kBlockSize);
      v.mod.trigger = 0.0f;
    }
    for (size_t i = 0; i < n; i++) outMono[i] += frames[i].out * g;
  }
}

void workerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    workerBusy = true;                  // before the test: configure() frees only an idle worker's voices
    if (!running || config.plaitsCore != 0) { workerBusy = false; continue; }
    if (workerResetRequested) { stages[WORKER] = Stage{}; workerBlocks = 0; workerResetRequested = false; }
    const uint8_t target = workerTarget;
    const uint32_t t = ESP.getCycleCount();
    renderPlaits(workBuf[target], workerFrames, workerFrameCount);
    account(WORKER, ESP.getCycleCount() - t);
    workerBlocks++;
    __sync_synchronize();               // the samples before the flag, seen in that order by core 1
    workerFilled[target] = true;
    workerBusy = false;
  }
}

void* allocate(size_t bytes, bool psram) {
  return heap_caps_malloc(bytes, psram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                       : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

template <class T> void destroy(T*& p) {
  if (p) { p->~T(); heap_caps_free(p); p = nullptr; }
}

void freeAll() {
  for (auto& v : voices) {
    if (v.voice) { v.voice->~Voice(); heap_caps_free(v.voice); v.voice = nullptr; }
    if (v.pool)  { heap_caps_free(v.pool); v.pool = nullptr; }
  }
  plaitsBytes[0] = plaitsBytes[1] = 0;
  destroy(lowpass);
  destroy(freeverb);
  destroy(ambience);
}

template <class T> T* makeEffect(bool psram, String& reason, const char* name) {
  void* raw = allocate(sizeof(T), psram);
  if (!raw) {
    reason = String(name) + ": " + sizeof(T) + " B not available in " + (psram ? "PSRAM" : "internal RAM");
    return nullptr;
  }
  T* d = new (raw) T();
  d->init((int)sampleRate);
  return d;
}

void setParam(FaustDsp* d, const char* label, float value) {
  FaustParams p;
  d->buildUserInterface(&p);
  if (FAUSTFLOAT* zone = p.find(label)) *zone = value;
}

inline void swapBuffers(float** in, float** out) {
  float* s0 = in[0]; float* s1 = in[1];
  in[0] = out[0]; in[1] = out[1]; out[0] = s0; out[1] = s1;
}

}  // namespace

void process(int16_t* io, size_t frames, uint32_t renderCycles) {
  blocksSeen = blocksSeen + 1;
  if (!running || frames > MAX_FRAMES) return;
  if (resetRequested) {
    for (int s = 0; s < STAGES; s++) if (s != WORKER) stages[s] = Stage{};
    blocksMeasured = 0;
    workerLate = 0;
    workerResetRequested = true;
    resetRequested = false;
  }
  const uint32_t c0 = ESP.getCycleCount();
  constexpr float IN = 1.0f / 32768.0f;
  for (size_t i = 0; i < frames; i++) { bufL[i] = io[i * 2] * IN; bufR[i] = io[i * 2 + 1] * IN; }

  uint32_t cPlaits = 0, cLowpass = 0, cReverb = 0, cWait = 0;
  if (config.plaitsVoices && config.plaitsCore == 0 && worker) {
    /* What the worker rendered during the previous block, then the next one. */
    const uint32_t t = ESP.getCycleCount();
    if (primed) {
      const uint32_t limit = WORKER_WAIT_MAX_US * (budgetPerSample * sampleRate / 1000000);
      while (!workerFilled[slot] && ESP.getCycleCount() - t < limit) {}
      cWait = ESP.getCycleCount() - t;
      if (workerFilled[slot]) {
        __sync_synchronize();
        for (size_t i = 0; i < frames; i++) { bufL[i] += workBuf[slot][i]; bufR[i] += workBuf[slot][i]; }
        workerFilled[slot] = false;
      } else {
        workerLate++;
      }
    }
    slot ^= 1;
    workerTarget = slot;
    workerFrameCount = frames;
    primed = true;
    xTaskNotifyGive(worker);
    cPlaits = ESP.getCycleCount() - t - cWait;     // the mix and the hand-over
  } else if (config.plaitsVoices) {
    const uint32_t t = ESP.getCycleCount();
    renderPlaits(outL, plaitsFrames, frames);
    for (size_t i = 0; i < frames; i++) { bufL[i] += outL[i]; bufR[i] += outL[i]; }
    cPlaits = ESP.getCycleCount() - t;
  }
  float* in[2]  = {bufL, bufR};
  float* out[2] = {outL, outR};
  if (lowpass) {
    const uint32_t t = ESP.getCycleCount();
    lowpass->compute((int)frames, in, out);
    cLowpass = ESP.getCycleCount() - t;
    swapBuffers(in, out);
  }
  if (freeverb || ambience) {
    const uint32_t t = ESP.getCycleCount();
    if (freeverb) freeverb->compute((int)frames, in, out);
    else          ambience->compute((int)frames, in, out);
    cReverb = ESP.getCycleCount() - t;
    swapBuffers(in, out);
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
  account(WAIT, cWait);
  account(BENCH, bench);
  account(TOTAL, renderCycles + bench);
  blocksMeasured++;
  framesPerBlock = frames;
  /* THE GUARD: core 1 over a block's budget, 20 blocks in a row — the audio
   * task would never yield again, and the watchdog restarts the card. */
  if (renderCycles + bench > budgetPerSample * frames) {
    overBlocks = overBlocks + 1;
    if (overBlocks >= OVERLOAD_BLOCKS) { running = false; stoppedOverload = true; }
  } else {
    overBlocks = 0;
  }
}

bool configure(const Config& c, String& reason) {
  running = false;
  /* The audio task may be inside process(), the worker inside its render: wait
   * for two more blocks and an idle worker. Without a running audio task (no
   * block within 50 ms), nothing reads the objects either. */
  const uint32_t b0 = blocksSeen;
  for (int i = 0; i < 50 && blocksSeen - b0 < 2; i++) vTaskDelay(pdMS_TO_TICKS(1));
  for (int i = 0; i < 50 && workerBusy; i++) vTaskDelay(pdMS_TO_TICKS(1));
  freeAll();
  config = c;
  if (config.plaitsVoices > 2) config.plaitsVoices = 2;
  primed = false;
  workerFilled[0] = workerFilled[1] = false;
  stoppedOverload = false;
  overBlocks = 0;
  const AudioEngine::Metriques m = AudioEngine::metriques();
  if (m.sampleRateReel) sampleRate = m.sampleRateReel;
  budgetPerSample = (uint32_t)((uint64_t)getCpuFrequencyMhz() * 1000000ULL / sampleRate);

  for (uint8_t n = 0; n < config.plaitsVoices; n++) {
    PlaitsVoice& v = voices[n];
    const uint32_t caps = config.plaitsPsram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
    const uint32_t before = heap_caps_get_free_size(caps);
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
    plaitsBytes[n] = before - heap_caps_get_free_size(caps);
  }
  if (config.plaitsVoices && config.plaitsCore == 0) {
    if (!worker &&
        xTaskCreatePinnedToCore(workerTask, "dsp-bench", WORKER_STACK, nullptr,
                                config.workerPriority, &worker, 0) != pdPASS) {
      worker = nullptr;
      reason = "the core 0 worker: no task";
      freeAll();
      return false;
    }
    vTaskPrioritySet(worker, config.workerPriority);
  }
  if (config.lowpass) {
    if (!(lowpass = makeEffect<FaustLowpass>(false, reason, "low-pass"))) { freeAll(); return false; }
    setParam(lowpass, "cutoff", config.cutoff);
    setParam(lowpass, "q", config.q);
  }
  if (config.reverb == REVERB_FREEVERB) {
    if (!(freeverb = makeEffect<FaustFreeverb>(config.reverbPsram, reason, "freeverb"))) { freeAll(); return false; }
    setParam(freeverb, "Wet", config.wet);
    setParam(freeverb, "RoomSize", config.room);
    setParam(freeverb, "Damp", config.damp);
  } else if (config.reverb == REVERB_AMBIENCE) {
    if (!(ambience = makeEffect<FaustAmbience>(config.reverbPsram, reason, "ambience"))) { freeAll(); return false; }
    setParam(ambience, "decay", config.decay);
    setParam(ambience, "damp", config.adamp);
    setParam(ambience, "wet", config.awet);
  }
  resetRequested = true;
  running = config.plaitsVoices || lowpass || freeverb || ambience;
  return true;
}

void resetCounters() { resetRequested = true; }

String report() {
  const AudioEngine::Metriques m = AudioEngine::metriques();
  char head[900];
  snprintf(head, sizeof head,
           "{\"running\":%s,\"stopped\":\"%s\",\"config\":{\"plaits\":%u,\"engines\":[%u,%u],\"plaits_where\":\"%s\","
           "\"plaits_core\":%u,\"worker_prio\":%u,\"lowpass\":%s,\"reverb\":\"%s\",\"reverb_where\":\"%s\","
           "\"audible\":%s,\"note_ms\":%u,\"cutoff\":%.1f,\"q\":%.2f,\"wet\":%.3f,\"room\":%.3f,\"damp\":%.3f,"
           "\"decay\":%.2f,\"adamp\":%.2f,\"awet\":%.2f},"
           "\"memory\":{\"plaits_bytes\":[%u,%u],\"lowpass_bytes\":%u,\"reverb_bytes\":%u,\"worker_stack\":%u,"
           "\"internal_free\":%u,\"internal_largest\":%u,\"psram_free\":%u},"
           "\"sample_rate\":%u,\"budget_cycles_per_sample\":%u,\"blocks\":%u,\"worker_blocks\":%u,\"worker_late\":%u,"
           "\"late_blocks_since_boot\":%u,\"engine\":%d,",
           running ? "true" : "false", stoppedOverload ? "overload" : "",
           config.plaitsVoices, config.engines[0], config.engines[1],
           config.plaitsPsram ? "psram" : "internal", config.plaitsCore, config.workerPriority,
           lowpass ? "true" : "false",
           freeverb ? "freeverb" : (ambience ? "ambience" : "none"),
           config.reverbPsram ? "psram" : "internal", config.audible ? "true" : "false", config.notePeriodMs,
           config.cutoff, config.q, config.wet, config.room, config.damp, config.decay, config.adamp, config.awet,
           (unsigned)plaitsBytes[0], (unsigned)plaitsBytes[1],
           lowpass ? (unsigned)sizeof(FaustLowpass) : 0u,
           freeverb ? (unsigned)sizeof(FaustFreeverb) : (ambience ? (unsigned)sizeof(FaustAmbience) : 0u),
           worker ? (unsigned)WORKER_STACK : 0u,
           (unsigned)m.heapLibre, (unsigned)m.heapPlusGrosBloc, (unsigned)m.psramLibre,
           (unsigned)sampleRate, (unsigned)budgetPerSample, (unsigned)blocksMeasured, (unsigned)workerBlocks,
           (unsigned)workerLate, (unsigned)m.sousAlimentations, m.moteur);
  String s = head;
  s += "\"cycles_per_sample\":{";
  const uint32_t f = framesPerBlock ? framesPerBlock : 1;
  for (int i = 0; i < STAGES; i++) {
    const uint32_t n = (i == WORKER) ? workerBlocks : blocksMeasured;
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
