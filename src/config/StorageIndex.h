#pragma once
// src/config/StorageIndex.h — WHAT storage HOLDS, KEPT IN PSRAM (MESURES §241).
//
// Walking storage costs 25 to 45 ms per entry: hundreds of 128-byte reads, each
// parking core 0. Done in the web server, it froze every other request — the
// transport included — for seconds: 1.1 s for the script list and 1.5 s for the
// sound list at each connection of the app, 9 s for the file list. And past 5 s
// AsyncTCP's watchdog restarted the board (§240).
//
// So storage is walked ONCE, after it is mounted, by a background task — before the
// app can connect — and the three modules that write to it (Differe, Repertoire,
// SampleStore) tell the index what they changed, right after the flash said yes.
// Routes read the index: no flash read. The block count (what LittleFS really
// uses) is measured with the walk, and again at silence after a write.
//
// Paths are those of the LittleFS API ("/compositions/04/x.nms"), without the
// mount point. Any task may call; never from an interrupt.
#include <Arduino.h>

namespace StorageIndex {

/** Starts the background walk. Called once storage is mounted; again does nothing. */
void begin();

/** THE STORAGE TASK RUNS THE FLASH BURSTS, ON CORE 0 (MESURES §242). A burst of flash reads
 *  started on core 1 — the audio's core — starved the audio: a composition reopened by the
 *  web server (1 792 reads), the audio woken but not elected for ~83 ms, one reopening in
 *  three or four; at boot, the loop reading the first cue's scripts (1 416 reads). Started on
 *  core 0, core 1 only waits in ipc1 for each read: 0 late blocks in 30 reopenings. `fn` runs
 *  in this task, after what was handed over before; `wait`: returns once it is done. Handed
 *  over before begin(), it runs first, before the walk — unless the caller waits: then here.
 *  Called from a job, it runs at once. */
void run(void (*fn)(), bool wait);

/** True once the first walk is done. */
bool ready();
/** Waits at most `ms` for the first walk (a request right after a boot). */
bool waitReady(uint32_t ms);

/** A file the flash now holds, at this size (its folders are added if missing). */
void fileWritten(const char* path, size_t size);
/** A folder the flash now holds. */
void dirMade(const char* path);
/** A file, or a folder and everything under it, the flash no longer holds. */
void removed(const char* path);
/** A file or a folder (and everything under it) moved. */
void renamed(const char* from, const char* to);

/** Every entry under `folder` ("/" for all), in walk order; `recursive` false = its
 *  direct children only. Under the index's lock: `visit` must not do I/O. */
void visit(const char* folder, bool recursive,
           void (*visit)(const char* path, size_t size, bool dir, void* ctx), void* ctx);

/** Whether `path` is a file the index holds, and its size. */
bool fileSize(const char* path, size_t& size);
/** Whether `path` is a folder the index holds. */
bool isDir(const char* path);

struct Stats {
  size_t files = 0, scripts = 0, content = 0;   // from the index
  size_t used = 0, total = 0;                    // LittleFS's blocks, from the last measure
  bool measured = false;                         // false until the first walk is done
};
Stats stats();

}  // namespace StorageIndex
