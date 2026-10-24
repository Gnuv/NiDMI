#pragma once
/* WHAT EACH PROCESS COSTS ON THIS CARD (MESURES §231). While it plays, the card measures the
 * cycles each kind of process takes — an effect, a sample voice, a kit voice, a Plaits voice per
 * engine — and declares them (GET /api/audio/costs): the app shows them in the inspector's
 * process menu, before a process is placed. Nothing is copied by hand into the app: a process
 * this card has not run yet is "not measured yet".
 *
 * The unit: cycles per sample of the core that runs it — core 1 for the audio task, core 0 for
 * the synth voices; the budget is the core's cycles per sample (5 000 at 240 MHz and 48 kHz).
 *   - an effect: one effect of a chain, on its block (Effects);
 *   - a sample voice (play-sf), a clip of a play list, from memory or streamed from the SD: one
 *     voice's whole block (AudioEngine);
 *   - a kit voice (play-sfz): the kernel's block over the voices it rendered (SfzStore);
 *   - a Plaits voice, per engine (SynthVoice, core 0).
 * What was measured while the flash stopped the cores is thrown away (§155: up to 45 ms). The
 * mean follows the last few hundred blocks; the "p99" is an estimate of the level 99 % of the
 * blocks stay under — an isolated spike (a cue that starts, an interrupt) does not move it.
 *
 * Kept across restarts — opening a composition restarts the card —, in NVS ("nidmi-costs"),
 * with the firmware's version: another firmware measures again. Written at the silence, and
 * not announced (Differe::nvsQuietString): a measurement is not something the user made, the
 * title bar's "waiting for the silence" does not wait for it. */
#include <Arduino.h>

namespace ProcessCosts {

enum Kind : uint8_t {
  LOW_PASS, AMBIENCE,            // an effect (core 1)
  PLAY_SF,                       // a sample voice (core 1)
  PLAY_LIST, PLAY_LIST_STREAM,   // a clip of a play list: from memory, streamed from the SD
  PLAY_SFZ,                      // a kit voice (core 1)
  PLAITS,                        // a Plaits voice, engine 0 (core 0) — engine e is PLAITS + e
  KINDS = PLAITS + 24
};

/* At start: the meters (PSRAM), and what this firmware measured before. Once. */
void begin();

// ── Audio task (core 1) ──────────────────────────────────────────────────────
/* Around a block: what the block measured counts only if the flash did not stop the cores. */
void beginBlock();
void endBlock();
/* One unit of `kind` took `cycles` for `frames` samples. */
void note(uint8_t kind, uint32_t cycles, uint32_t frames);

// ── Synth task (core 0) ──────────────────────────────────────────────────────
/* One Plaits voice's render, the flash's stops already excluded by the caller. */
void noteSynth(int engine, uint32_t cycles, uint32_t frames);

// ── Loop ─────────────────────────────────────────────────────────────────────
/* What was measured since the last time, posted for the silence once a minute at most. */
void loop();

/* JSON for GET /api/audio/costs. */
String json();

}  // namespace ProcessCosts
