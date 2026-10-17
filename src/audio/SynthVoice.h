#pragma once
/* THE SYNTH VOICE: one Plaits, BESIDE the sampler (MESURES §209-210).
 *
 * Until now Plaits was an ENGINE, exclusive: a cue played sounds or Plaits, and
 * a single card refused Plaits for memory (§127). Measured on 17 October: its
 * state costs the same in PSRAM as in internal RAM, and there it takes no
 * internal RAM at all; and core 1 cannot hold it with the SFZ kit and the
 * effects, while core 0 is 92 % idle by the cable. So:
 *
 *   - one voice, its state (Voice + scratch pool) in PSRAM, kept once allocated;
 *   - rendered by a task of its own on CORE 0, under MIDI and TCP/IP, LEAD blocks
 *     ahead of the audio task: what TCP/IP or MIDI take from it is absorbed, not
 *     heard (one block ahead, a reloaded page made it miss 11-14 blocks in 8 s);
 *     a note therefore sounds LEAD x 2.5 ms later than a sample;
 *   - monophonic, last note wins; the LPG makes the release (a note-off does
 *     nothing, as before), unless `drone`;
 *   - it listens to ONE MIDI channel, its track's (0: all).
 *
 * Cue keys (engine -2): `synth=<engine 0..23>;synthbloc=<id>;synthch=<0..16>;`
 * `synthgain=<g>;harmonics=…;timbre=…;morph=…;decay=…;lpg_colour=…;drone=0|1`.
 * A cue without `synth=` silences it. Live: /api/audio/params (the five continuous
 * and drone), the CC table (harmonics…), the block's fader (sampler/gain).
 *
 * ITS GUARD: if a render takes more than 90 % of a block, 40 renders in a row,
 * the voice stops until the next cue and says so — a task that never sleeps
 * starves core 0's idle task, and the watchdog restarted the card (§209). */
#include <Arduino.h>
#include "AudioEngine.h"

namespace SynthVoice {

constexpr uint8_t LEAD = 3;          // blocks rendered ahead: +7.5 ms for a note

// ── Any task: the cue, the routes, the CC table ──────────────────────────────
/* The cue's voice: `engine` 0..23, or -1 (none: silent, its state kept). The
 * first engine >= 0 allocates the voice in PSRAM and starts the core 0 task;
 * false + reason if that fails. */
bool configure(int engine, uint32_t bloc, uint8_t channel, float gain, String& reason);
void setParams(const AudioEngine::Params& p);
/* The engine, live (POST /api/audio/params synth_engine=…): a write, nothing
 * allocated — a game gesture. The next cue's `synth=` says it again. */
void setEngine(int engine);
AudioEngine::Params params();
uint8_t setBlockGain(uint32_t bloc, float gain);   // 1 if the block is the voice's
bool active();                       // a cue names it, and the guard has not stopped it
String status();                     // JSON

// ── Audio task only ──────────────────────────────────────────────────────────
void noteOn(uint8_t channel, uint8_t note, uint8_t velocity);
uint32_t bloc();
/* The next block the core 0 task rendered, mono, ADDED into `out` at the voice's
 * gain. False when it is not active, or late (nothing ready: silence, counted). */
bool take(float* out, size_t frames);

}  // namespace SynthVoice
