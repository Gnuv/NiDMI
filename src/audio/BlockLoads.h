#pragma once
/* WHAT EACH BLOCK LOADS, LIVE (MESURES §235). The app shows, on each block of the grid, the share
 * of its core the block took over the last second — a sample voice's sound, a play list, a kit, a
 * Plaits voice, an effect — beside the two cores' load of the menu bar (NIDMI_CPU).
 *
 * The cycles are the ones ProcessCosts already counts (§231): no clock read is added. They are
 * summed by block id (the composition's block, the head of its chain: the `bloc=`, `lbloc=`,
 * `sfzbloc=`, `synthbloc=` of the cue line, an effect's block of `fx=`), each core in its own
 * table, ONE writer each — the audio task on core 1, the synth task on core 0 —, read by the loop
 * once a second. Measured only while a tab follows the load (CPU_LOAD:1): otherwise a writer
 * returns at its first instruction.
 *
 * The tables are in PSRAM (two, ~1 KB); a block takes an entry the first time it plays, the one
 * least recently used if the table is full. An entry is claimed under a sequence number (odd while
 * it changes): the loop never mixes two blocks' counts. */
#include <Arduino.h>

namespace BlockLoads {

constexpr uint8_t MAX = 28;              // both tables: 24 entries on core 1, 4 on core 0

struct Load {
  uint32_t block;
  uint8_t  core;
  uint16_t tenths;                       // tenths of a percent of the core, 0 when under 0.1 %
};

/* At start: the tables. Once. */
void begin();

// ── Loop ─────────────────────────────────────────────────────────────────────
/* Measured only while a tab follows the load: the loop says so once a second. Switched on, the
 * counts start from now. */
void setEnabled(bool on);
bool enabled();
/* Each block that played since the last call, and its share of its core over `elapsedUs`. */
uint8_t take(Load* out, uint8_t max, uint32_t elapsedUs);

// ── Audio task (core 1), synth task (core 0) ─────────────────────────────────
/* `block` took `cycles` of `core` (0 or 1). Each core's only writer calls it; block 0 is none. */
void add(uint8_t core, uint32_t block, uint32_t cycles);

}  // namespace BlockLoads
