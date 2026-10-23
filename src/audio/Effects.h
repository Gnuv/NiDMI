#pragma once
/* THE EFFECTS OF THE CARD (MESURES §209-210): a low-pass on ONE chosen part of
 * the mix, and a light reverb on the whole of it — both Faust, src/audio/faust/.
 *
 *   instruments ──┬──────────────────────────────┐
 *                 └─ those the cue names in       ├─ + ─ reverb ─ out
 *                    `lpblocs` ─ low-pass ────────┘
 *
 * The low-pass filters the instruments of ITS group in the app (the blocks of the
 * group its FX track belongs to): `lpblocs` lists their ids — a play-list, a
 * sound, the SFZ kit, the synth voice. The reverb takes the sum.
 *
 * Cue keys (engine -2): `lowpass=1;lpblocs=12,15;lpcutoff=1200;lpq=2;`
 * `reverb=1;rvwet=0.25;rvdecay=1.2;rvdamp=0.35`. A cue without `lowpass` or
 * `reverb` turns that effect off. Live: POST /api/audio/fx (the same keys), and
 * the scripts — s("sys.lp_cutoff") … (NiDMI.cpp, nidmi_sys_recevoir): the pressure
 * of a pad, smoothed by a script, opens the filter.
 *
 * Memory: the low-pass is 144 B (internal RAM); the reverb 18 KB, in PSRAM,
 * allocated the first time a cue asks for it — one of each per cue slot (§212) — it costs the same there as in
 * internal RAM (it fits in the cache, §209). Measured on core 1: low-pass
 * 150-330 cycles/sample, reverb 360-730 (3-6 % and 7-15 % of the budget). */
#include <Arduino.h>

namespace Effects {

constexpr uint8_t LOWPASS_BLOCKS_MAX = 8;
/* ONE SET PER CUE SLOT (§212): during a cross-fade the leaving cue keeps its low-pass and its
 * reverb while the arriving cue has its own — AudioEngine::CUE_SLOTS of them. */
constexpr uint8_t SLOTS = 2;

// ── Any task: the cue, the route, the scripts ────────────────────────────────
/* The cue's effects in `slot`, from its parameter string (the keys above). Allocates that
 * slot's reverb in PSRAM the first time; false + reason if it cannot. */
bool configureFromCue(uint8_t slot, const String& params, String& reason);
void off(uint8_t slot);                      // no effect in that slot
/* One parameter, live, in the slots of `slots` (a bit each): "lp_cutoff" (Hz), "lp_q", "rv_wet"
 * (0..1), "rv_decay" (s), "rv_damp" (0..1) — a script's s("sys.lp_cutoff") moves the effects
 * of ITS cue (§212). False: unknown name. */
bool setParam(const char* name, float value, uint8_t slots = 0xFF);
/* The cue keys, live (POST /api/audio/fx), in `slot`: only those present change. */
bool applyKeys(uint8_t slot, const String& params, String& reason);
String status(uint8_t slot);                 // JSON for /api/audio/status and the route
uint32_t psramBytes();                       // the reverbs allocated, kept once allocated (§217)

// ── Audio task ───────────────────────────────────────────────────────────────
/* Does the instrument of this block go through the slot's low-pass? (Read once per
 * block per voice: a word compare, no lock — the list is swapped whole.) */
bool filters(uint8_t slot, uint32_t bloc);
/* `mainL/R`: the slot's mix; `busL/R`: what its low-pass takes (cleared by the caller,
 * filled by the instruments it filters). Result in mainL/R. */
void process(uint8_t slot, float* mainL, float* mainR, float* busL, float* busR, size_t frames);
/* The slot is emptied (its Release over, or cut): what its filter and its reverb still hold
 * goes — the next cue in that slot starts from silence. */
void clear(uint8_t slot);
/* The guard (AudioEngine): core 1 is over budget — drop the reverbs, then the low-passes,
 * until the next cue. Says so once. */
void shed();

}  // namespace Effects
