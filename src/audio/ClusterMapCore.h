#pragma once
// src/audio/ClusterMapCore.h — A FAT FILE'S CLUSTER CHAIN AS EXTENTS, AND BYTE -> SECTOR ADDRESSING.
//
// Why: FatFs seeks by walking the file's cluster chain from its start (the firmware's FatFs is
// built without "fast seek"), one FAT-sector read at a time. Measured (MESURES §205): the further
// into a file a clip starts, the longer its stream takes to deliver its first data — about 1.2 ms
// per MB, 25 ms at the start of the file, 67 ms at 35 MB, and extrapolated ~400 ms at 300 MB.
// That delay is what the clip's HEAD has to cover. Here the chain is walked ONCE, when the sound
// arrives, and kept as a short list of EXTENTS (a file written in one go is a single extent); any
// byte of the file is then a division away from its sector.
//
// PURE C++: no Arduino, no FatFs, no allocation — the hardware (reading a FAT sector, reading data
// sectors, growing the extent list) comes in through callbacks. That is what lets it be tested on
// a computer against a synthetic, fragmented FAT image (hardware/bench/sd/cluster_map_test.cpp).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ClusterMapCore {

constexpr uint32_t SECTOR = 512;

/* A run of consecutive clusters: `count` clusters from physical cluster `start`, holding the file's
 * logical clusters `logical` .. `logical + count - 1`. */
struct Extent { uint32_t logical; uint32_t start; uint32_t count; };

struct Geometry {
  uint8_t  fatBits = 0;          // 16 or 32
  uint32_t fatBase = 0;          // first sector of the FAT
  uint32_t dataBase = 0;         // sector of cluster 2
  uint32_t clusterSectors = 0;   // sectors per cluster
  uint32_t nEntries = 0;         // FAT entries: clusters + 2
};

enum class Walk { Ok, BadGeometry, BadChain, IoError, TooManyExtents };

inline uint32_t _le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint32_t _le16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* THE WALK. `readFatSector(sector, dst512)` -> bool; `push(extent)` -> bool (false: no room).
 * A file of `fileBytes` bytes holds ceil(fileBytes / clusterBytes) clusters, starting at `first`;
 * every link must stay inside the FAT, and a chain that ends early is refused. The FAT is read one
 * sector at a time, and each sector once while the chain climbs. */
template <class ReadFatSector, class Push>
Walk walk(const Geometry& g, uint32_t first, uint32_t fileBytes, ReadFatSector readFatSector, Push push,
          uint32_t* clustersOut = nullptr) {
  if ((g.fatBits != 16 && g.fatBits != 32) || g.clusterSectors == 0) return Walk::BadGeometry;
  const uint64_t clusterBytes = (uint64_t)g.clusterSectors * SECTOR;
  if (fileBytes == 0 || first < 2 || first >= g.nEntries) return Walk::BadChain;
  const uint64_t need64 = ((uint64_t)fileBytes + clusterBytes - 1) / clusterBytes;
  if (need64 > g.nEntries) return Walk::BadChain;
  const uint32_t need = (uint32_t)need64;

  uint8_t  win[SECTOR];
  uint32_t winSector = 0xFFFFFFFFu;
  bool     winValid = false;
  Walk     error = Walk::Ok;
  auto entry = [&](uint32_t c) -> uint32_t {
    const uint32_t bytePos = (g.fatBits == 32) ? c * 4u : c * 2u;
    const uint32_t sector = g.fatBase + bytePos / SECTOR;
    if (!winValid || winSector != sector) {
      if (!readFatSector(sector, win)) { error = Walk::IoError; return 0; }
      winSector = sector; winValid = true;
    }
    const uint8_t* p = win + (bytePos % SECTOR);
    return (g.fatBits == 32) ? (_le32(p) & 0x0FFFFFFFu) : _le16(p);
  };

  Extent run = { 0, first, 1 };
  uint32_t c = first;
  for (uint32_t idx = 1; idx < need; idx++) {
    const uint32_t next = entry(c);
    if (error != Walk::Ok) return error;
    if (next < 2 || next >= g.nEntries) return Walk::BadChain;       // free, reserved, bad or end of chain: too early
    if (next == c + 1) run.count++;
    else {
      if (!push(run)) return Walk::TooManyExtents;
      run.logical = idx; run.start = next; run.count = 1;
    }
    c = next;
  }
  if (!push(run)) return Walk::TooManyExtents;
  if (clustersOut) *clustersOut = need;
  return Walk::Ok;
}

/* THE ADDRESSING. A view over the extents of one file. */
struct View {
  const Geometry* g;
  const Extent*   ext;
  uint32_t        nExt;
  uint32_t        size;          // the file's size in bytes
};

struct Loc {
  uint32_t sector;               // the absolute sector holding the byte
  uint32_t inSector;             // its offset in that sector
  uint32_t contig;               // bytes from it to the end of its run of consecutive clusters (file end included)
};

inline bool locate(const View& v, uint32_t offset, Loc& out) {
  if (offset >= v.size || v.nExt == 0) return false;
  const uint64_t clusterBytes = (uint64_t)v.g->clusterSectors * SECTOR;
  const uint32_t ci = (uint32_t)(offset / clusterBytes);
  uint32_t lo = 0, hi = v.nExt;                      // last extent whose first logical cluster is <= ci
  while (lo + 1 < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (v.ext[mid].logical <= ci) lo = mid; else hi = mid;
  }
  const Extent& e = v.ext[lo];
  if (ci < e.logical || ci >= e.logical + e.count) return false;
  const uint64_t inRun = (uint64_t)(ci - e.logical) * clusterBytes + (offset % clusterBytes);
  out.sector   = v.g->dataBase + (e.start - 2) * v.g->clusterSectors + (uint32_t)(inRun / SECTOR);
  out.inSector = (uint32_t)(inRun % SECTOR);
  const uint64_t toRunEnd = (uint64_t)e.count * clusterBytes - inRun;
  const uint64_t toFileEnd = (uint64_t)v.size - offset;
  out.contig = (uint32_t)(toRunEnd < toFileEnd ? toRunEnd : toFileEnd);
  return true;
}

/* READ `n` BYTES at `offset`. `readSectors(sector, count, dst)` -> bool reads whole sectors straight
 * into `dst`; the first and last partial sectors go through `bounce` (SECTOR bytes). Returns the bytes
 * delivered — fewer than asked at the end of the file or when a read failed. `maxSectors` bounds one
 * call (the bus is held for its duration). */
template <class ReadSectors>
size_t readBytes(const View& v, uint32_t offset, uint8_t* dst, size_t n, ReadSectors readSectors,
                 uint8_t* bounce, uint32_t maxSectors = 64) {
  if (offset >= v.size) return 0;
  if (n > (size_t)(v.size - offset)) n = (size_t)(v.size - offset);
  size_t done = 0;
  while (done < n) {
    Loc loc;
    if (!locate(v, offset + (uint32_t)done, loc)) break;
    size_t span = n - done;
    if (span > loc.contig) span = loc.contig;
    uint32_t sector = loc.sector;
    size_t got = 0;
    if (loc.inSector) {                              // a partial first sector
      if (!readSectors(sector, 1, bounce)) return done;
      size_t take = SECTOR - loc.inSector;
      if (take > span) take = span;
      memcpy(dst + done, bounce + loc.inSector, take);
      got += take; sector++;
    }
    uint32_t full = (uint32_t)((span - got) / SECTOR);
    while (full) {                                   // whole sectors, straight into the destination
      const uint32_t k = full > maxSectors ? maxSectors : full;
      if (!readSectors(sector, k, dst + done + got)) return done + got;
      got += (size_t)k * SECTOR; sector += k; full -= k;
    }
    if (span - got) {                                // a partial last sector
      if (!readSectors(sector, 1, bounce)) return done + got;
      memcpy(dst + done + got, bounce, span - got);
      got = span;
    }
    done += got;
  }
  return done;
}

}  // namespace ClusterMapCore
