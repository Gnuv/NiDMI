#pragma once
// src/diag/HeapTrough.h — WHERE THE INTERNAL HEAP GOES AT BOOT (MESURES §243).
//
// At boot, with the app's page served, the internal heap fell to 2 to 4 KB free (§242) —
// 60 KB at rest a minute later. Nothing said when, nor what held it. Armed for the next
// boots (RTC memory, like the autopsy), a sampler on core 0 reads the free internal heap
// every 2 ms for the first 60 s: the lowest value of each 50 ms, and at the lowest point,
// a photo of the heap (every block, and which task's stack or TCB it is) — and another at
// the end, to compare. Marks say what happened when (the page served, the cable up...).
// Bench only: off by default, it costs a 3 KB stack and a few ms per photo.
#include <Arduino.h>

namespace HeapTrough {

/** Armed: on at every software restart until disarmed. A power cut disarms it. */
void arm(bool armed);
bool armed();

/** Starts the sampler if armed. Called early in nidmi_begin(). */
void begin();

/** A moment worth knowing, in the timeline (a short literal: kept by pointer). Any task. */
void mark(const char* what);

/** The timeline, the marks and the two photos, as JSON (built in PSRAM). Empty if never armed. */
bool json(char*& text, size_t& len);

}  // namespace HeapTrough
