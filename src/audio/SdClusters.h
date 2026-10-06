#pragma once
// src/audio/SdClusters.h — CLUSTER MAPS: STREAMS READ THE SD WITHOUT FatFs's SEEK.
//
// FatFs seeks by walking a file's cluster chain from its start, one FAT sector at a time: measured
// (MESURES §205) ~1.2 ms per MB of position — 25 ms at the start of a file, 67 ms at 35 MB, ~400 ms
// at 300 MB. A clip's head has to outlast that delay, so it set the head's length and made clips
// deep in a long file fail. Here the chain of each STREAMED sound is walked ONCE, when the sound
// arrives (SampleStore, in the SD-card task), and kept as a list of extents in PSRAM — a file
// written in one go is a single extent. A stream or a head then reads straight from the sectors
// (ClusterMapCore.h does the addressing): the delay no longer depends on where in the file the clip
// starts, and a failed read leaves nothing behind (a FatFs file object keeps its error forever).
//
// FALLBACK, always: a sound without a map (FAT12, exFAT, a fragmented file past the extent cap, a
// map that did not pass its check against FatFs) or with a stale one (the card was remounted: its
// generation changed) is read through FatFs, exactly as before. Maps are rebuilt after a remount.
#include <Arduino.h>

namespace SdClusters {

struct Map;                                           // opaque, reference-counted

enum class Status { Ok, Stale, IoError };

/* Walk the chain of /samples/<name>, check it against FatFs (three spans read both ways must
 * match), and keep it under `name` — replacing any earlier map. SD-card task. False: no map (the
 * reason is on the web console), the sound stays on the FatFs path. */
bool build(const char* name);
/* After a remount: every map again, from the names already known. SD-card task. */
void rebuildAll();

/* A counted reference to the map of `name`, or nullptr (none, or stale). Any task. `release` it. */
Map* acquire(const char* name);
void release(Map* map);

/* `n` bytes of the file at byte `offset`, straight from the sectors. Returns the bytes delivered
 * (fewer at the end of the file or on a failure; `status` says which). Stale: the card was mounted
 * again since the map was built — read through FatFs instead. */
size_t read(Map* map, uint32_t offset, uint8_t* dst, size_t n, Status& status);

/* A check on demand (/api/diag/sd?verify_map=<name>): `spans` random spans read through the map and
 * through FatFs, compared byte for byte. SD-card task. */
struct Verify { uint32_t spans; uint32_t bytes; uint32_t mismatches; bool ran; };
Verify verify(const char* name, uint32_t spans);

String diagnostics();                                 // JSON object

}  // namespace SdClusters
