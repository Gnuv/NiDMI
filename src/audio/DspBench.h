#pragma once
/* THE DSP BENCH — measurement images only (NIDMI_DSP_BENCH=1).
 *
 * The question it answers (October 2026, a conference demo on ONE card): can
 * Plaits, an SFZ kit, sounds, a low-pass and a light reverb all run on the same
 * card as the web interface? The CPU of Plaits and of the SFZ player is known,
 * each alone (MESURES §11, §208); what made synthesis leave the single card was
 * MEMORY (§122-127: Plaits in internal RAM, the page no longer reloaded), and
 * Plaits from PSRAM was never measured. The bench runs, ON THE AUDIO TASK and
 * after the block the engine rendered (samples, SFZ, sine), up to two Plaits
 * voices of its own (internal RAM or PSRAM) and the two Faust effects of
 * src/audio/faust/, and times each stage in CPU cycles. "audible" mixes the
 * result into what goes to the DAC; otherwise it is computed and dropped.
 *
 * Not a feature: the production image does not carry it (and so carries no
 * Plaits code at all, §127.4). */
#include <Arduino.h>

#if NIDMI_DSP_BENCH

namespace DspBench {

struct Config {
  uint8_t  plaitsVoices = 0;          // 0..2
  uint8_t  engines[2]   = {8, 16};    // Plaits engine per voice (0..23)
  bool     plaitsPsram  = false;      // Voice + its scratch pool in PSRAM (else internal RAM)
  bool     lowpass      = false;
  bool     reverb       = false;
  bool     reverbPsram  = true;       // the freeverb is ~150 KB: internal RAM cannot hold it
  bool     audible      = false;
  uint16_t notePeriodMs = 400;        // a new note per voice, this often
  float    plaitsGain   = 0.5f;
  float    cutoff = 1200.0f, q = 2.0f;                        // low-pass
  float    wet = 0.3333f, room = 0.5f, damp = 0.5f;           // freeverb (the June values)
};

/* The audio task, once per block, after the engine's render and before the
 * volume and the silence gate: `renderCycles` is what that render cost. */
void process(int16_t* interleaved, size_t frames, uint32_t renderCycles);

/* The web server: stop the bench, wait for the audio task to leave it, free,
 * allocate, start again. False (and nothing running) when an allocation fails. */
bool configure(const Config& c, String& reason);

/* JSON: the configuration, the memory, and per stage the cycles per sample,
 * average and worst block, since the last reset. */
String report();
void resetCounters();

}  // namespace DspBench

#endif
