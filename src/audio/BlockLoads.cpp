// src/audio/BlockLoads.cpp — what each block loads, live (MESURES §235). See the header.
#include "BlockLoads.h"
#include <esp_heap_caps.h>

namespace BlockLoads {
namespace {

constexpr uint8_t SIZE[2] = {4, 24};     // core 0: the synth voices; core 1: voices, kits, effects

/* One table per core. `block`, `cycles` and `seq` are written by the core's writer only, read by
 * the loop: 32-bit aligned, never torn. `cycles` adds up and wraps — the loop takes differences
 * (a second of a whole core is 240 M cycles: far from 2^32). */
struct Table {
  uint32_t          tick;                // the writer's calls, for the least recently used
  volatile uint32_t block[24];           // contiguous: the writer's scan
  volatile uint32_t cycles[24];
  volatile uint32_t seq[24];             // odd while the entry changes block
  uint32_t          used[24];            // the writer's tick at its last call
  uint32_t          seenSeq[24];         // the loop's: what it read last time
  uint32_t          seenCycles[24];
};

Table* tables = nullptr;                 // PSRAM: two
volatile bool on = false;

}  // namespace

void begin() {
  if (!tables) tables = (Table*)heap_caps_calloc(2, sizeof(Table), MALLOC_CAP_SPIRAM);
}

bool enabled() { return on; }

void setEnabled(bool want) {
  if (!tables || want == on) return;
  if (want)                              // the writers are idle: what they hold is the baseline
    for (uint8_t c = 0; c < 2; c++) {
      Table& t = tables[c];
      for (uint8_t i = 0; i < SIZE[c]; i++) { t.seenSeq[i] = t.seq[i]; t.seenCycles[i] = t.cycles[i]; }
    }
  __sync_synchronize();
  on = want;
}

void add(uint8_t core, uint32_t block, uint32_t cycles) {
  if (!on || !block || core > 1 || !tables) return;
  Table& t = tables[core];
  const uint8_t n = SIZE[core];
  const uint32_t now = ++t.tick;
  uint8_t oldest = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (t.block[i] == block) { t.cycles[i] = t.cycles[i] + cycles; t.used[i] = now; return; }
    if (now - t.used[i] > now - t.used[oldest]) oldest = i;   // a free entry (used 0) is the oldest
  }
  t.seq[oldest] = t.seq[oldest] + 1;     // odd: the loop leaves it for this time
  __sync_synchronize();
  t.block[oldest] = block;
  t.cycles[oldest] = cycles;
  t.used[oldest] = now;
  __sync_synchronize();
  t.seq[oldest] = t.seq[oldest] + 1;
}

uint8_t take(Load* out, uint8_t max, uint32_t elapsedUs) {
  if (!tables) return 0;
  const uint64_t budget = (uint64_t)elapsedUs * getCpuFrequencyMhz();
  uint8_t n = 0;
  for (uint8_t c = 0; c < 2; c++) {
    Table& t = tables[c];
    for (uint8_t i = 0; i < SIZE[c]; i++) {
      const uint32_t q = t.seq[i];
      __sync_synchronize();
      const uint32_t b = t.block[i], cy = t.cycles[i];
      __sync_synchronize();
      if ((q & 1) || q != t.seq[i]) continue;          // changing block: its whole count next time
      const uint32_t d = (q == t.seenSeq[i]) ? cy - t.seenCycles[i] : cy;
      t.seenSeq[i] = q;
      t.seenCycles[i] = cy;
      if (!b || !d || !budget || n >= max) continue;
      const uint64_t tenths = (uint64_t)d * 1000u / budget;
      out[n++] = {b, c, (uint16_t)(tenths > 1000 ? 1000 : tenths)};
    }
  }
  return n;
}

}  // namespace BlockLoads
