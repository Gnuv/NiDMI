#include "SdStream.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <new>

#include "SampleStore.h"
#include "../config/SdCard.h"
#include "../server/WebDebugConsole.h"

namespace SdStream {
namespace {

// ── Heads ────────────────────────────────────────────────────────────────────
struct Head {
  char           name[SampleStore::NOM_MAX];
  uint32_t       start   = 0;
  uint32_t       frames  = 0;
  uint32_t       seenMs  = 0;
  int16_t*       pcm     = nullptr;
  volatile uint8_t state = 0;           // 0 free, 1 requested, 2 ready, 3 failed
  volatile bool  urgent  = false;       // a note is waiting for it: read it before the others
  volatile bool  anticipated = false;   // requested for the NEXT cue (promoted as soon as a current request names it)
  uint32_t       bytes   = 0;           // what it weighs in the budget (0: failed, nothing taken)
  uint8_t        tries   = 0;           // failed reads so far (a glitch is read again, up to MAX_TRIES)
  bool           retryable = false;     // the failure may pass (card, PSRAM), unlike "not a streamed sound"
};
constexpr uint8_t MAX_TRIES = 3;
Head* _heads = nullptr;               // MAX_HEADS, in PSRAM, taken on first need (see _takeTables)

/* The head table changes under several tasks (the cue, the web server, the SD-card
 * task): a short lock, never held during a read. */
StaticSemaphore_t _headsLockBuffer;
SemaphoreHandle_t _headsLock = xSemaphoreCreateMutexStatic(&_headsLockBuffer);
struct HeadsLock {
  HeadsLock()  { if (_headsLock) xSemaphoreTake(_headsLock, portMAX_DELAY); }
  ~HeadsLock() { if (_headsLock) xSemaphoreGive(_headsLock); }
};

String _path(const char* name) { return String(SdCard::FOLDER) + "/" + name; }

/* Read head i: open, position, read HEAD_FRAMES — in the SD-card task. */
bool _loadHead(uint8_t i) {                          // false: nothing changed (the card is not there)
  Head& h = _heads[i];
  if (!SdCard::mounted()) return false;             // the card is not there: it stays "requested"
  /* A failure is final for this attempt: `retryable` says whether it may pass (the card, PSRAM)
   * or not (a sound that is not streamed, a start beyond the end) — see _retryFailed. */
  auto fail = [&](bool retryable) { h.bytes = 0; h.retryable = retryable; if (retryable && h.tries < 255) h.tries++; h.state = 3; return true; };
  const int s = SampleStore::indexDe(h.name);
  if (s < 0 || !SampleStore::isStreamed((uint8_t)s)) {
    NIDMI_WEB_LOG("[SD] tete de %s : ce n'est pas un son lu en flux", h.name);
    return fail(false);
  }
  const uint32_t total = (uint32_t)SampleStore::trames((uint8_t)s);
  const uint8_t  channels = SampleStore::stereo((uint8_t)s) ? 2 : 1;
  if (h.start >= total) {
    NIDMI_WEB_LOG("[SD] tete de %s : le debut (trame %lu) est au-dela de la fin", h.name, (unsigned long)h.start);
    return fail(false);
  }
  const uint32_t n = (total - h.start < HEAD_FRAMES) ? total - h.start : HEAD_FRAMES;
  File f = SdCard::open(_path(h.name).c_str());
  if (!f) { NIDMI_WEB_LOG("[SD] tete de %s : ouverture impossible", h.name); return fail(true); }
  int16_t* pcm = (int16_t*)heap_caps_malloc((size_t)n * channels * 2, MALLOC_CAP_SPIRAM);
  if (!pcm) { f.close(); NIDMI_WEB_LOG("[SD] tete de %s : PSRAM insuffisante", h.name); return fail(true); }
  const uint32_t t0 = millis();
  const size_t   bytes = (size_t)n * channels * 2;
  size_t got = 0;
  if (f.seek(SampleStore::dataOffset((uint8_t)s) + (size_t)h.start * channels * 2)) {
    while (got < bytes) {
      const size_t step = (bytes - got < 16384) ? bytes - got : 16384;
      const size_t k = f.read((uint8_t*)pcm + got, step);
      if (!k) break;
      got += k;
      vTaskDelay(1);                                  // yield: core 0's IDLE task
    }
  }
  f.close();
  if (got != bytes) {
    heap_caps_free(pcm);
    NIDMI_WEB_LOG("[SD] tete de %s : lecture incomplete (%u / %u o)", h.name, (unsigned)got, (unsigned)bytes);
    return fail(true);
  }
  h.pcm = pcm; h.frames = n; h.tries = 0;
  __sync_synchronize();
  h.state = 2;
  NIDMI_WEB_LOG("[SD] tete de %s @%lu : %lu trames en %lu ms", h.name, (unsigned long)h.start,
                (unsigned long)n, (unsigned long)(millis() - t0));
  return true;
}

// ── Streams ──────────────────────────────────────────────────────────────────
struct Stream {
  volatile uint8_t state = 0;           // 0 free, 1 active
  uint8_t          voice = 0;
  uint8_t          channels = 2;
  char             name[SampleStore::NOM_MAX] = {0};
  uint32_t         dataOffset = 0;      // bytes: where the data begins in the file
  uint32_t         startFrame = 0, endFrame = 0; // frames: the buffer fills from [startFrame, endFrame)
  volatile uint32_t seq = 0;            // written by the audio
  volatile uint32_t consumer = 0;
  volatile uint32_t seqSeen = 0;        // written by the reader
  volatile uint32_t lo = 0, hi = 0;     // [lo, hi): the valid frames of the buffer
  volatile uint32_t underruns = 0, jumps = 0, starts = 0;
  int16_t*         ring = nullptr;      // RING_FRAMES x 2 int16, in PSRAM, taken once
  // the reader's own:
  File             file;
  char             openName[SampleStore::NOM_MAX] = {0};
  uint32_t         readEnd = 0;         // the end it aims for (copy of `endFrame` at the last request)
  uint32_t         retryAtMs = 0;       // a read failed: not before this time (0: no retry pending)
  uint8_t          errStreak = 0;       // failed reads in a row; MAX_ERRORS and the stream gives up
};
/* A READ THAT FAILS DOES NOT KILL THE STREAM: the reader replaces itself (a failed read leaves
 * FatFs' position unreliable) and tries again RETRY_MS later — a burst of noise on the bus must
 * not silence a four-minute clip until its end. The ring holds 0.74 s of audio: a hiccup
 * shorter than that is inaudible. After MAX_ERRORS in a row (~4 s) the stream gives up; the
 * card supervisor (SdCard) will have checked the card by then. */
constexpr uint8_t  MAX_ERRORS = 25;
constexpr uint32_t RETRY_MS   = 150;
Stream* _streams = nullptr;           // MAX_STREAMS, likewise
volatile uint32_t _underrunsTotal = 0;
volatile uint32_t _delayedNotes = 0, _droppedNotes = 0, _worstWaitMs = 0, _noHeadNotes = 0, _readErrors = 0, _noStream = 0;
volatile uint32_t _refusals = 0, _anticipatedRefusals = 0;
volatile uint32_t _readerAlive = 0;
volatile bool     _stopReader = false;

/* THE TABLES ARE IN PSRAM. A few hundred bytes each, but the largest contiguous block
 * of internal RAM is the scarce resource (RESSOURCES_CARTE.md): a `.bss` of over a
 * kilobyte cost it 1,024 bytes, measured. Taken at the first streamed clip — never by
 * the audio task —, under lock, and never given back. */
bool _takeTables() {
  if (_heads && _streams) return true;
  if (!_headsLock) return false;
  xSemaphoreTake(_headsLock, portMAX_DELAY);
  if (!_heads) _heads = (Head*)heap_caps_calloc(MAX_HEADS, sizeof(Head), MALLOC_CAP_SPIRAM);
  if (!_streams) {
    void* m = heap_caps_calloc(MAX_STREAMS, sizeof(Stream), MALLOC_CAP_SPIRAM);
    if (m) _streams = new (m) Stream[MAX_STREAMS];   // File has a constructor: run it
  }
  const bool ok = _heads && _streams;
  xSemaphoreGive(_headsLock);
  return ok;
}

void _startReader();

/* SERVE ONE STREAM: position it if there is a request, then fill it while there is
 * room. One chunk at a time (CHUNK_FRAMES) — the task yields between two. */
bool _serve(Stream& x) {
  uint32_t s = x.seq;
  if (s != x.seqSeen) {                                   // a (re)request: position
    char     name[SampleStore::NOM_MAX];
    strlcpy(name, x.name, sizeof(name));
    const uint32_t from = x.startFrame, end = x.endFrame, offset = x.dataOffset;
    const uint8_t  channels = x.channels;
    if (x.seq != s) return true;                          // it changed during the copy: next turn
    if (strcmp(name, x.openName) || !x.file) {
      if (x.file) x.file.close();
      x.openName[0] = 0;
      x.file = SdCard::open(_path(name).c_str());
      if (x.file) strlcpy(x.openName, name, sizeof(x.openName));
    }
    x.lo = from; x.hi = from; x.readEnd = end;
    x.retryAtMs = 0; x.errStreak = 0;
    if (!x.file || !x.file.seek(offset + (size_t)from * channels * 2)) {
      NIDMI_WEB_LOG("[SD] flux de %s : placement impossible a la trame %lu", name, (unsigned long)from);
      x.readEnd = from;                                   // nothing to read: silence
    }
    x.starts = x.starts + 1;
    __sync_synchronize();
    x.seqSeen = s;
    return true;
  }
  if (x.retryAtMs) {                                      // a read failed a moment ago
    if ((int32_t)(millis() - x.retryAtMs) < 0) return false;
    x.retryAtMs = 0;
    /* REOPEN, then place: a disk error is STICKY in FatFs (`fp->err`) — every later read or
     * seek on that file object fails, the card back or not. Only a fresh open recovers. */
    char name[SampleStore::NOM_MAX];
    strlcpy(name, x.openName, sizeof(name));          // `openName` stays: the next try needs it if this one fails
    if (x.file) x.file.close();
    if (name[0]) x.file = SdCard::open(_path(name).c_str());
    if (!x.file || !x.file.seek(x.dataOffset + (size_t)x.hi * x.channels * 2)) {
      _readErrors = _readErrors + 1;
      if (x.errStreak == 0 || x.errStreak % 10 == 9)
        NIDMI_WEB_LOG("[SD] flux de %s : reprise impossible a la trame %lu (%s)", name, (unsigned long)x.hi, x.file ? "placement" : "ouverture");
      if (++x.errStreak >= MAX_ERRORS) { x.readEnd = x.hi; return false; }
      x.retryAtMs = millis() + RETRY_MS;
      return false;
    }
    NIDMI_WEB_LOG("[SD] flux de %s : reprise a la trame %lu apres %u echec(s)", name, (unsigned long)x.hi, (unsigned)x.errStreak);
  }
  uint32_t hi = x.hi;
  if (hi >= x.readEnd || !x.file) return false;           // everything is read
  const uint32_t consumer = x.consumer;
  /* THE VOICE HAS OVERTAKEN US: it read past what we had. We jump ahead instead of
   * reading what it will never replay — silence in the meantime. */
  if (consumer >= x.startFrame && consumer > hi) {
    const uint32_t target = consumer + JUMP_FRAMES;
    if (target >= x.readEnd) { x.hi = x.lo = x.readEnd; return false; }
    x.lo = target; __sync_synchronize(); x.hi = target;
    if (!x.file.seek(x.dataOffset + (size_t)target * x.channels * 2)) { x.readEnd = target; return false; }
    x.jumps = x.jumps + 1;
    hi = target;
  }
  const uint32_t base  = (consumer > x.lo) ? consumer : x.lo;
  uint32_t       limit = base + RING_FRAMES - GUARD_FRAMES;
  if (limit > x.readEnd) limit = x.readEnd;
  if (hi >= limit) return false;                          // the buffer is full
  uint32_t n = limit - hi;
  if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;
  const uint32_t toRingEnd = RING_FRAMES - (hi & (RING_FRAMES - 1));
  if (n > toRingEnd) n = toRingEnd;                       // do not straddle the end of the buffer
  const size_t bytes = (size_t)n * x.channels * 2;
  int16_t* dest = x.ring + (size_t)(hi & (RING_FRAMES - 1)) * x.channels;
  const size_t got = x.file.read((uint8_t*)dest, bytes);
  const uint32_t frames = (uint32_t)(got / ((size_t)x.channels * 2));
  __sync_synchronize();
  x.hi = hi + frames;
  if (got != bytes) {
    if (hi + frames >= x.endFrame) { x.readEnd = hi + frames; return false; }     // the end of the clip
    _readErrors = _readErrors + 1;                        // the read FAILED: keep what came, try again shortly
    if (++x.errStreak >= MAX_ERRORS) { x.readEnd = hi + frames; return false; }
    x.retryAtMs = millis() + RETRY_MS;
    return false;
  }
  x.errStreak = 0;
  return true;
}

void _readerTask(void*) {
  uint32_t idleSince = millis();
  for (;;) {
    if (_stopReader || !_streams) break;                   // the card is unmounting: give everything back
    bool anyActive = false, didWork = false;
    for (uint8_t i = 0; i < MAX_STREAMS; i++) {
      Stream& x = _streams[i];
      if (x.state == 1) { anyActive = true; if (_serve(x)) didWork = true; }
      else if (x.file) { x.file.close(); x.openName[0] = 0; }   // a released stream: its file too
    }
    if (anyActive) idleSince = millis();
    else if (millis() - idleSince > 1500) break;
    vTaskDelay(didWork ? 1 : pdMS_TO_TICKS(4));            // never a busy turn
  }
  if (_streams) for (uint8_t i = 0; i < MAX_STREAMS; i++) if (_streams[i].file) { _streams[i].file.close(); _streams[i].openName[0] = 0; }
  __sync_lock_release(&_readerAlive);
  bool remaining = false;
  if (_streams && !_stopReader) for (uint8_t i = 0; i < MAX_STREAMS; i++) if (_streams[i].state == 1) remaining = true;
  if (remaining) _startReader();
  vTaskDelete(nullptr);
}

/* The reader task: core 0, priority 3 — above the SD-card task (1) that loads and
 * measures, below WiFi, the TCP/IP stack, the audio and the loop. Born and dying on
 * demand: no stack kept in internal RAM when nothing is being read. */
void _startReader() {
  if (__sync_lock_test_and_set(&_readerAlive, 1)) return;
  if (xTaskCreatePinnedToCore(_readerTask, "sd-stream", 6144, nullptr, 3, nullptr, 0) != pdPASS) {
    __sync_lock_release(&_readerAlive);
    NIDMI_WEB_LOG("[SD] lecteur de flux impossible (memoire)");
  }
}

}  // namespace

// ── Heads: API ───────────────────────────────────────────────────────────────
int8_t requestHead(const char* name, uint32_t startFrame, bool anticipated) {
  if (!_takeTables()) return HEAD_UNKNOWN;
  /* What this head will weigh: read from the "streamed" slot of the store (length, channels). */
  const int slot = SampleStore::indexDe(name);
  if (slot < 0 || !SampleStore::isStreamed((uint8_t)slot)) return HEAD_UNKNOWN;
  const uint32_t total = (uint32_t)SampleStore::trames((uint8_t)slot);
  if (startFrame >= total) return HEAD_UNKNOWN;
  const uint32_t bytes = ((total - startFrame < HEAD_FRAMES) ? total - startFrame : HEAD_FRAMES)
                       * (SampleStore::stereo((uint8_t)slot) ? 2u : 1u) * 2u;
  int8_t idx = HEAD_UNKNOWN;
  bool isNew = false;
  {
    HeadsLock lock;
    int8_t freeSlot = -1;
    uint32_t taken = 0, anticipatedBytes = 0;
    for (int8_t i = 0; i < (int8_t)MAX_HEADS; i++) {
      Head& h = _heads[i];
      /* A failed head asked for AGAIN is a new try (a glitch must not silence a clip for good):
       * its entry goes back to the free ones and is allocated afresh, budget included. */
      if (h.state == 3 && h.start == startFrame && !strcmp(h.name, name)) { h.pcm = nullptr; h.tries = 0; h.state = 0; }
      if (h.state == 0) { if (freeSlot < 0) freeSlot = i; continue; }
      taken += h.bytes;
      if (h.anticipated) anticipatedBytes += h.bytes;
      if (idx == HEAD_UNKNOWN && h.start == startFrame && !strcmp(h.name, name)) {
        h.seenMs = millis();
        if (!anticipated) h.anticipated = false;          // a current request PROMOTES it
        idx = i;
      }
    }
    if (idx == HEAD_UNKNOWN) {
      const bool roomInTable = freeSlot >= 0;
      const bool withinBudget = taken + bytes <= HEADS_BUDGET_BYTES
                             && (!anticipated || anticipatedBytes + bytes <= ANTICIPATED_BUDGET_BYTES);
      const bool psramOk = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= PSRAM_RESERVE_BYTES + bytes;
      if (!roomInTable || !withinBudget || !psramOk) {
        const bool totalBudgetOk = roomInTable && taken + bytes <= HEADS_BUDGET_BYTES && psramOk;
        idx = (anticipated && totalBudgetOk) ? HEAD_ANTICIPATED_FULL : HEAD_BUDGET_FULL;   // only the half-budget is short
      } else {
        Head& h = _heads[freeSlot];
        strlcpy(h.name, name, sizeof(h.name));
        h.start = startFrame; h.frames = 0; h.pcm = nullptr; h.seenMs = millis();
        h.bytes = bytes; h.urgent = false; h.anticipated = anticipated;
        __sync_synchronize();
        h.state = 1;
        idx = freeSlot; isNew = true;
      }
    }
  }
  if (isNew) SdCard::loadHeads();
  return idx;
}

bool headAnticipated(int8_t i) { return _heads && i >= 0 && i < (int8_t)MAX_HEADS && _heads[i].state != 0 && _heads[i].anticipated; }
bool headEvictable(int8_t i)   { return _heads && i >= 0 && i < (int8_t)MAX_HEADS && (_heads[i].state == 2 || _heads[i].state == 3); }
void recordRefusal()           { _refusals = _refusals + 1; }
void recordAnticipatedRefusal() { _anticipatedRefusals = _anticipatedRefusals + 1; }

bool headReady(int8_t i) { return _heads && i >= 0 && i < (int8_t)MAX_HEADS && _heads[i].state == 2; }

bool headView(int8_t i, HeadView& v) {
  if (!headReady(i)) return false;
  v.pcm = _heads[i].pcm; v.start = _heads[i].start; v.frames = _heads[i].frames;
  return true;
}

uint32_t headAgeMs(int8_t i) {
  if (!_heads || i < 0 || i >= (int8_t)MAX_HEADS || _heads[i].state == 0) return 0;
  return millis() - _heads[i].seenMs;
}
bool headInUse(int8_t i) { return _heads && i >= 0 && i < (int8_t)MAX_HEADS && _heads[i].state != 0; }

void releaseHead(int8_t i) {
  if (!_heads || i < 0 || i >= (int8_t)MAX_HEADS) return;
  HeadsLock lock;
  Head& h = _heads[i];
  int16_t* pcm = h.pcm;
  h.state = 0;
  __sync_synchronize();
  h.pcm = nullptr; h.frames = 0; h.bytes = 0; h.anticipated = false; h.urgent = false;
  if (pcm) heap_caps_free(pcm);
}

/* One head at a time, THE URGENT ONE FIRST: the one a note waits for goes ahead of the
 * others (it then waits for the head being read, at most, plus its own). An absent card
 * stops the loop — the heads stay "requested". */
/* Failed heads whose cause may have passed (the card was away, a read glitched): back to
 * "requested", budget permitting, at most MAX_TRIES times — a clip must not stay silent for the
 * rest of the concert because one read failed. Called by the SD-card task before it reads. */
void _retryFailed() {
  HeadsLock lock;
  uint32_t taken = 0;
  for (uint8_t i = 0; i < MAX_HEADS; i++) if (_heads[i].state != 0) taken += _heads[i].bytes;
  for (uint8_t i = 0; i < MAX_HEADS; i++) {
    Head& h = _heads[i];
    if (h.state != 3 || !h.retryable || h.tries >= MAX_TRIES) continue;
    const int slot = SampleStore::indexDe(h.name);
    if (slot < 0 || !SampleStore::isStreamed((uint8_t)slot)) continue;
    const uint32_t total = (uint32_t)SampleStore::trames((uint8_t)slot);
    if (h.start >= total) continue;
    const uint32_t bytes = ((total - h.start < HEAD_FRAMES) ? total - h.start : HEAD_FRAMES)
                         * (SampleStore::stereo((uint8_t)slot) ? 2u : 1u) * 2u;
    if (taken + bytes > HEADS_BUDGET_BYTES) continue;
    taken += bytes;
    h.bytes = bytes; h.seenMs = millis(); h.state = 1;
  }
}

bool failedHeads() {
  if (!_heads) return false;
  for (uint8_t i = 0; i < MAX_HEADS; i++)
    if (_heads[i].state == 3 && _heads[i].retryable && _heads[i].tries < MAX_TRIES) return true;
  return false;
}

void loadHeads() {
  if (!_heads) return;
  _retryFailed();
  for (;;) {
    int8_t pick = -1;
    for (uint8_t i = 0; i < MAX_HEADS && pick < 0; i++) if (_heads[i].state == 1 && _heads[i].urgent) pick = (int8_t)i;
    for (uint8_t i = 0; i < MAX_HEADS && pick < 0; i++) if (_heads[i].state == 1 && !_heads[i].anticipated) pick = (int8_t)i;
    for (uint8_t i = 0; i < MAX_HEADS && pick < 0; i++) if (_heads[i].state == 1) pick = (int8_t)i;   // anticipated ones, last
    if (pick < 0) break;
    if (!_loadHead((uint8_t)pick)) break;
  }
}

bool headLoading(int8_t i) { return _heads && i >= 0 && i < (int8_t)MAX_HEADS && _heads[i].state == 1; }
void prioritize(int8_t i)  { if (headLoading(i)) _heads[i].urgent = true; }

/* The notes that waited for their head: how many, the longest wait, and those we had to
 * give up on. Written by the audio task — plain counters, never a log. */
void recordWait(uint32_t ms) {
  _delayedNotes = _delayedNotes + 1;
  if (ms > _worstWaitMs) _worstWaitMs = ms;
}
void recordDrop() { _droppedNotes = _droppedNotes + 1; }
void recordNoHead() { _noHeadNotes = _noHeadNotes + 1; }
void recordNoStream() { _noStream = _noStream + 1; }
uint32_t trouble() { return _underrunsTotal + _noHeadNotes + _noStream + _droppedNotes + _readErrors; }

// ── Streams: API ─────────────────────────────────────────────────────────────
int8_t acquire(uint8_t voice) {
  if (!_streams) return -1;
  for (int8_t i = 0; i < (int8_t)MAX_STREAMS; i++) {
    Stream& x = _streams[i];
    if (x.state != 0) continue;
    if (!x.ring) {
      x.ring = (int16_t*)heap_caps_malloc((size_t)RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
      if (!x.ring) { NIDMI_WEB_LOG("[SD] flux : PSRAM insuffisante pour un tampon"); return -1; }
    }
    x.voice = voice; x.lo = x.hi = 0; x.consumer = 0;
    x.underruns = 0; x.jumps = 0; x.starts = 0;
    x.seqSeen = x.seq;                  // nothing to serve until `start` has spoken
    __sync_synchronize();
    x.state = 1;
    return i;
  }
  return -1;
}

bool    owns(int8_t f, uint8_t voice) { return _streams && f >= 0 && f < (int8_t)MAX_STREAMS && _streams[f].state == 1 && _streams[f].voice == voice; }
bool    active(int8_t f)  { return _streams && f >= 0 && f < (int8_t)MAX_STREAMS && _streams[f].state == 1; }
uint8_t voiceOf(int8_t f) { return (_streams && f >= 0 && f < (int8_t)MAX_STREAMS) ? _streams[f].voice : 0; }

void release(int8_t f) {
  if (!_streams || f < 0 || f >= (int8_t)MAX_STREAMS) return;
  _streams[f].state = 0;                 // the reader closes its file on its next turn
}

void start(int8_t f, const char* name, uint32_t startFrame, uint32_t endFrame, uint8_t channels, uint32_t dataOffsetBytes) {
  if (!_streams || f < 0 || f >= (int8_t)MAX_STREAMS) return;
  Stream& x = _streams[f];
  strlcpy(x.name, name, sizeof(x.name));
  x.dataOffset = dataOffsetBytes; x.channels = channels;
  x.startFrame = startFrame; x.endFrame = endFrame;
  x.consumer = 0;
  __sync_synchronize();
  x.seq = x.seq + 1;
  _startReader();
}

void restart(int8_t f) {
  if (!_streams || f < 0 || f >= (int8_t)MAX_STREAMS) return;
  Stream& x = _streams[f];
  x.consumer = 0;
  __sync_synchronize();
  x.seq = x.seq + 1;
  _startReader();
}

void position(int8_t f, uint32_t frame) {
  if (_streams && f >= 0 && f < (int8_t)MAX_STREAMS) _streams[f].consumer = frame;
}

bool read(int8_t f, uint32_t frame, int16_t& left, int16_t& right) {
  if (!_streams || f < 0 || f >= (int8_t)MAX_STREAMS) return false;
  const Stream& x = _streams[f];
  if (x.seqSeen != x.seq || !x.ring) return false;
  const uint32_t hi = x.hi, lo = x.lo;
  const uint32_t lowest = (hi - lo > RING_FRAMES) ? hi - RING_FRAMES : lo;
  if (frame < lowest || frame >= hi) return false;
  const int16_t* p = x.ring + (size_t)(frame & (RING_FRAMES - 1)) * x.channels;
  if (x.channels == 2) { left = p[0]; right = p[1]; } else { left = right = p[0]; }
  return true;
}

/* The card unmounts: no stream left, no open file. FatFs does not stand an unmount under
 * an open file; the reader closes its own and stops. The voices that were playing finish
 * what their buffer holds, then go silent. */
void stopAll() {
  if (_streams) for (uint8_t i = 0; i < MAX_STREAMS; i++) _streams[i].state = 0;
  if (!_readerAlive) return;
  _stopReader = true;
  for (int i = 0; i < 300 && _readerAlive; i++) vTaskDelay(pdMS_TO_TICKS(10));   // up to 3 s: a read on a lost card fails slowly
  _stopReader = false;
}

void underrun(int8_t f) {
  if (_streams && f >= 0 && f < (int8_t)MAX_STREAMS) _streams[f].underruns = _streams[f].underruns + 1;
  _underrunsTotal = _underrunsTotal + 1;
}

String diagnostics() {
  String j = "{\"reader\":" + String(_readerAlive ? "true" : "false")
           + ",\"underruns_total\":" + String((unsigned long)_underrunsTotal)
           + ",\"delayed_notes\":" + String((unsigned long)_delayedNotes)
           + ",\"worst_wait_ms\":" + String((unsigned long)_worstWaitMs)
           + ",\"dropped_notes\":" + String((unsigned long)_droppedNotes)
           + ",\"notes_without_head\":" + String((unsigned long)_noHeadNotes)
           + ",\"read_errors\":" + String((unsigned long)_readErrors)
           + ",\"clips_without_stream\":" + String((unsigned long)_noStream)
           + ",\"refused_heads\":" + String((unsigned long)_refusals)
           + ",\"refused_anticipated\":" + String((unsigned long)_anticipatedRefusals) + ",\"streams\":[";
  bool first = true;
  for (uint8_t i = 0; _streams && i < MAX_STREAMS; i++) {
    const Stream& x = _streams[i];
    if (x.state != 1) continue;
    if (!first) j += ",";
    first = false;
    const uint32_t hi = x.hi, c = x.consumer;
    j += "{\"name\":\"" + String(x.name) + "\",\"voice\":" + String(x.voice)
       + ",\"position\":" + String((unsigned long)c) + ",\"lead_frames\":" + String(hi > c ? (unsigned long)(hi - c) : 0UL)
       + ",\"end\":" + String((unsigned long)x.endFrame) + ",\"underrun_blocks\":" + String((unsigned long)x.underruns)
       + ",\"jumps\":" + String((unsigned long)x.jumps) + ",\"starts\":" + String((unsigned long)x.starts) + "}";
  }
  uint32_t taken = 0, anticipatedBytes = 0;
  for (uint8_t i = 0; _heads && i < MAX_HEADS; i++) if (_heads[i].state != 0) { taken += _heads[i].bytes; if (_heads[i].anticipated) anticipatedBytes += _heads[i].bytes; }
  j += "],\"heads_budget\":{\"used\":" + String((unsigned long)taken) + ",\"max\":" + String((unsigned long)HEADS_BUDGET_BYTES)
     + ",\"anticipated\":" + String((unsigned long)anticipatedBytes) + ",\"max_anticipated\":" + String((unsigned long)ANTICIPATED_BUDGET_BYTES) + "},\"heads\":[";
  first = true;
  for (uint8_t i = 0; _heads && i < MAX_HEADS; i++) {
    const Head& h = _heads[i];
    if (h.state == 0) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"name\":\"" + String(h.name) + "\",\"start\":" + String((unsigned long)h.start)
       + ",\"state\":\"" + String(h.state == 1 ? "requested" : h.state == 2 ? "ready" : "failed")
       + "\",\"frames\":" + String((unsigned long)h.frames) + ",\"bytes\":" + String((unsigned long)h.bytes)
       + (h.state == 3 ? ",\"tries\":" + String((unsigned)h.tries) + (h.retryable ? "" : ",\"final\":true") : String(""))
       + (h.anticipated ? ",\"anticipated\":true" : "") + "}";
  }
  j += "]}";
  return j;
}

}  // namespace SdStream
