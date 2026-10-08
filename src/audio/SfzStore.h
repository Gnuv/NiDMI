// SfzStore — the SFZ player of the card: instruments from the SD, in PSRAM,
// swapped per cue (PROMPT_SESSION_SFZ, step 3; decisions of 6 October).
//
// AN INSTRUMENT IS A CUE'S GUEST: `sfz=kit-test/kit.sfz` in a cue line loads
// it (instruments live in PSRAM, samples int16 — the kernel's contract), and
// it is UNLOADED when no cue of the list names it anymore — several .sfz can
// live in one composition, at different times. Samples are deduplicated by
// path across instruments (two kits sharing a WAV load it once), bounded by
// BUDGET_PSRAM; refusals carry a message (missing file, bad WAV, budget…).
//
// THE ENGINE IS THE AUDIO TASK'S. sfz16_engine (~3 ko, voices inside) is a
// static here — internal RAM, never touched by another task. The loader
// (loopTask: the cue, the living route) PROPOSES an instrument; the audio
// task ADOPTS it at the start of a block (the play-list bank pattern, §196):
// sounding voices fast-release against the OLD instrument, which stays alive
// until they drained (a few ms) — freeing waits RETRAIT_MARGIN_MS after the
// swap, on the loader's side.
//
// The kernel is sfz16.{h,c} next to this file (MESURES §207: 96 host checks,
// parity 0.35-0.61 LSB against the reference core). NOT WIRED YET: MIDI CC
// (locc/hicc gating needs a CC path to the audio task — the event queue
// carries notes only); the first test kit uses none.
#pragma once
#include <Arduino.h>

namespace SfzStore {

// Instruments and their samples live under this folder of the SD card.
constexpr const char* DOSSIER = "/sfz";
// int16 PCM kept in PSRAM, all instruments together. 8.25 Mo free at boot,
// but the SD heads (2 Mo) and the web buffers live there too: 4 Mo is the
// milestone-1 ceiling (PROMPT_SESSION_SFZ), refusals beyond it.
constexpr size_t BUDGET_PSRAM = 4 * 1024 * 1024;
// Instruments KEPT loaded (loaded on the cue that names them, unloaded when
// no cue does): four kits at hand covers alternating cues without thrashing.
constexpr int SIMULTANES = 4;

// ── Loader side (loopTask: the cue, POST /api/audio/sfz) ───────────────────
/* Make `chemin` (relative to /sfz, e.g. "kit-test/kit.sfz") the playing
 * instrument — loaded now if needed (SD reads in chunks, ~0.4 s for a kit).
 * Empty chemin: the SFZ goes silent (its notes stop). False with `raison`
 * on every refusal: nothing is truncated in silence. */
bool proposer(const char* chemin, String& raison);
/* Housekeeping, called after each cue apply: `employesCsv` is the comma list
 * of the sfz= paths the WHOLE cue list still names. Retires the others
 * (freed RETRAIT_MARGIN_MS later, once drained) and frees the expired. */
void entretenir(const String& employesCsv);

// ── Audio task only ─────────────────────────────────────────────────────────
void adopter();                       // block start: swap the proposed instrument
bool notesArmees();                   // the current cue routes notes here
void noteOn(uint8_t note, uint8_t velo);
void noteOff(uint8_t note);
/* Renders the block into g/d (cleared first, float — the caller converts at
 * its int16 bus). False when nothing plays; buffers untouched then. */
bool rendreBloc(float* g, float* d, uint32_t trames);

// ── Status (/api/audio/status, refusals for the app) ───────────────────────
/* What the card's own FatFs sees in a folder of the SD (the Fichiers panel
 * walks /samples only): names, sizes, dir flags — the diagnostic of a /sfz
 * that will not open (§208). Read-only. */
String  lister(const char* dossier);
bool    charge();                     // an instrument plays (or is proposed)
String  chemin();                     // "" when silent
int     regions();
int     samples();
size_t  octets();                     // int16 PCM held, all instruments
int     voixActives();
uint32_t generation();                // bumps on every instrument change
uint32_t notesRecues();               // diagnostic: notes that reached the engine (§208)
bool    moteurInstValide();          // diagnostic: the engine's instrument pointer (§208)
String  regionPremiere();            // diagnostic: the parsed first region, as the card sees it (§208)
void blocRendu();  void evenementVu();     // the audio loop's heartbeat, and popped events (§208)
uint32_t blocsRendus(); uint32_t evenementsVus();
String  erreur();                     // the last refusal, "" if none

}  // namespace SfzStore
