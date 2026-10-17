#pragma once
/* THE DSP BENCH — measurement images only (NIDMI_DSP_BENCH=1).
 *
 * The question it answers (October 2026, a conference demo on ONE card): can
 * Plaits, an SFZ kit, sounds, a low-pass and a light reverb all run on the same
 * card as the web interface? The CPU of Plaits and of the SFZ player is known,
 * each alone (MESURES §11, §208); what made synthesis leave the single card was
 * MEMORY (§122-127: Plaits in internal RAM, the page no longer reloaded).
 *
 * The bench runs, ON THE AUDIO TASK and after the block the engine rendered
 * (samples, SFZ, sine), up to two Plaits voices of its own — their state in
 * internal RAM or PSRAM, rendered on core 1 (the audio task) or on core 0 (a
 * worker task, one block ahead) — and the Faust effects of src/audio/faust/
 * (a low-pass; the June freeverb or a light reverb), and times each stage in
 * CPU cycles. "audible" mixes the result into what goes to the DAC; otherwise
 * it is computed and dropped.
 *
 * Its own guard: when core 1 spends more than a block's budget on 20 blocks in
 * a row, the bench stops itself (`stopped`: "overload") — an overloaded audio
 * task never yields, and the task watchdog restarted the card (MESURES §209).
 *
 * Not a feature: the production image does not carry it (and so carries no
 * Plaits code at all, §127.4). */
#include <Arduino.h>

#if NIDMI_DSP_BENCH

namespace DspBench {

enum Reverb : uint8_t { REVERB_NONE = 0, REVERB_FREEVERB = 1, REVERB_AMBIENCE = 2 };

struct Config {
  uint8_t  plaitsVoices = 0;          // 0..2
  uint8_t  engines[2]   = {8, 16};    // Plaits engine per voice (0..23)
  bool     plaitsPsram  = true;       // Voice + its scratch pool in PSRAM (else internal RAM)
  uint8_t  plaitsCore   = 1;          // 1: in the audio task; 0: a worker task on core 0, one block ahead
  uint8_t  workerPriority = 17;       // the core 0 worker: under MIDI (19) and TCP/IP (18)
  bool     lowpass      = false;
  uint8_t  reverb       = REVERB_NONE;
  bool     reverbPsram  = true;       // the freeverb is ~150 KB: internal RAM cannot hold it
  bool     audible      = false;
  uint16_t notePeriodMs = 400;        // a new note per voice, this often
  float    plaitsGain   = 0.5f;
  float    cutoff = 1200.0f, q = 2.0f;                        // low-pass
  float    wet = 0.3333f, room = 0.5f, damp = 0.5f;           // freeverb (the June values)
  float    decay = 1.2f, adamp = 0.35f, awet = 0.25f;         // the light reverb
};

/* The audio task, once per block, after the engine's render and before the
 * volume and the silence gate: `renderCycles` is what that render cost. */
void process(int16_t* interleaved, size_t frames, uint32_t renderCycles);

/* The web server: stop the bench, wait for the audio task and the worker to
 * leave it, free, allocate, start again. False (and nothing running) when an
 * allocation fails. */
bool configure(const Config& c, String& reason);

/* JSON: the configuration, the memory, and per stage the cycles per sample,
 * average and worst block, since the last reset. */
String report();
void resetCounters();

}  // namespace DspBench

#endif
