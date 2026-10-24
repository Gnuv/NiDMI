#pragma once
/* THE EFFECTS OF THE CARD (MESURES §209-210, §230): low-passes and light reverbs, both
 * Faust (src/audio/faust/), placed as in the web version — an FX track treats the SUM of its
 * group's instruments, and an FX of the MIX zone the sum of all the groups:
 *
 *   group 1's instruments ─ + ─ its FX, in track order ─┐
 *   group 2's instruments ─ + ─ its FX, in track order ─┼─ + ─ the MIX zone's FX ─ envelope ─ out
 *   instruments of a group without FX, of no group ─────┘
 *
 * Per cue slot (§212) a CHAIN of at most PER_SLOT effects. The cue says it (engine -2):
 *   fx=low-pass:<block>:<group>:<cutoff>:<q>,ambience:<block>:<group>:<wet>:<decay>:<damp>,…
 *   fxmix=<the MIX zone's group>
 * in the order the signal takes them; the group is the app's rank (1 for the first group, the
 * MIX zone after them — the numbering of the instruments' grp= and of the learnt-CC table, §221).
 * A cue without `fx` has no effect. Live: POST /api/audio/fx, the same two keys — the whole
 * chain of the cue that plays. A learnt CC (CcMap, §228) moves the effects of its kind in the
 * FX block's group: "lp_cutoff", "lp_q", "rv_wet", "rv_decay", "rv_damp".
 *
 * Writers (the cue, the route, a CC) post a chain; the audio task adopts it at the start of a
 * block. An effect that stays the same block keeps its state — removing a neighbour does not
 * cut its tail; another block in its place starts from silence.
 *
 * Memory: each effect's DSP in PSRAM, allocated the first time a chain needs it and kept — a
 * low-pass ~100 B, a reverb 18 KB; it costs the same there as in internal RAM (it fits in the
 * cache, §209). Measured on core 1: reverb 360-730 cycles/sample (7-15 % of the budget); the
 * low-pass 230-310 (5-6 %) — a state-variable filter since §229 (the biquad went unstable near
 * 20 Hz and killed the cue's sound). Four reverbs in a cue: up to 60 % of core 1 — the guard
 * (shed) drops the reverbs, then the low-passes, until the next cue. */
#include <Arduino.h>

namespace Effects {

/* ONE CHAIN PER CUE SLOT (§212): during a cross-fade the leaving cue keeps its effects while
 * the arriving cue has its own — AudioEngine::CUE_SLOTS of them. */
constexpr uint8_t SLOTS = 2;
/* AT MOST FOUR EFFECTS PER CUE (§230): a fifth is refused, and said. */
constexpr uint8_t PER_SLOT = 4;

// ── Any task: the cue, the route, the scripts ────────────────────────────────
/* The cue's chain in `slot`, from its parameter string (the keys above). Allocates the PSRAM
 * its effects need the first time; false + reason if one of them was refused (the others run). */
bool configureFromCue(uint8_t slot, const String& params, String& reason);
void off(uint8_t slot);                      // no effect in that slot
/* One parameter, live, in the slots of `slots` (a bit each), on the effects of its kind in
 * `group` (0: every group): "lp_cutoff" (Hz), "lp_q", "rv_wet" (0..1), "rv_decay" (s),
 * "rv_damp" (0..1) — a learnt CC moves the effects of the cue whose branch it came out of
 * (§212, §228). False: unknown name. */
bool setParam(const char* name, float value, uint8_t slots = 0xFF, uint8_t group = 0);
/* Is `name` one of setParam's? The CC table asks (§228): such a target is an effect's. */
bool isParam(const char* name);
/* Does that parameter move by octaves? The cutoff does — the app's slider is `log` (§228): a
 * CC spread linearly put the whole useful register in the first tenth of the knob. */
bool inOctaves(const char* name);
/* The chain of the cue that plays, live (POST /api/audio/fx): `fx` and `fxmix`, as a cue says them. */
bool applyKeys(uint8_t slot, const String& params, String& reason);
String status(uint8_t slot);                 // JSON for /api/audio/status and the route
uint32_t psramBytes();                       // the effects allocated, kept once allocated (§217)

// ── Audio task ───────────────────────────────────────────────────────────────
/* At the start of the slot's block: the chain a writer posted is adopted, whole. */
void beginBlock(uint8_t slot);
/* The groups (bit g) whose sum goes through effects of their own — the MIX zone's not among
 * them: their instruments are summed apart, then processGroup. */
uint32_t groupMask(uint8_t slot);
/* That group's effects, in their order, on its sum — in place. */
void processGroup(uint8_t slot, uint8_t group, float* l, float* r, size_t frames);
/* The MIX zone's effects on the slot's mix (every group summed) — in place. */
void processMix(uint8_t slot, float* l, float* r, size_t frames);
/* The slot is emptied (its Release over, or cut): what its effects still hold goes — the next
 * cue in that slot starts from silence. */
void clear(uint8_t slot);
/* The guard (AudioEngine): core 1 is over budget — drop the reverbs, then the low-passes,
 * until the next cue. Says so once. */
void shed();

}  // namespace Effects
