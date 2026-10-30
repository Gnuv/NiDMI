#include "Peaks.h"

namespace Peaks {

void begin(Result& r, uint32_t frames, uint32_t rate, uint8_t channels, bool exact) {
  memset(r.lo, 0, sizeof r.lo);
  memset(r.hi, 0, sizeof r.hi);
  r.peak = 0;
  r.leadFrames = UINT32_MAX;
  r.frames = frames; r.rate = rate; r.channels = channels ? channels : 1; r.exact = exact;
}

void feed(Result& r, const int16_t* pcm, uint32_t first, uint32_t n) {
  if (!r.frames) return;
  const uint8_t ch = r.channels;
  for (uint32_t k = 0; k < n; k++) {
    const uint32_t frame = first + k;
    if (frame >= r.frames) return;
    const uint16_t c = (uint16_t)((uint64_t)frame * COLUMNS / r.frames);
    for (uint8_t j = 0; j < ch; j++) {
      const int16_t s = pcm[k * ch + j];
      const int8_t  q = (int8_t)(s >> 8);
      if (q < r.lo[c]) r.lo[c] = q;
      if (q > r.hi[c]) r.hi[c] = q;
      const int16_t a = s == INT16_MIN ? INT16_MAX : (s < 0 ? -s : s);
      if (a > r.peak) r.peak = a;
      if (a > SILENCE && r.leadFrames == UINT32_MAX) r.leadFrames = frame;
    }
  }
}

String json(const Result& r, const char* name) {
  String o;
  o.reserve(2400);
  const uint32_t ms = r.rate ? (uint32_t)((uint64_t)r.frames * 1000ULL / r.rate) : 0;
  const long lead = r.leadFrames == UINT32_MAX ? -1L
                  : (long)(r.rate ? (uint64_t)r.leadFrames * 1000ULL / r.rate : 0);
  o += "{\"name\":\""; o += name; o += "\",\"duration_ms\":"; o += ms;
  o += ",\"channels\":"; o += r.channels; o += ",\"rate\":"; o += r.rate;
  o += ",\"peak\":"; o += r.peak; o += ",\"lead_ms\":"; o += lead;
  o += ",\"exact\":"; o += r.exact ? "true" : "false";
  o += ",\"lo\":[";
  for (uint16_t c = 0; c < COLUMNS; c++) { if (c) o += ','; o += r.lo[c]; }
  o += "],\"hi\":[";
  for (uint16_t c = 0; c < COLUMNS; c++) { if (c) o += ','; o += r.hi[c]; }
  o += "]}";
  return o;
}

}  // namespace Peaks
