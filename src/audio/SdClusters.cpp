#include "SdClusters.h"

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include "ClusterMapCore.h"
#include "SampleStore.h"
#include "../config/SdCard.h"
#include "../config/SdSpiDisk.h"
#include "../server/WebDebugConsole.h"

namespace SdClusters {
namespace {

using namespace ClusterMapCore;

/* A fragmented file past this many extents gets no map and is read through FatFs: 8 192 extents is
 * 96 KB of PSRAM, and a card written once is a handful. */
constexpr uint32_t MAX_EXTENTS = 8192;
constexpr uint8_t  ENTRIES = SAMPLES_MAX;          // one per sound the store can hold

}  // namespace

struct Map {
  Geometry g;
  Extent*  ext = nullptr;
  uint32_t nExt = 0, size = 0, gen = 0, clusters = 0;
  int      refs = 1;                               // the table's own reference
};

namespace {

struct Entry { char name[SampleStore::NOM_MAX]; Map* map; };
Entry* _table = nullptr;                           // in PSRAM, taken at the first map

StaticSemaphore_t _lockBuffer;
SemaphoreHandle_t _lock = xSemaphoreCreateMutexStatic(&_lockBuffer);
struct Lock {
  Lock()  { if (_lock) xSemaphoreTake(_lock, portMAX_DELAY); }
  ~Lock() { if (_lock) xSemaphoreGive(_lock); }
};

/* Counters: written by several tasks (the readers, the SD-card task), so atomic adds. */
volatile uint32_t _built = 0, _rejected = 0, _walkMsLast = 0, _walkMsMax = 0;
volatile uint32_t _directReads = 0, _directBytes = 0, _stale = 0, _ioErrors = 0;
inline void add(volatile uint32_t& c, uint32_t n = 1) { __sync_fetch_and_add(&c, n); }

bool _takeTable() {
  if (_table) return true;
  Lock lock;
  if (!_table) _table = (Entry*)heap_caps_calloc(ENTRIES, sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return _table != nullptr;
}

void _free(Map* m) {
  if (!m) return;
  if (m->ext) heap_caps_free(m->ext);
  heap_caps_free(m);
}

/* THE CHECK AGAINST FatFs: a map that does not give back the bytes FatFs gives is worse than no map.
 * Spans at the start (unaligned), in the middle, and at the very end of the file — the places a wrong
 * first cluster, a wrong data base or a short last extent would show. */
bool _agreesWithFatFs(Map* m, const String& path, uint32_t* spansOut = nullptr, uint32_t* bytesOut = nullptr) {
  File f = SdSpiDisk::open(path.c_str());
  if (!f) return false;
  constexpr size_t LEN = 2048;
  uint8_t* a = (uint8_t*)heap_caps_malloc(2 * LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!a) { f.close(); return false; }
  uint8_t* b = a + LEN;
  const uint32_t offs[3] = { 1, m->size / 2 + 37, m->size > LEN + 5 ? m->size - LEN - 5 : 0 };
  bool ok = true;
  for (uint8_t i = 0; i < 3 && ok; i++) {
    const uint32_t off = offs[i];
    const size_t want = (m->size - off < LEN) ? m->size - off : LEN;
    if (!want) continue;
    Status st;
    const size_t got = read(m, off, a, want, st);
    size_t ref = 0;
    if (f.seek(off)) ref = f.read(b, want);
    ok = (got == want && ref == want && !memcmp(a, b, want));
    if (spansOut) (*spansOut)++;
    if (bytesOut) *bytesOut += (uint32_t)want;
  }
  f.close();
  heap_caps_free(a);
  return ok;
}

}  // namespace

size_t read(Map* m, uint32_t offset, uint8_t* dst, size_t n, Status& status) {
  status = Status::Ok;
  if (!m || m->gen != SdSpiDisk::generation()) { status = Status::Stale; add(_stale); return 0; }
  const View v{ &m->g, m->ext, m->nExt, m->size };
  bool ioFailed = false;
  uint8_t bounce[SECTOR];
  const size_t got = readBytes(v, offset, dst, n,
      [&](uint32_t sector, uint32_t count, uint8_t* d) {
        if (SdSpiDisk::readSectors(d, sector, count)) return true;
        ioFailed = true; return false;
      }, bounce);
  add(_directReads); add(_directBytes, (uint32_t)got);
  if (got < n && ioFailed) { status = Status::IoError; add(_ioErrors); }
  return got;
}

bool build(const char* name) {
  if (!name || !*name || !_takeTable()) return false;
  const String path = String(SdCard::FOLDER) + "/" + name;
  SdSpiDisk::FsGeometry fg;
  if (!SdSpiDisk::fsGeometry(fg)) {
    add(_rejected);
    NIDMI_WEB_LOG("[SD] %s : pas de carte des clusters (FAT16/FAT32 a secteurs de 512 o seulement) — lu par FatFs", name);
    return false;
  }
  uint32_t first = 0, bytes = 0;
  if (!SdSpiDisk::fileStart(path.c_str(), first, bytes)) {
    add(_rejected);
    NIDMI_WEB_LOG("[SD] %s : pas de carte des clusters (premier cluster illisible) — lu par FatFs", name);
    return false;
  }
  Geometry g;
  g.fatBits = fg.fatBits; g.fatBase = fg.fatBase; g.dataBase = fg.dataBase;
  g.clusterSectors = fg.clusterSectors; g.nEntries = fg.nEntries;

  Extent* ext = nullptr;
  uint32_t n = 0, cap = 0, clusters = 0, reads = 0;
  const uint32_t t0 = millis();
  const Walk w = walk(g, first, bytes,
      [&](uint32_t sector, uint8_t* dst) {
        if ((++reads & 15) == 0) vTaskDelay(1);                 // a long chain: let core 0's IDLE task run
        return SdSpiDisk::readSectors(dst, sector, 1);
      },
      [&](const Extent& e) {
        if (n >= MAX_EXTENTS) return false;
        if (n == cap) {
          const uint32_t ncap = cap ? cap * 2 : 16;
          Extent* bigger = (Extent*)heap_caps_realloc(ext, ncap * sizeof(Extent), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if (!bigger) return false;
          ext = bigger; cap = ncap;
        }
        ext[n++] = e;
        return true;
      }, &clusters);
  const uint32_t walkMs = millis() - t0;
  _walkMsLast = walkMs; if (walkMs > _walkMsMax) _walkMsMax = walkMs;
  if (w != Walk::Ok) {
    if (ext) heap_caps_free(ext);
    add(_rejected);
    NIDMI_WEB_LOG("[SD] %s : pas de carte des clusters (parcours %d : %s) — lu par FatFs", name, (int)w,
                  w == Walk::TooManyExtents ? "trop fragmente" : w == Walk::IoError ? "lecture" : "chaine incoherente");
    return false;
  }
  Map* m = (Map*)heap_caps_calloc(1, sizeof(Map), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!m) { heap_caps_free(ext); add(_rejected); return false; }
  m->g = g; m->ext = ext; m->nExt = n; m->size = bytes; m->gen = SdSpiDisk::generation(); m->clusters = clusters; m->refs = 1;
  if (!_agreesWithFatFs(m, path)) {
    _free(m);
    add(_rejected);
    NIDMI_WEB_LOG("[SD] %s : la carte des clusters ne rend pas les octets de FatFs — ecartee, lu par FatFs", name);
    return false;
  }
  Map* old = nullptr;
  {
    Lock lock;
    Entry* e = nullptr;
    for (uint8_t i = 0; i < ENTRIES; i++) if (_table[i].map && !strcmp(_table[i].name, name)) { e = &_table[i]; break; }
    if (!e) for (uint8_t i = 0; i < ENTRIES; i++) if (!_table[i].map) { e = &_table[i]; break; }
    if (!e) { _free(m); add(_rejected); return false; }
    old = e->map;
    strlcpy(e->name, name, sizeof(e->name));
    e->map = m;
  }
  if (old) release(old);
  add(_built);
  NIDMI_WEB_LOG("[SD] carte des clusters de %s : %u extent(s), %u clusters de %u Ko, parcours %u ms (%u lectures de FAT)",
                name, (unsigned)n, (unsigned)clusters, (unsigned)(g.clusterSectors / 2), (unsigned)walkMs, (unsigned)reads);
  return true;
}

void rebuildAll() {
  if (!_table) return;
  for (uint8_t i = 0; i < ENTRIES; i++) {
    char name[SampleStore::NOM_MAX];
    {
      Lock lock;
      if (!_table[i].map) continue;
      strlcpy(name, _table[i].name, sizeof(name));
    }
    build(name);
  }
}

Map* acquire(const char* name) {
  if (!_table || !name) return nullptr;
  Lock lock;
  for (uint8_t i = 0; i < ENTRIES; i++) {
    Map* m = _table[i].map;
    if (m && !strcmp(_table[i].name, name)) {
      if (m->gen != SdSpiDisk::generation()) return nullptr;     // stale: FatFs, until it is rebuilt
      m->refs++;
      return m;
    }
  }
  return nullptr;
}

void release(Map* m) {
  if (!m) return;
  bool last = false;
  { Lock lock; last = (--m->refs == 0); }
  if (last) _free(m);
}

Verify verify(const char* name, uint32_t spans) {
  Verify out{ 0, 0, 0, false };
  Map* m = acquire(name);
  if (!m) return out;
  const String path = String(SdCard::FOLDER) + "/" + name;
  File f = SdSpiDisk::open(path.c_str());
  constexpr size_t MAXLEN = 16384;
  uint8_t* a = (uint8_t*)heap_caps_malloc(2 * MAXLEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (f && a && m->size > 8) {
    out.ran = true;
    uint8_t* b = a + MAXLEN;
    for (uint32_t i = 0; i < spans; i++) {
      uint32_t off = esp_random() % m->size, len = 1 + esp_random() % MAXLEN;
      if (i == 0) off = 0;
      if (i == 1) off = m->size - 1;
      if (i == 2) off = m->size > 700 ? m->size - 700 : 0;       // the last sector: a short span at the very end
      if (i % 4 == 3) { off &= ~511u; }                            // some starting on a sector boundary
      const size_t want = (m->size - off < len) ? m->size - off : len;
      Status st;
      const size_t got = read(m, off, a, want, st);
      size_t ref = 0;
      if (f.seek(off)) ref = f.read(b, want);
      out.spans++; out.bytes += (uint32_t)want;
      if (got != want || ref != want || memcmp(a, b, want)) out.mismatches++;
      vTaskDelay(1);
    }
  }
  if (f) f.close();
  if (a) heap_caps_free(a);
  release(m);
  return out;
}

String diagnostics() {
  String j = "{\"built\":" + String((unsigned long)_built)
           + ",\"rejected\":" + String((unsigned long)_rejected)
           + ",\"walk_ms_last\":" + String((unsigned long)_walkMsLast)
           + ",\"walk_ms_max\":" + String((unsigned long)_walkMsMax)
           + ",\"direct_reads\":" + String((unsigned long)_directReads)
           + ",\"direct_kb\":" + String((unsigned long)(_directBytes / 1024))
           + ",\"stale\":" + String((unsigned long)_stale)
           + ",\"io_errors\":" + String((unsigned long)_ioErrors)
           + ",\"maps\":[";
  if (_table) {
    Lock lock;
    bool first = true;
    for (uint8_t i = 0; i < ENTRIES; i++) {
      const Map* m = _table[i].map;
      if (!m) continue;
      if (!first) j += ",";
      first = false;
      j += "{\"name\":\"" + String(_table[i].name) + "\",\"extents\":" + String((unsigned long)m->nExt)
         + ",\"clusters\":" + String((unsigned long)m->clusters) + ",\"cluster_kb\":" + String((unsigned long)(m->g.clusterSectors / 2))
         + ",\"bytes\":" + String((unsigned long)m->size)
         + ",\"fresh\":" + String(m->gen == SdSpiDisk::generation() ? "true" : "false") + "}";
    }
  }
  j += "]}";
  return j;
}

}  // namespace SdClusters
