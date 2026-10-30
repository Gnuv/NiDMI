#pragma once
/* THE WAVEFORM OF A SOUND (MESURES §253): what the file browser draws — a min/max per column,
 * the peak, and the silence at its head (it tells a sound that is empty, or that starts late).
 * Fed in increasing frame order: from the PCM in PSRAM at once (the web server), or chunk by
 * chunk from the SD by the SD card's task — densely, or by one window per column for a long
 * sound (`exact` false: the peak and the head silence are then those of what was read). */
#include <Arduino.h>

namespace Peaks {

constexpr uint16_t COLUMNS = 240;
constexpr int16_t  SILENCE = 33;           // -60 dBFS: below, a frame counts as silence

struct Result {
  int8_t   lo[COLUMNS], hi[COLUMNS];       // per column, the sample >> 8 (-128..127)
  int16_t  peak;                           // the largest |sample| read
  uint32_t leadFrames;                     // frames before the first one above SILENCE; UINT32_MAX: none
  uint32_t frames, rate;
  uint8_t  channels;
  bool     exact;                          // every frame was read
};

void   begin(Result& r, uint32_t frames, uint32_t rate, uint8_t channels, bool exact);
/* `n` interleaved frames starting at frame `first`. */
void   feed(Result& r, const int16_t* pcm, uint32_t first, uint32_t n);
String json(const Result& r, const char* name);

}  // namespace Peaks
