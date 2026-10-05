#pragma once
// src/config/SdSpiDisk.h — OUR SD-CARD-OVER-SPI DRIVER, read-only.
//
// Why not Arduino's SD library: it demands that CMD8 return the 0x1AA echo and
// gives up otherwise (`goto unknown_card`), without saying why — the board has no
// serial port. The user's card initialises perfectly (CMD0, OCR, ACMD41: raw trace
// in §204) but answers CMD8 with `01 00 00 00 00`; a desktop reader accepts it,
// the library does not. Here CMD8 is a piece of information, not a condition, and
// every failure says at which step it happened.
//
// THE DRIVER GOES FURTHER THAN MOUNTING: it is what reads streamed sounds.
// Multi-block reads (CMD18) in one go, block CRC16 checked: counted, and REJECTED (read
// again) once the card has proven it sends valid ones — a corrupted transfer must never
// reach the audio; a card that sends no valid CRC at all is only counted. A failed block
// is read again. Nothing writes: FatFs sees a protected medium.
//
// It plugs into FatFs the way the library does (ff_diskio_register, then
// esp_vfs_fat_register): files are opened through Arduino's `File` API, under the
// mount point.
#include <Arduino.h>
#include <FS.h>
#include <SPI.h>

namespace SdSpiDisk {

/* Initialises the card (400 kHz), mounts it under `mountPoint` ("/sd") and moves to
 * `hz`. False: `reason` names the step that failed. To be called from ITS OWN task
 * (SdCard): initialisation waits for the card, up to 1.5 s. */
bool mount(SPIClass& spi, uint8_t cs, uint32_t hz, const char* mountPoint, String& reason);
void unmount();

bool        mounted();
uint64_t    capacityBytes();
const char* type();              // "SD", "SDHC/SDXC"
uint32_t    cmd8Echo();          // what CMD8 returned (0x1AA: a conforming card)

File open(const char* path);                     // read-only; empty when nothing is mounted

// Counters, since the mount.
uint32_t blocksRead();           // 512-byte blocks read
uint32_t crcErrors();            // CRC16 mismatches
uint32_t crcRejected();          // ... of which the block was read again (once the card has proven its CRCs)
uint32_t retries();              // blocks read again after a failure
/* The clock the bus runs at NOW: it falls back by itself (halved, 5 MHz at the lowest) when
 * reads must be done again in a burst — a loose wire at 20 MHz — and stays there. */
uint32_t frequency();
uint32_t fallbacks();            // how many times it fell back since the mount
/* Reads that failed after ALL their attempts: in a row (`failStreak`, back to 0 at the next
 * success) and since the mount. The first failure makes SdCard CHECK the card (probe): a
 * reader that fails stops reading, so no streak builds up by itself. */
uint32_t failStreak();
uint32_t failTotal();
/* Reads sector 0, with all the driver's attempts: does the card still answer? A success
 * puts `failStreak` back to 0. The supervisor (SdCard) asks it after any failed read, five
 * times a quarter of a second apart. */
bool     probe();
/* BENCH: for the next `ms` milliseconds every read fails on purpose, without touching the bus —
 * what a card pulled out then pushed back looks like to everything above
 * (/api/diag/sd?simulate_loss=1[&ms=...]). */
void simulateOutage(uint32_t ms);
/* BENCH: the next `reads` reads each have their first block corrupted (bad CRC), like a noisy
 * bus — each is read again and succeeds; a burst of them makes the bus clock fall back
 * (/api/diag/sd?simulate_noise=N). */
void simulateNoise(uint32_t reads);

}  // namespace SdSpiDisk
