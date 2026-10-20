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
constexpr uint8_t  RING = LEAD + 1;
constexpr uint32_t TASK_STACK = 6144;
constexpr UBaseType_t TASK_PRIORITY = 17;         // under TCP/IP (18), MIDI (19), sensors (20): CLAUDE.md
constexpr uint32_t OVER_RENDERS = 40;             // the guard: 40 renders over 90 % of a block
#ifdef PLAITS_LEGER
constexpr size_t POOL = 1024;                     // the light image: the largest engine left is Swarm, 512 B (§17)
#else
constexpr size_t POOL = 16384;
#endif

/* THE NOTES, from the audio task to the core 0 task: one writer, one reader. */
struct Note { uint8_t note, velocity; };

/* ONE VOICE: its Plaits, its blocks rendered ahead, its notes, its gain, its guard. */
struct Voice {
  plaits::Voice*       voice = nullptr;           // PSRAM, kept once allocated
  char*                pool  = nullptr;
  float*               ring  = nullptr;           // RING x FRAMES, PSRAM (the audio task reads 480 B a block)
  plaits::Patch        patch;
  plaits::Modulations  mod;
  volatile uint32_t    produced = 0, consumed = 0;   // written by the task / by the audio task
  volatile bool        enabled = false, overloaded = false, flushRequested = false, primed = false;
  volatile bool        droneWanted = false;
  volatile uint32_t    blockId = 0;
  volatile uint8_t     channel = 0;
  volatile float       gainTarget = 1.0f;
  float                gainNow = 0.0f;            // audio task
  volatile int         engineNow = -1;
  Note                 notes[16];
  volatile uint8_t     notesHead = 0, notesTail = 0;
  float                velocityGain = 1.0f;       // core 0 task: the last note's velocity
  volatile uint32_t    lateBlocks = 0, rendered = 0, worstCycles = 0;
  volatile uint64_t    sumCycles = 0;
  uint32_t             overInRow = 0;             // core 0 task
  bool                 overloadSaid = false;
  volatile uint32_t    configured = 0;            // when a cue last configured it (the guard of the two)
};
Voice voices[VOICES];
plaits::Voice::Frame frames[FRAMES];              // the task's scratch, internal RAM (the voices in turn)
TaskHandle_t         task = nullptr;
volatile uint32_t    configureSeq = 0;
/* THE GUARD OF THE TWO (§212): the voices render one after the other on core 0, above the SD
 * tasks (the streams of the play lists, the kit loads) and the idle task the watchdog watches.
 * Two heavy engines together left core 0 1 % idle (measured, engine 8): past 90 % of a block
 * for both, 40 blocks in a row, the voice a cue configured FIRST — the leaving cue's — stops. */
uint32_t             bothOverInRow = 0;
bool                 bothSaid = false;

inline Voice* voiceOf(uint8_t v) { return v < VOICES ? &voices[v] : nullptr; }

void render(Voice& s, float* out) {
  bool trigger = false;
  while (s.notesTail != s.notesHead) {
    const Note n = s.notes[s.notesTail & 15];
    s.notesTail = s.notesTail + 1;
    s.patch.note = n.note;
    s.velocityGain = n.velocity / 127.0f;
    trigger = true;
  }
  s.mod.trigger_patched = !s.droneWanted;
  for (size_t i = 0; i < FRAMES; i += plaits::kBlockSize) {
    s.mod.trigger = (trigger && i == 0) ? 1.0f : 0.0f;
    s.voice->Render(s.patch, s.mod, &frames[i], plaits::kBlockSize);
  }
  const float k = s.velocityGain / 32768.0f;
  for (size_t i = 0; i < FRAMES; i++) out[i] = frames[i].out * k;
}

void taskLoop(void*) {
  const uint32_t limit = (uint32_t)((uint64_t)getCpuFrequencyMhz() * 1000000ULL * FRAMES
                                    / AudioEngine::SAMPLE_RATE * 9 / 10);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    /* Both voices, a block at a time each, until both are LEAD blocks ahead. */
    for (bool more = true; more;) {
      more = false;
      uint32_t pass = 0;
      uint8_t renderedNow = 0;
      for (Voice& s : voices) {
        if (!(s.enabled && !s.overloaded && s.voice && s.produced - s.consumed < LEAD)) continue;
        const uint32_t t = ESP.getCycleCount();
        render(s, s.ring + (s.produced % RING) * FRAMES);
        const uint32_t c = ESP.getCycleCount() - t;
        pass += c; renderedNow++;
        __sync_synchronize();                   // the samples before the counter, seen in that order
        s.produced = s.produced + 1;
        s.rendered = s.rendered + 1;
        s.sumCycles = s.sumCycles + c;
        if (c > s.worstCycles) s.worstCycles = c;
        if (c > limit) {
          if (++s.overInRow >= OVER_RENDERS) {
            s.overloaded = true;
            if (!s.overloadSaid) {
              s.overloadSaid = true;
              Serial.printf("[audio] synth voice %u: %u renders over 90 %% of a block on core 0 — stopped until the next cue\n",
                            (unsigned)(&s - voices), (unsigned)OVER_RENDERS);
            }
          }
        } else {
          s.overInRow = 0;
        }
        more = true;
      }
      if (renderedNow == VOICES) {
        if (pass > limit) {
          if (++bothOverInRow >= OVER_RENDERS) {
            bothOverInRow = 0;
            Voice& older = (voices[0].configured <= voices[1].configured) ? voices[0] : voices[1];
            older.overloaded = true;
            if (!bothSaid) {
              bothSaid = true;
              Serial.printf("[audio] two synth voices over 90 %% of a block on core 0: voice %u (the older cue's) stopped\n",
                            (unsigned)(&older - voices));
            }
          }
        } else {
          bothOverInRow = 0;
        }
      }
    }
  }
}

bool allocate(Voice& s, String& reason) {
  if (s.voice) return true;
  s.pool = (char*)heap_caps_malloc(POOL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  void* raw = heap_caps_malloc(sizeof(plaits::Voice), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  float* ring = (float*)heap_caps_calloc(RING * FRAMES, sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s.pool || !raw || !ring) {
    if (s.pool) heap_caps_free(s.pool);
    if (raw) heap_caps_free(raw);
    if (ring) heap_caps_free(ring);
    s.pool = nullptr;
    reason = String("synth voice: ") + (sizeof(plaits::Voice) + POOL + RING * FRAMES * sizeof(float))
           + " B of PSRAM not available";
    return false;
  }
  plaits::Voice* v = new (raw) plaits::Voice();
  stmlib::BufferAllocator allocator(s.pool, POOL);
  v->Init(&allocator);
  s.patch = plaits::Patch{};
  s.patch.note = 48.0f;
  s.patch.harmonics = s.patch.timbre = s.patch.morph = 0.5f;
  s.patch.decay = 0.5f;
  s.patch.lpg_colour = 0.5f;
  s.mod = plaits::Modulations{};
  s.mod.trigger_patched = true;
  s.ring = ring;
  s.voice = v;
  return true;
}

inline float unit(float v) { return (v == v) ? (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) : 0.0f; }
inline float fader(float g, float otherwise) { return (g == g && g >= 0.0f) ? (g > 3.981f ? 3.981f : g) : otherwise; }   // +12 dB

}  // namespace

bool configure(uint8_t v, int engine, uint32_t bloc, uint8_t ch, float gain, String& reason) {
  Voice* s = voiceOf(v);
  if (!s) { reason = "synth voice: no such voice"; return false; }
  s->blockId = bloc;
  s->channel = ch > 16 ? 0 : ch;
  s->gainTarget = fader(gain, 1.0f);
  if (engine < 0) { s->enabled = false; s->engineNow = -1; return true; }
  if (!allocate(*s, reason)) { s->enabled = false; return false; }
  if (!task && xTaskCreatePinnedToCore(taskLoop, "synth", TASK_STACK, nullptr, TASK_PRIORITY, &task, 0) != pdPASS) {
    task = nullptr;
    reason = "synth voice: no task on core 0";
    s->enabled = false;
    return false;
  }
  s->patch.engine = engine % 24;
  s->engineNow = engine % 24;
  if (!s->enabled || s->overloaded) { s->flushRequested = true; s->primed = false; }
  s->overloaded = false;       // a new cue: it may try again
  s->overloadSaid = false;
  s->configured = ++configureSeq;
  bothSaid = false;
  s->enabled = true;
  xTaskNotifyGive(task);
  return true;
}

void setParams(uint8_t v, const AudioEngine::Params& p) {
  Voice* s = voiceOf(v);
  if (!s) return;
  s->patch.harmonics  = unit(p.harmonics);
  s->patch.timbre     = unit(p.timbre);
  s->patch.morph      = unit(p.morph);
  s->patch.decay      = unit(p.decay);
  s->patch.lpg_colour = unit(p.lpgColour);
  s->droneWanted = p.drone >= 0.5f;
}

void setEngine(uint8_t v, int engine) {
  Voice* s = voiceOf(v);
  if (!s || engine < 0) return;
  s->patch.engine = engine % 24;
  if (s->enabled) s->engineNow = engine % 24;
}

AudioEngine::Params params(uint8_t v) {
  const Voice* s = voiceOf(v);
  if (!s) return AudioEngine::Params{};
  return AudioEngine::Params{s->patch.harmonics, s->patch.timbre, s->patch.morph, s->patch.decay,
                             s->patch.lpg_colour, s->droneWanted ? 1.0f : 0.0f};
}

uint8_t setBlockGain(uint32_t bloc, float gain) {
  if (!bloc) return 0;
  uint8_t n = 0;
  for (Voice& s : voices)
    if (s.blockId == bloc) { s.gainTarget = fader(gain, 0.0f); n++; }
  return n;
}

bool active(uint8_t v) { const Voice* s = voiceOf(v); return s && s->enabled && !s->overloaded && s->voice; }
uint32_t bloc(uint8_t v) { const Voice* s = voiceOf(v); return s ? s->blockId : 0; }

void noteOn(uint8_t v, uint8_t ch, uint8_t note, uint8_t velocity) {
  Voice* s = voiceOf(v);
  if (!s || !active(v) || !velocity) return;
  if (s->channel && ch && ch != s->channel) return;
  /* Full (16 notes within one render — never seen): this one is dropped. The
   * tail is the core 0 task's alone: the audio task never moves it. */
  if ((uint8_t)(s->notesHead - s->notesTail) >= 16) return;
  s->notes[s->notesHead & 15] = Note{note, velocity};
  s->notesHead = s->notesHead + 1;
}

bool take(uint8_t v, float* out, size_t frameCount) {
  Voice* s = voiceOf(v);
  if (!s || !active(v) || frameCount != FRAMES) return false;
  if (s->flushRequested) {                   // stale blocks of before the cue: dropped
    s->consumed = s->produced;
    s->flushRequested = false;
    xTaskNotifyGive(task);
    return false;
  }
  if (s->produced == s->consumed) {
    if (s->primed) s->lateBlocks = s->lateBlocks + 1;
    xTaskNotifyGive(task);
    return false;
  }
  s->primed = true;
  __sync_synchronize();
  const float* src = s->ring + (s->consumed % RING) * FRAMES;
  const float g0 = s->gainNow, g1 = s->gainTarget, step = (g1 - g0) / (float)FRAMES;
  for (size_t i = 0; i < FRAMES; i++) out[i] += src[i] * (g0 + step * (float)i);
  s->gainNow = g1;
  s->consumed = s->consumed + 1;
  xTaskNotifyGive(task);
  return true;
}

String status(uint8_t v) {
  const Voice* s = voiceOf(v);
  if (!s) return "null";
  char t[320];
  const uint32_t n = s->rendered;
  snprintf(t, sizeof t,
           "{\"engine\":%d,\"bloc\":%lu,\"ch\":%u,\"active\":%s,\"overload\":%s,\"lead\":%u,"
           "\"late\":%lu,\"rendered\":%lu,\"cycles_per_sample\":%.0f,\"worst_cycles_per_sample\":%.0f,"
           "\"gain\":%.3f,\"psram_bytes\":%u}",
           s->engineNow, (unsigned long)s->blockId, (unsigned)s->channel, active(v) ? "true" : "false",
           s->overloaded ? "true" : "false", (unsigned)LEAD, (unsigned long)s->lateBlocks, (unsigned long)n,
           n ? (double)s->sumCycles / ((double)n * FRAMES) : 0.0, (double)s->worstCycles / FRAMES,
           s->gainTarget, s->voice ? (unsigned)(sizeof(plaits::Voice) + POOL + RING * FRAMES * sizeof(float)) : 0u);
  return String(t);
}

}  // namespace SynthVoice
