/* THE SYNTH VOICE — see SynthVoice.h. */
#include "SynthVoice.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <new>
#include <PlaitsDSP.h>

namespace SynthVoice {
namespace {

constexpr size_t   FRAMES = 120;                  // AudioEngine's block (2.5 ms = 10 x plaits::kBlockSize)
constexpr uint8_t  SLOTS = LEAD + 1;
constexpr uint32_t TASK_STACK = 6144;
constexpr UBaseType_t TASK_PRIORITY = 17;         // under TCP/IP (18), MIDI (19), sensors (20): CLAUDE.md
constexpr uint32_t OVER_RENDERS = 40;             // the guard: 40 renders over 90 % of a block
#ifdef PLAITS_LEGER
constexpr size_t POOL = 1024;                     // the light image: the largest engine left is Swarm, 512 B (§17)
#else
constexpr size_t POOL = 16384;
#endif

plaits::Voice*       voice = nullptr;             // PSRAM, kept once allocated
char*                pool  = nullptr;
plaits::Patch        patch;
plaits::Modulations  mod;
plaits::Voice::Frame frames[FRAMES];              // the task's scratch, internal RAM
float                ring[SLOTS][FRAMES];         // rendered ahead, internal RAM
volatile uint32_t    produced = 0, consumed = 0;  // written by the task / by the audio task
TaskHandle_t         task = nullptr;

volatile bool     enabled = false, overloaded = false, flushRequested = false, primed = false;
volatile bool     droneWanted = false;
volatile uint32_t blockId = 0;
volatile uint8_t  channel = 0;
volatile float    gainTarget = 1.0f;
float             gainNow = 0.0f;                 // audio task
volatile int      engineNow = -1;

/* THE NOTES, from the audio task to the core 0 task: one writer, one reader. */
struct Note { uint8_t note, velocity; };
Note              notes[16];
volatile uint8_t  notesHead = 0, notesTail = 0;
float             velocityGain = 1.0f;            // core 0 task: the last note's velocity

volatile uint32_t lateBlocks = 0, rendered = 0, worstCycles = 0;
volatile uint64_t sumCycles = 0;
bool              overloadSaid = false;

void render(float* out) {
  bool trigger = false;
  while (notesTail != notesHead) {
    const Note n = notes[notesTail & 15];
    notesTail = notesTail + 1;
    patch.note = n.note;
    velocityGain = n.velocity / 127.0f;
    trigger = true;
  }
  mod.trigger_patched = !droneWanted;
  for (size_t i = 0; i < FRAMES; i += plaits::kBlockSize) {
    mod.trigger = (trigger && i == 0) ? 1.0f : 0.0f;
    voice->Render(patch, mod, &frames[i], plaits::kBlockSize);
  }
  const float k = velocityGain / 32768.0f;
  for (size_t i = 0; i < FRAMES; i++) out[i] = frames[i].out * k;
}

void taskLoop(void*) {
  const uint32_t limit = (uint32_t)((uint64_t)getCpuFrequencyMhz() * 1000000ULL * FRAMES
                                    / AudioEngine::SAMPLE_RATE * 9 / 10);
  uint32_t overInRow = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    while (enabled && !overloaded && voice && produced - consumed < LEAD) {
      const uint32_t t = ESP.getCycleCount();
      render(ring[produced % SLOTS]);
      const uint32_t c = ESP.getCycleCount() - t;
      __sync_synchronize();                     // the samples before the counter, seen in that order
      produced = produced + 1;
      rendered = rendered + 1;
      sumCycles = sumCycles + c;
      if (c > worstCycles) worstCycles = c;
      if (c > limit) {
        if (++overInRow >= OVER_RENDERS) {
          overloaded = true;
          if (!overloadSaid) {
            overloadSaid = true;
            Serial.printf("[audio] synth voice: %u renders over 90 %% of a block on core 0 — stopped until the next cue\n",
                          (unsigned)OVER_RENDERS);
          }
        }
      } else {
        overInRow = 0;
      }
    }
  }
}

bool allocate(String& reason) {
  if (voice) return true;
  pool = (char*)heap_caps_malloc(POOL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  void* raw = heap_caps_malloc(sizeof(plaits::Voice), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pool || !raw) {
    if (pool) heap_caps_free(pool);
    if (raw) heap_caps_free(raw);
    pool = nullptr;
    reason = String("synth voice: ") + (sizeof(plaits::Voice) + POOL) + " B of PSRAM not available";
    return false;
  }
  plaits::Voice* v = new (raw) plaits::Voice();
  stmlib::BufferAllocator allocator(pool, POOL);
  v->Init(&allocator);
  patch = plaits::Patch{};
  patch.note = 48.0f;
  patch.harmonics = patch.timbre = patch.morph = 0.5f;
  patch.decay = 0.5f;
  patch.lpg_colour = 0.5f;
  mod = plaits::Modulations{};
  mod.trigger_patched = true;
  voice = v;
  return true;
}

inline float unit(float v) { return (v == v) ? (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) : 0.0f; }

}  // namespace

bool configure(int engine, uint32_t bloc, uint8_t ch, float gain, String& reason) {
  blockId = bloc;
  channel = ch > 16 ? 0 : ch;
  gainTarget = (gain == gain && gain >= 0.0f) ? (gain > 3.981f ? 3.981f : gain) : 1.0f;   // the faders' +12 dB
  if (engine < 0) { enabled = false; engineNow = -1; return true; }
  if (!allocate(reason)) { enabled = false; return false; }
  if (!task && xTaskCreatePinnedToCore(taskLoop, "synth", TASK_STACK, nullptr, TASK_PRIORITY, &task, 0) != pdPASS) {
    task = nullptr;
    reason = "synth voice: no task on core 0";
    enabled = false;
    return false;
  }
  patch.engine = engine % 24;
  engineNow = engine % 24;
  if (!enabled || overloaded) { flushRequested = true; primed = false; }
  overloaded = false;          // a new cue: it may try again
  overloadSaid = false;
  enabled = true;
  xTaskNotifyGive(task);
  return true;
}

void setParams(const AudioEngine::Params& p) {
  patch.harmonics  = unit(p.harmonics);
  patch.timbre     = unit(p.timbre);
  patch.morph      = unit(p.morph);
  patch.decay      = unit(p.decay);
  patch.lpg_colour = unit(p.lpgColour);
  droneWanted = p.drone >= 0.5f;
}

AudioEngine::Params params() {
  return AudioEngine::Params{patch.harmonics, patch.timbre, patch.morph, patch.decay, patch.lpg_colour,
                             droneWanted ? 1.0f : 0.0f};
}

uint8_t setBlockGain(uint32_t bloc, float gain) {
  if (!bloc || bloc != blockId) return 0;
  gainTarget = (gain == gain && gain >= 0.0f) ? (gain > 3.981f ? 3.981f : gain) : 0.0f;
  return 1;
}

bool active() { return enabled && !overloaded && voice; }
uint32_t bloc() { return blockId; }

void noteOn(uint8_t ch, uint8_t note, uint8_t velocity) {
  if (!active() || !velocity) return;
  if (channel && ch && ch != channel) return;
  /* Full (16 notes within one render — never seen): this one is dropped. The
   * tail is the core 0 task's alone: the audio task never moves it. */
  if ((uint8_t)(notesHead - notesTail) >= 16) return;
  notes[notesHead & 15] = Note{note, velocity};
  notesHead = notesHead + 1;
}

bool take(float* out, size_t frameCount) {
  if (!active() || frameCount != FRAMES) return false;
  if (flushRequested) {                      // stale blocks of before the cue: dropped
    consumed = produced;
    flushRequested = false;
    xTaskNotifyGive(task);
    return false;
  }
  if (produced == consumed) {
    if (primed) lateBlocks = lateBlocks + 1;
    xTaskNotifyGive(task);
    return false;
  }
  primed = true;
  __sync_synchronize();
  const float* src = ring[consumed % SLOTS];
  const float g0 = gainNow, g1 = gainTarget, step = (g1 - g0) / (float)FRAMES;
  for (size_t i = 0; i < FRAMES; i++) out[i] += src[i] * (g0 + step * (float)i);
  gainNow = g1;
  consumed = consumed + 1;
  xTaskNotifyGive(task);
  return true;
}

String status() {
  char s[320];
  const uint32_t n = rendered;
  snprintf(s, sizeof s,
           "{\"engine\":%d,\"bloc\":%lu,\"ch\":%u,\"active\":%s,\"overload\":%s,\"lead\":%u,"
           "\"late\":%lu,\"rendered\":%lu,\"cycles_per_sample\":%.0f,\"worst_cycles_per_sample\":%.0f,"
           "\"gain\":%.3f,\"psram_bytes\":%u}",
           engineNow, (unsigned long)blockId, (unsigned)channel, active() ? "true" : "false",
           overloaded ? "true" : "false", (unsigned)LEAD, (unsigned long)lateBlocks, (unsigned long)n,
           n ? (double)sumCycles / ((double)n * FRAMES) : 0.0, (double)worstCycles / FRAMES,
           gainTarget, voice ? (unsigned)(sizeof(plaits::Voice) + POOL) : 0u);
  return String(s);
}

}  // namespace SynthVoice
