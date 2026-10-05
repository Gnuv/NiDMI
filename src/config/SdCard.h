#pragma once
// src/config/SdCard.h — THE SD CARD, declared as a component (`sd_spi`).
//
// A file SUPPORT next to `storage` (Stockage.h): the same layout (`/samples` for
// sounds), another volume — `sd` — that /api/fichiers reports with its state.
// What the firmware does with it, for now:
//
//   - it MOUNTS it when the component is declared (at boot, or as soon as the
//     user declares it);
//   - it reads its sounds — `/samples/*.wav`, 16-bit PCM — into PSRAM like those
//     of storage: same store (SampleStore), same names in the cues. A name that
//     storage already holds wins. Sounds too long for PSRAM are STREAMED instead
//     (SdStream.h);
//   - that is ALL: read-only. No upload, no delete, no composition or
//     .instrument on the card (CONVERGENCE §9.7, later); the card is filled from
//     a computer.
//
// ITS OWN TASK, never the loop nor the web server. Mounting a card means SPI
// exchanges that can take a while (an absent card makes you wait); reading a
// multi-MB sound takes seconds. Neither the loop (cues, RTP-MIDI, OSC), nor
// async_tcp (5 s watchdog), nor the audio may wait for it: the task has priority
// 1, runs on core 0, is born and dies on demand (no stack kept in internal RAM)
// and yields the CPU after every chunk it reads.
//
// Removing the component unmounts the card but does NOT unload the sounds
// already read: they stay playable until the next boot (SampleStore only gives
// PSRAM back one sound at a time, when a storage file is deleted — it has no
// "unload a volume").
#include <Arduino.h>
#include <FS.h>

namespace SdCard {

constexpr const char* VOLUME_ID = "sd";          // the id /api/fichiers gives it
constexpr const char* FOLDER    = "/samples";    // same layout as storage
/* 10 MHz: a breadboard with jumper wires does not always hold 20 MHz.
 * `measure(..., hz)` remounts the card at another frequency to try it. */
constexpr uint32_t    DEFAULT_FREQUENCY_HZ = 10000000;

// The component is declared: its pins are known, the card is to be mounted.
// Idempotent (boot-time restoration, then an identical re-declaration).
void declare(uint8_t cs, uint8_t sck, uint8_t miso, uint8_t mosi);
// The component is removed: the card gets unmounted.
void undeclare();

bool     declared();
bool     mounted();         // the card answered and its file system is mounted
uint64_t capacityBytes();   // its capacity in bytes, 0 when not mounted

/* A user GESTURE may have changed things (they just plugged the card in): a new
 * attempt, at most one every 5 s, in the task. Called by the file list when the
 * component is declared but the card is absent. */
void retryIfDue();

/* SampleStore has finished reading storage: the card's sounds may follow. */
void loadSounds();
/* HEADS of streamed clips were requested (SdStream.h): the task reads them, one by one. */
void loadHeads();

// Opens a card file for reading; an empty File when the card is not mounted.
File open(const char* path);

/* DIAGNOSTICS — a board without a serial port cannot say why a card won't mount.
 * The `[SD]` messages go to the web console (NIDMI_WEB_LOG); and when mounting
 * fails the task PROBES the bus by hand: CMD0 then CMD8 at 400 kHz. A live card
 * answers 0x01 to CMD0; 0xFF is silence (card absent, MISO, power or CS badly
 * wired). This is what /api/diag/sd returns.
 *   retry   : forces a new attempt right now (no 5 s wait).
 *   wiring  : probes the wiring (MISO at rest, MISO/MOSI swapped) when it won't mount.
 *   measure : the name of a .wav in /samples whose sequential read is timed
 *             (throughput, worst latency of a 16 KB chunk, a seek to the middle) —
 *             what sizes the streamed reading. `hz`: the bus frequency.
 * The measurement runs in the task; the request does not wait: read the
 * diagnostics again until `measure.state == "finished"`. */
String diagnostics();
void   tryMount();
// The wiring probe (MISO level, swapped MISO/MOSI) — card NOT mounted only.
void   probeWiring();
bool   measure(const char* name, uint32_t hz);

}  // namespace SdCard
