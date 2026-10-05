#pragma once
// src/audio/SdStream.h — PLAYING A LONG SOUND FROM THE SD CARD WITHOUT LOADING IT.
//
// PSRAM: 8 MB for all sounds. A multi-minute SD sound does not fit, and that is
// exactly what an SD card is for. So it is STREAMED: a reader (a task of its own)
// fills a ring buffer in PSRAM a little ahead of time, and the audio task reads
// ONLY that buffer. This is the disk-streaming principle of a sampler; the
// difference with a computer is that nobody does the read-ahead for us, and that
// the audio must NEVER wait for the SD:
//
//   - a read can take 1 to 100+ ms, or a 68 ms seek (measured, MESURES §204), when
//     the audio DMA only has ~30 ms of lead;
//   - an empty buffer yields SILENCE, not a late block: we lose sound, never time.
//     The clip's time keeps running (its position advances); the reader, if it is
//     late, jumps ahead and catches up.
//
// TWO PIECES.
//
//  THE HEAD. A clip that starts in the middle of a long file cannot wait for 68 ms
//  + 24 ms of reading. The START of every armed clip (HEAD_FRAMES, 0.37 s at
//  44.1 kHz) is therefore read ahead, once, by the SD-card task, and kept in
//  PSRAM: the clip starts from the head, instantly, while the reader positions
//  itself behind and fills the buffer from the end of the head. A head is
//  identified by (sound, start frame): two clips that start at the same place
//  share it. It is EVICTED when nothing names it any more (AudioEngine).
//
//  THE STREAM. One playlist voice, one buffer (RING_FRAMES), one open file. The
//  voice plays the head, then the buffer. On loop or chaining, the voice starts
//  again from a clip's head and the stream repositions (`start` / `restart`).
//
// WHO WRITES WHAT (one writer per field, no lock in the audio task):
//   the audio:  seq, startFrame, endFrame, name, dataOffset, channels, consumer
//   the reader: lo, hi, seqSeen, and its files
// `seqSeen == seq`: the reader has positioned itself for the last request; before
// that the buffer is not to be read (the head covers the wait).
#include <Arduino.h>

namespace SdStream {

constexpr uint8_t  MAX_STREAMS  = 3;        // simultaneous streams (one per playing playlist voice)
constexpr uint8_t  MAX_HEADS    = 64;       // table entries (a few dozen bytes each)
constexpr uint32_t HEAD_FRAMES  = 16384;    // a clip's head: 0.37 s at 44.1 kHz
/* THE HEAD BUDGET, IN BYTES, not a count: a stereo head takes 64 KB, a mono one 32 KB.
 * 2 MB = ~32 stereo clips (~64 mono) with a head — a quarter of the free PSRAM
 * (8.2 MB at boot). Sum of the PSRAM ceilings: storage sounds 1.56 MB + preloaded SD
 * sounds 3 MB + heads 2 MB + buffers ~0.5 MB ≈ 7 MB — hence the reserve below: NO head
 * is taken if the free PSRAM falls under 1 MB (the web server's response buffers, the
 * banks and the console live there). */
constexpr uint32_t HEADS_BUDGET_BYTES = 2u * 1024u * 1024u;
/* ANTICIPATED heads (the next cue, a block's "Preload" option) only get half of it: the
 * playing cue always comes first, and evicts them if needed. */
constexpr uint32_t ANTICIPATED_BUDGET_BYTES = 1u * 1024u * 1024u;
constexpr uint32_t PSRAM_RESERVE_BYTES = 1u * 1024u * 1024u;
constexpr uint32_t RING_FRAMES  = 32768;    // one buffer: 0.74 s at 44.1 kHz (power of 2)
constexpr uint32_t CHUNK_FRAMES = 4096;     // what the reader reads in one go
constexpr uint32_t GUARD_FRAMES = 1024;     // never write closer than this to the audio reader
constexpr uint32_t JUMP_FRAMES  = 2048;     // an overtaken reader repositions this far ahead

// ── Heads ────────────────────────────────────────────────────────────────────
/* The index of the head of (name, startFrame) — requested if it does not exist. It is
 * loaded in the SD-card task; `headReady` says when. `anticipated`: for the NEXT cue
 * (under its half-budget; a current request promotes it, and may evict it until it has).
 *   >= 0: the index;  -1: not a possible head (sound absent, not streamed, start past the end);
 *   -2: the BUDGET is full (or the PSRAM is under its reserve, or the table is) — the caller
 *       evicts what it can, asks again, then records a refusal (`recordRefusal`);
 *   -3: an ANTICIPATED request, and the half-budget of anticipated heads is full — only a
 *       stale anticipated head can make room for it (evicting current heads would not help). */
constexpr int8_t HEAD_UNKNOWN = -1, HEAD_BUDGET_FULL = -2, HEAD_ANTICIPATED_FULL = -3;
int8_t   requestHead(const char* name, uint32_t startFrame, bool anticipated = false);
bool     headAnticipated(int8_t i);                     // requested for the next cue, not yet promoted
bool     headEvictable(int8_t i);                       // ready or failed — never a head being read by the SD task
void     recordRefusal();                               // a clip got no head: budget full
void     recordAnticipatedRefusal();                    // an anticipated head found no room (harmless)
bool     headReady(int8_t i);
struct   HeadView { const int16_t* pcm; uint32_t start; uint32_t frames; };
bool     headView(int8_t i, HeadView& v);               // false if it is not ready
uint32_t headAgeMs(int8_t i);                           // since the last request; 0 if free
bool     headInUse(int8_t i);                           // the table holds it (ready, requested or failed)
void     releaseHead(int8_t i);                         // the caller checked that nothing names it
void     loadHeads();                                   // run by the SD-card task
bool     headLoading(int8_t i);                         // requested, not read yet (nor failed)
void     prioritize(int8_t i);                          // a note is waiting for it: read it before the others
void     recordWait(uint32_t ms);                       // a note waited for its head (diagnostic counters)
void     recordDrop();                                  // ... or was given up on
void     recordNoHead();                                // a note whose clip has NO head (failed, refused): nothing to wait for
bool     failedHeads();                                 // some failed head may be read again (SdCard::service asks)

// ── Streams ──────────────────────────────────────────────────────────────────
// Called by the AUDIO TASK, and only it (except diagnostics).
int8_t   acquire(uint8_t voice);                        // -1: no stream left, or no PSRAM left
bool     owns(int8_t f, uint8_t voice);
bool     active(int8_t f);
uint8_t  voiceOf(int8_t f);
void     release(int8_t f);
/* (Re)positions the stream: fill [startFrame, endFrame) of `name`. `dataOffsetBytes`: where
 * the data begins in the file. */
void     start(int8_t f, const char* name, uint32_t startFrame, uint32_t endFrame,
               uint8_t channels, uint32_t dataOffsetBytes);
void     restart(int8_t f);                             // same parameters: the voice has looped
void     position(int8_t f, uint32_t frame);            // where the voice is
/* A frame of the buffer. False: not (or no longer) in the buffer — silence. */
bool     read(int8_t f, uint32_t frame, int16_t& left, int16_t& right);
void     underrun(int8_t f);                            // a block ran short of data
void     stopAll();                                     // the card unmounts: give the streams back and close the files

String   diagnostics();

}  // namespace SdStream
