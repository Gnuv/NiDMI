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
// THREE TASKS, EACH ONE ALONE ON ITS PART (the rule of the SD, SdCard.h: neither
// the loop — cues, RTP-MIDI, OSC — nor async_tcp may wait for the card):
//   - ANY task (the cue on loopTask, the living route on async_tcp) DEPOSITS a
//     wish and returns at once: proposer(), entretenir(). They read nothing;
//   - the sd-card task SERVES it (servir(), called by SdCard): the only one
//     that reads the SD, loads, retires and frees — the only writer of the
//     pool. A kit takes about a second per MB; the old instrument keeps the
//     keyboard meanwhile, and a request superseded while loading is not
//     adopted (the newest wish wins; what was loaded stays as a cache);
//   - the AUDIO task adopts at the start of a block (the play-list bank
//     pattern, §196): sounding voices fast-release against the OLD instrument,
//     which stays alive until they drained — freeing waits RETRAIT_MARGIN_MS.
//
// The kernel is sfz16.{h,c} next to this file (MESURES §207: host-tested,
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
// milestone-1 ceiling (PROMPT_SESSION_SFZ), refusals beyond it. The
// instrument being REPLACED stays loaded until the new one is adopted: both
// must fit.
constexpr size_t BUDGET_PSRAM = 4 * 1024 * 1024;
// Instruments KEPT loaded (loaded on the cue that names them, unloaded when
// no cue does): four kits at hand covers alternating cues without thrashing.
constexpr int SIMULTANES = 4;
// A path (relative to /sfz) longer than this is refused, never cut.
constexpr int CHEMIN_MAX = 96;

// ── Any task: deposit, never wait ───────────────────────────────────────────
/* Make `chemin` (relative to /sfz, e.g. "kit-test/kit.sfz") the playing
 * instrument. RETURNS AT ONCE: the sd-card task loads it if needed (see
 * chargement(), the announcement NIDMI_SFZ:charge|pret|refus:<chemin>, and
 * erreur() for a refusal), the audio task adopts it. Empty chemin: the SFZ
 * goes silent now (its notes stop). False with `raison` only for a request
 * that cannot even be tried (path too long or climbing out of /sfz). */
bool proposer(const char* chemin, String& raison);
/* Housekeeping, called after each cue apply: `employesCsv` is the comma list
 * of the sfz= paths the WHOLE cue list still names. The sd-card task retires
 * the others (freed RETRAIT_MARGIN_MS later, once drained). */
void entretenir(const String& employesCsv);

// ── The sd-card task only (SdCard calls it for its `_wantSfz` flag) ─────────
void servir();

// ── Audio task only ─────────────────────────────────────────────────────────
void adopter();                       // block start: swap the proposed instrument
bool notesArmees();                   // the current cue routes notes here
void noteOn(uint8_t note, uint8_t velo);
void noteOff(uint8_t note);
/* Renders the block into g/d (cleared first, float — the caller converts at
 * its int16 bus). False when nothing plays; buffers untouched then. */
bool rendreBloc(float* g, float* d, uint32_t trames);

// ── Status (/api/audio/status, refusals for the app) — any task ─────────────
/* What the card's own FatFs sees in a folder of the SD (the Fichiers panel
 * walks /samples only): names, sizes, dir flags — the diagnostic of a /sfz
 * that will not open (§208). Read-only, for the web: never waits on a card in
 * doubt. */
String  lister(const char* dossier);
bool    charge();                     // an instrument plays (or is proposed)
bool    chargement();                 // a wish is not served yet (loading, or waiting for the card)
int     enMemoire();                  // instruments held in PSRAM (loaded, retiring included)
String  demande();                    // the path asked for last ("" = silence)
String  chemin();                     // the playing instrument, "" when silent
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
String  echapper(const String& s);   // a text made safe inside a JSON string
String  diagnostic();                 // the pool as JSON: held instruments, PCM bytes, last load time

}  // namespace SfzStore
