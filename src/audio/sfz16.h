/**
 * sfz16.h — SFZ sampler core for the board: int16 samples, fixed-point playhead.
 *
 * Pure C99, zero dependencies, no allocation anywhere, no file I/O: the host
 * (the card loader, or the host test) decodes WAV files to int16 buffers and
 * hands them over. This file lives in the FIRMWARE (src/audio/, the
 * ClusterMapCore.h model: one home, host-tested from outside); the test that
 * proves it is hardware/bench/sfz16_test.{cpp,sh} in the app repo. The same
 * decisions that MESURES §206 measured on the S3:
 *
 *   - samples stay int16 (half the PSRAM of float32, no copy);
 *   - the playhead is fixed point: an integer frame index plus a 32-bit
 *     phase accumulator (unit 2^-32 frame) — 32-bit arithmetic only in the
 *     per-sample loop, rate resolution 2^-32 (no drift), interpolation weight
 *     15 bits (≤ 1 LSB at 16 bits);
 *   - the amplitude envelope, gains and mixing stay float (the S3 has a
 *     single-precision FPU): renders match the reference core to ≤ 2 LSB.
 *
 * Differences with engines/core/play-sfz/source/sfz.h (the WASM core, kept as
 * the browser prototype and as the parity reference):
 *   - voices live in sfz16_engine (a few KB, internal RAM on the card), not
 *     inside the instrument: an instrument is PSRAM and swappable per cue
 *     (loaded on the cue that names it, unloaded when no cue does — decision
 *     of 6 October), while the engine and its voices persist;
 *   - regions are lean (~80 o against 772): the locc/hicc arrays are gone,
 *     replaced by at most SFZ16_MAX_CC_CONDS sparse entries;
 *   - limits REFUSE with a message (sfz16_parse/add_sample/link fill `err`),
 *     they never truncate in silence;
 *   - <global> inherits into <region> even with no <group> in between (the
 *     WASM core loses it — sfz_parse.c starts regions from group_defaults,
 *     which only receive the global at a <group> header).
 *
 * The single place sample data is touched is sfz16_read_x(): when heads +
 * streaming come (next step), that is the seam.
 */

#ifndef SFZ16_H
#define SFZ16_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compile-time limits — refusing, not truncating ──────────────────────── */
#ifndef SFZ16_MAX_REGIONS
#define SFZ16_MAX_REGIONS   256
#endif
#ifndef SFZ16_MAX_SAMPLES
#define SFZ16_MAX_SAMPLES   128
#endif
#ifndef SFZ16_MAX_VOICES
#define SFZ16_MAX_VOICES    32
#endif
#ifndef SFZ16_MAX_CC_CONDS
#define SFZ16_MAX_CC_CONDS  4      /* distinct locc/hicc controllers per region */
#endif
#ifndef SFZ16_PATH_LEN
#define SFZ16_PATH_LEN      160
#endif

typedef enum {
    SFZ16_LOOP_NONE = 0,
    SFZ16_LOOP_ONESHOT,
    SFZ16_LOOP_CONTINUOUS,
    SFZ16_LOOP_SUSTAIN
} sfz16_loop_mode;

/* ── A decoded sample buffer, host-owned (PSRAM on the card) ─────────────── */
typedef struct {
    const int16_t *data;           /* interleaved frames                      */
    uint32_t       frames;
    uint8_t        channels;       /* 1 or 2                                  */
    float          sample_rate;    /* of the file (44100…)                    */
    char           path[SFZ16_PATH_LEN];
} sfz16_sample;

/* ── A region: one playable zone (~80 o) ─────────────────────────────────── */
typedef struct {
    uint8_t  lokey, hikey;
    uint8_t  lovel, hivel;
    uint8_t  pitch_keycenter;
    float    pitch_keytrack;       /* percent — float, as the reference core   */
    int16_t  tune;                 /* cents                                   */
    int8_t   transpose;            /* semitones                               */
    float    pan;                  /* -100..+100 — float, as the reference    */
    float    volume_db;
    float    amp_veltrack;         /* percent                                 */

    /* DAHDSR (seconds, sustain is a 0..1 level) — same float math as the
     * reference core, so renders stay comparable. */
    float    ampeg_delay, ampeg_attack, ampeg_hold, ampeg_decay,
             ampeg_sustain, ampeg_release;

    int32_t  loop_start, loop_end; /* frames, -1 = whole sample               */
    int32_t  offset, end;
    uint8_t  loop_mode;

    int16_t  group, off_by;
    uint8_t  seq_length, seq_position;
    float    rand_lo, rand_hi;     /* 0..1, span only if rand_hi > rand_lo    */

    /* Sparse CC gate (the 128-entry arrays of the reference core are the
     * whole reason a region weighed 772 o). lo=0, hi=127 = no constraint. */
    uint8_t  cc_count;
    uint8_t  cc[SFZ16_MAX_CC_CONDS];
    uint8_t  cc_lo[SFZ16_MAX_CC_CONDS];
    uint8_t  cc_hi[SFZ16_MAX_CC_CONDS];

    int16_t  sample_index;         /* filled by sfz16_link, -1 until then     */
} sfz16_region;

/* ── The instrument: regions + sample table (PSRAM on the card) ──────────── */
typedef struct {
    sfz16_region regions[SFZ16_MAX_REGIONS];
    int          region_count;

    sfz16_sample samples[SFZ16_MAX_SAMPLES];
    int          sample_count;

    uint8_t      seq_counter[128];  /* round-robin, per note                  */

    float        host_sample_rate;  /* engine rate, set from the loader       */
} sfz16_instrument;

/* What the parser wants to say beyond ok / refused. */
typedef struct {
    int  regions;
    int  unknown_opcodes;          /* ignored tokens (a filter the core has
                                      no idea about, for instance)           */
    char first_unknown[24];
} sfz16_report;

/* ── The engine: voices + CC state, a few KB, INTERNAL RAM on the card ───── */

/* One sounding region instance. The playhead is the pair ipos/fpos: the
 * integer frame index and a 32-bit phase accumulator (unit 2^-32 frame);
 * inc_int/inc_frac advance them with 32-bit arithmetic only. */
typedef struct {
    uint8_t  active, note, velocity, releasing, held;

    const sfz16_region *region;
    const sfz16_sample *sample;

    uint32_t ipos;
    uint32_t fpos;
    uint32_t inc_int;
    uint32_t inc_frac;

    uint32_t loop_start, loop_end, play_end;

    float    amp;                 /* velocity × volume                     */
    float    gain_l, gain_r;      /* after pan                            */
    float    env, env_inc;
    uint8_t  env_stage;           /* 0 delay 1 attack 2 hold 3 decay 4 sustain 5 release */
    uint32_t stage_samples;
    float    sustain_level;
} sfz16_voice_state;

typedef struct {
    sfz16_instrument *inst;         /* current instrument, NULL = silent       */
    sfz16_voice_state voices[SFZ16_MAX_VOICES];
    uint8_t           cc[128];      /* CC state survives instrument swaps     */
    float             host_sample_rate;
} sfz16_engine;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Reset an instrument to empty. Call once before sfz16_parse. */
void sfz16_init(sfz16_instrument *inst, float host_sample_rate);

/* Parse SFZ text into regions AND the sample table: every distinct sample
 * path becomes one sfz16_sample entry with data=NULL, which the host fills
 * (decode the WAV to int16 into PSRAM, set data/frames/channels/sample_rate)
 * before sfz16_link. Returns 0, or -1 with a refusal in `err` (too many
 * regions, too many distinct samples, path too long, more CC conditions
 * than the sparse table holds). Never truncates in silence. `rep` may be
 * NULL. */
int sfz16_parse(sfz16_instrument *inst, const char *text, size_t len,
                sfz16_report *rep, char *err, size_t err_len);

/* Refuses (naming it in `err`) if a sample still has no data — a missing
 * file on the card, not silence. */
int sfz16_link(sfz16_instrument *inst, char *err, size_t err_len);

/* Engine. Kept apart from the instrument: voices are the audio path, they
 * live in internal RAM and persist across instrument swaps. */
void sfz16_engine_init(sfz16_engine *eng, float host_sample_rate);

/* Swap instruments (NULL = silent). Sounding voices are fast-released (3 ms)
 * against the OLD instrument: the caller keeps it alive until the voices
 * drained (query sfz16_active_voices(old) == 0, a few ms), as play-list
 * keeps a stream for the fading clip. */
void sfz16_set_instrument(sfz16_engine *eng, sfz16_instrument *inst);

void sfz16_note_on(sfz16_engine *eng, uint8_t note, uint8_t velocity);
void sfz16_note_off(sfz16_engine *eng, uint8_t note);
void sfz16_all_notes_off(sfz16_engine *eng);
void sfz16_cc(sfz16_engine *eng, uint8_t cc, uint8_t value);
int  sfz16_active_voices(const sfz16_engine *eng);

/* Does the host have to call sfz16_render? True with an instrument, and ALSO
 * with none while voices are still sounding: set_instrument only starts their
 * fast release, a voice dies by being rendered (render never reads eng->inst).
 * A host that skips the render on a NULL instrument freezes them. */
int  sfz16_needs_render(const sfz16_engine *eng);

/* Render `frames` of stereo audio, summing every active voice into out_l and
 * out_r (float, as the reference core — the firmware converts at its bus).
 * Buffers are NOT cleared: the caller zeroes them first. */
void sfz16_render(sfz16_engine *eng, float *out_l, float *out_r, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* SFZ16_H */
