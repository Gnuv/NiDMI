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
/* 20 MHz: measured 990 KB/s against 680 at 10 MHz (§204) — and FOUR long stereo sounds at
 * once (4 lists) need 705 KB/s: 10 MHz cannot hold them (measured, §205: thousands of silent
 * blocks), 20 MHz holds them clean. A loose wire does not always hold 20 MHz: the driver then
 * HALVES the clock by itself (SdSpiDisk.h, `frequency`) and says so (/api/diag/sd `hz`,
 * `hz_fallbacks`). `measure(..., hz)` remounts the card at another frequency to try it. */
constexpr uint32_t    DEFAULT_FREQUENCY_HZ = 20000000;

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

/* THE CARD'S SUPERVISION — from the loop, cheap (it throttles itself to 250 ms): remounts a
 * card that is missing or was lost, declares lost a card whose reads fail in a row, reads
 * failed heads again, and ANNOUNCES each change of state (`NIDMI_SD:<state>`). Headless: no
 * app, no page needed. */
void service();
/* "off" (not declared), "ok" (mounted), "absent" (declared, never answered), "lost" (it was
 * mounted, its reads failed in a row, it has not come back). */
const char* state();

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
// BENCH: for `ms` milliseconds every read fails, as a card pulled out and put back would. An
// outage longer than the checks (~1 s) makes the card be declared lost, unmounted, then
// remounted; a shorter one is a glitch that passes. (/api/diag/sd?simulate_loss=1[&ms=...])
void   simulateLoss(uint32_t ms);
// BENCH: the next `reads` reads each have a corrupted block, read again at once — a noisy bus
// (/api/diag/sd?simulate_noise=N).
void   simulateNoise(uint32_t reads);

}  // namespace SdCard
