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
// Multi-block reads (CMD18) in one go, block CRC16 checked (counted, not
// rejected: a card that sends no valid CRC shows in the counters), a failed block
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
uint32_t crcErrors();            // CRC16 mismatches (the data is kept anyway)
uint32_t retries();              // blocks read again after a failure

}  // namespace SdSpiDisk
