/**
 * sfz16.c — SFZ kernel for the board: parse + voices, pure C99.
 *
 * Same behaviour as the reference core (sfz_parse.c / sfz_engine.c, kept for
 * the browser prototype and the parity test), with the decisions of
 * MESURES §206: int16 samples, split fixed-point playhead (integer frame
 * index + 32-bit phase accumulator, 32-bit arithmetic only in the per-sample
 * loop), float envelope and gains. Two deliberate fixes:
 *   - <global> inherits into <region> even with no <group> in between;
 *   - limits REFUSE with a message instead of truncating in silence.
 *
 * No allocation anywhere: everything lives in caller-owned blocks. The one
 * place sample data is read is the inline read in sfz16_render — the seam
 * where heads + streaming will plug in.
 */

#include "sfz16.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>

/* The bench measured this kernel at -O3 (MESURES 206); it was never proven
 * at -Os, and the whole firmware stays at its long-proven -Os (platform.txt:35)
 * — the pragma keeps just this file where it was measured. */
#pragma GCC optimize ("O3")

/* ── small string helpers (no allocation) ────────────────────────────────── */

static int s16_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0)  return 0;
    }
    return 0;
}

static void s16_strlcpy(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* note name → MIDI number, as the reference parser ("60" or "c4"/"f#3"). */
static int s16_note_to_midi(const char *s) {
    int all_digit = 1;
    for (const char *p = s; *p; p++) {
        if (p == s && (*p == '-' || *p == '+')) continue;
        if (!isdigit((unsigned char)*p)) { all_digit = 0; break; }
    }
    if (all_digit) return atoi(s);

    char note = (char)tolower((unsigned char)s[0]);
    const int base_tab[7] = { 9, 11, 0, 2, 4, 5, 7 }; /* a..g */
    if (note < 'a' || note > 'g') return atoi(s) ? atoi(s) : 60;
    int base = base_tab[note - 'a'];

    int idx = 1, acc = 0;
    if (s[idx] == '#') { acc = 1; idx++; }
    else if (s[idx] == 'b') { acc = -1; idx++; }

    int oct = atoi(&s[idx]);
    int midi = (oct + 1) * 12 + base + acc;
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    return midi;
}

/* ── region defaults — same values as the reference parser ───────────────── */

static void region_set_defaults(sfz16_region *r) {
    memset(r, 0, sizeof(*r));
    r->hikey = 127;
    r->hivel = 127;
    r->pitch_keycenter = 60;
    r->pitch_keytrack = 100.0f;
    r->amp_veltrack = 100.0f;
    r->ampeg_attack = 0.001f;
    r->ampeg_sustain = 1.0f;
    r->ampeg_release = 0.05f;
    r->loop_start = -1;
    r->loop_end = -1;
    r->end = -1;
    r->seq_length = 1;
    r->seq_position = 1;
    r->sample_index = -1;
    for (int i = 0; i < SFZ16_MAX_CC_CONDS; i++) {
        r->cc[i] = 0; r->cc_lo[i] = 0; r->cc_hi[i] = 127; /* no constraint */
    }
}

/* ── sample table: distinct paths, registered as they are named ──────────── */

static int sample_index_of(const sfz16_instrument *inst, const char *path) {
    for (int i = 0; i < inst->sample_count; i++)
        if (!strcmp(inst->samples[i].path, path)) return i;
    return -1;
}

static int register_sample(sfz16_instrument *inst, const char *path,
                           char *err, size_t err_len) {
    int idx = sample_index_of(inst, path);
    if (idx >= 0) return idx;                       /* deduplicated */
    if (inst->sample_count >= SFZ16_MAX_SAMPLES) {
        snprintf(err, err_len, "trop de samples distincts (%d, max %d)",
                 inst->sample_count + 1, SFZ16_MAX_SAMPLES);
        return -1;
    }
    sfz16_sample *s = &inst->samples[inst->sample_count];
    s16_strlcpy(s->path, path, SFZ16_PATH_LEN);
    s->data = NULL;                                 /* the host fills it */
    s->frames = 0;
    s->channels = 0;
    s->sample_rate = 0.0f;
    return inst->sample_count++;
}

/* ── opcode application — same conversions as the reference parser ───────── */

/* Returns -1 only for a refusal (CC table full); unknown opcodes are counted
 * in `rep`, never fatal. */
static int apply_opcode(sfz16_instrument *inst, sfz16_region *t,
                        const char *key, const char *val,
                        const char *default_path,
                        sfz16_report *rep, char *err, size_t err_len) {
    char path[SFZ16_PATH_LEN];

    if (!strcmp(key, "sample")) {
        if (default_path[0] && val[0] != '/')
            snprintf(path, sizeof(path), "%s%s", default_path, val);
        else
            s16_strlcpy(path, val, sizeof(path));
        for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
        if (strlen(path) >= SFZ16_PATH_LEN) {
            snprintf(err, err_len, "chemin de sample trop long : %s", val);
            return -1;
        }
        int idx = register_sample(inst, path, err, err_len);
        if (idx < 0) return -1;
        t->sample_index = (int16_t)idx;
    } else if (!strcmp(key, "lokey")) {
        t->lokey = (uint8_t)s16_note_to_midi(val);
    } else if (!strcmp(key, "hikey")) {
        t->hikey = (uint8_t)s16_note_to_midi(val);
    } else if (!strcmp(key, "key")) {
        uint8_t k = (uint8_t)s16_note_to_midi(val);
        t->lokey = t->hikey = t->pitch_keycenter = k;
    } else if (!strcmp(key, "pitch_keycenter")) {
        t->pitch_keycenter = (uint8_t)s16_note_to_midi(val);
    } else if (!strcmp(key, "lovel")) {
        t->lovel = (uint8_t)atoi(val);
    } else if (!strcmp(key, "hivel")) {
        t->hivel = (uint8_t)atoi(val);
    } else if (!strcmp(key, "tune")) {
        t->tune = (int16_t)atoi(val);
    } else if (!strcmp(key, "volume")) {
        t->volume_db = (float)atof(val);
    } else if (!strcmp(key, "amp_veltrack")) {
        t->amp_veltrack = (float)atof(val);
    } else if (!strcmp(key, "ampeg_attack")) {
        t->ampeg_attack = (float)atof(val);
    } else if (!strcmp(key, "ampeg_release")) {
        t->ampeg_release = (float)atof(val);
    } else if (!strcmp(key, "ampeg_delay")) {
        t->ampeg_delay = (float)atof(val);
    } else if (!strcmp(key, "ampeg_hold")) {
        t->ampeg_hold = (float)atof(val);
    } else if (!strcmp(key, "ampeg_decay")) {
        t->ampeg_decay = (float)atof(val);
    } else if (!strcmp(key, "ampeg_sustain")) {
        t->ampeg_sustain = (float)atof(val) / 100.0f;
    } else if (!strcmp(key, "pan")) {
        t->pan = (float)atof(val);
    } else if (!strcmp(key, "transpose")) {
        t->transpose = (int8_t)atoi(val);
    } else if (!strcmp(key, "pitch_keytrack")) {
        t->pitch_keytrack = (float)atof(val);
    } else if (!strcmp(key, "loop_start") || !strcmp(key, "loopstart")) {
        t->loop_start = atoi(val);
    } else if (!strcmp(key, "loop_end") || !strcmp(key, "loopend")) {
        t->loop_end = atoi(val);
    } else if (!strcmp(key, "offset")) {
        t->offset = atoi(val);
    } else if (!strcmp(key, "end")) {
        t->end = atoi(val);
    } else if (!strcmp(key, "group")) {
        t->group = (int16_t)atoi(val);
    } else if (!strcmp(key, "off_by") || !strcmp(key, "offby")) {
        t->off_by = (int16_t)atoi(val);
    } else if (!strcmp(key, "seq_length")) {
        int v = atoi(val); if (v < 1) v = 1; if (v > 128) v = 128;
        t->seq_length = (uint8_t)v;
    } else if (!strcmp(key, "seq_position")) {
        int v = atoi(val); if (v < 1) v = 1; if (v > 128) v = 128;
        t->seq_position = (uint8_t)v;
    } else if (!strcmp(key, "rand_lo") || !strcmp(key, "lorand")) {
        t->rand_lo = (float)atof(val);
    } else if (!strcmp(key, "rand_hi") || !strcmp(key, "hirand")) {
        t->rand_hi = (float)atof(val);
    } else if (!strcmp(key, "loop_mode")) {
        if      (!strcmp(val, "no_loop"))         t->loop_mode = SFZ16_LOOP_NONE;
        else if (!strcmp(val, "one_shot"))        t->loop_mode = SFZ16_LOOP_ONESHOT;
        else if (!strcmp(val, "loop_continuous")) t->loop_mode = SFZ16_LOOP_CONTINUOUS;
        else if (!strcmp(val, "loop_sustain"))    t->loop_mode = SFZ16_LOOP_SUSTAIN;
    } else if (!s16_strncasecmp(key, "locc", 4) && isdigit((unsigned char)key[4])) {
        int cc = atoi(key + 4);
        if (cc >= 0 && cc < 128) {
            int v = atoi(val); if (v < 0) v = 0; if (v > 127) v = 127;
            /* find or create the entry for this controller, tightening it */
            int slot = -1;
            for (int i = 0; i < SFZ16_MAX_CC_CONDS; i++)
                if (t->cc_count > i && t->cc[i] == (uint8_t)cc) { slot = i; break; }
            if (slot < 0) {
                if (t->cc_count >= SFZ16_MAX_CC_CONDS) {
                    snprintf(err, err_len,
                             "trop de controleurs locc/hicc distincts (max %d)",
                             SFZ16_MAX_CC_CONDS);
                    return -1;
                }
                slot = t->cc_count++;
                t->cc[slot] = (uint8_t)cc;
                t->cc_lo[slot] = 0;
                t->cc_hi[slot] = 127;
            }
            if ((uint8_t)v > t->cc_lo[slot]) t->cc_lo[slot] = (uint8_t)v;
        }
    } else if (!s16_strncasecmp(key, "hicc", 4) && isdigit((unsigned char)key[4])) {
        int cc = atoi(key + 4);
        if (cc >= 0 && cc < 128) {
            int v = atoi(val); if (v < 0) v = 0; if (v > 127) v = 127;
            int slot = -1;
            for (int i = 0; i < SFZ16_MAX_CC_CONDS; i++)
                if (t->cc_count > i && t->cc[i] == (uint8_t)cc) { slot = i; break; }
            if (slot < 0) {
                if (t->cc_count >= SFZ16_MAX_CC_CONDS) {
                    snprintf(err, err_len,
                             "trop de controleurs locc/hicc distincts (max %d)",
                             SFZ16_MAX_CC_CONDS);
                    return -1;
                }
                slot = t->cc_count++;
                t->cc[slot] = (uint8_t)cc;
                t->cc_lo[slot] = 0;
                t->cc_hi[slot] = 127;
            }
            if ((uint8_t)v < t->cc_hi[slot]) t->cc_hi[slot] = (uint8_t)v;
        }
    } else if (strcmp(key, "default_path") != 0) {
        /* unknown opcode: counted and named, never fatal — a filter the core
         * does not implement must be VISIBLE, not silently dropped */
        if (rep) {
            rep->unknown_opcodes++;
            if (rep->first_unknown[0] == 0)
                s16_strlcpy(rep->first_unknown, key, sizeof(rep->first_unknown));
        }
    }
    return 0;
}

/* ── main parse — line scanner of the reference, with the <global> fix ────── */

typedef enum { S16_NONE, S16_GLOBAL, S16_CONTROL, S16_GROUP, S16_REGION } s16_section;

int sfz16_parse(sfz16_instrument *inst, const char *text, size_t len,
                sfz16_report *rep, char *err, size_t err_len) {
    if (!inst || !text) return -1;
    inst->region_count = 0;
    inst->sample_count = 0;
    if (rep) { rep->regions = 0; rep->unknown_opcodes = 0; rep->first_unknown[0] = 0; }
    if (err && err_len) err[0] = 0;

    sfz16_region group_defaults, global_defaults, current;
    region_set_defaults(&group_defaults);
    region_set_defaults(&global_defaults);
    region_set_defaults(&current);
    s16_section section = S16_NONE;
    char default_path[SFZ16_PATH_LEN]; default_path[0] = 0;

    size_t i = 0;
    char line[512];

    while (i < len) {
        size_t l = 0;
        while (i < len && text[i] != '\n' && l < sizeof(line) - 1) line[l++] = text[i++];
        if (i < len && text[i] == '\n') i++;
        line[l] = 0;

        char *cmt = strstr(line, "//");
        if (cmt) *cmt = 0;

        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) continue;

        while (*p) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;

            if (*p == '<') {
                char hdr[16]; int hl = 0;
                p++;
                while (*p && *p != '>' && hl < 15) hdr[hl++] = *p++;
                hdr[hl] = 0;
                if (*p == '>') p++;

                if (section == S16_REGION) {
                    if (inst->region_count >= SFZ16_MAX_REGIONS) {
                        snprintf(err, err_len, "trop de regions (%d, max %d)",
                                 SFZ16_MAX_REGIONS + 1, SFZ16_MAX_REGIONS);
                        return -1;
                    }
                    inst->regions[inst->region_count++] = current;
                }

                if (!strcmp(hdr, "global")) {
                    region_set_defaults(&global_defaults);
                    /* THE FIX (seen by the bench, MESURES §206): regions
                     * inherit through group_defaults, which only met
                     * global_defaults at a <group> header — a <global> with
                     * no <group> was lost. Keep the group layer in sync
                     * while the global opcodes accumulate. */
                    section = S16_GLOBAL;
                    current = global_defaults;
                } else if (!strcmp(hdr, "control")) {
                    section = S16_CONTROL;
                } else if (!strcmp(hdr, "group")) {
                    group_defaults = global_defaults;
                    section = S16_GROUP;
                    current = group_defaults;
                } else if (!strcmp(hdr, "region")) {
                    current = group_defaults;
                    section = S16_REGION;
                }
                continue;
            }

            char key[64]; int kl = 0;
            while (*p && *p != '=' && !isspace((unsigned char)*p) && kl < 63)
                key[kl++] = (char)tolower((unsigned char)*p++);
            key[kl] = 0;
            if (*p != '=') { while (*p && !isspace((unsigned char)*p)) p++; continue; }
            p++;

            char val[SFZ16_PATH_LEN]; int vl = 0;
            const int path_like = (!strcmp(key, "sample") || !strcmp(key, "default_path"));
            if (path_like) {
                while (*p) {
                    if (isspace((unsigned char)*p)) {
                        const char *q = p;
                        while (*q && isspace((unsigned char)*q)) q++;
                        const char *w = q;
                        while (*w && !isspace((unsigned char)*w) && *w != '=') w++;
                        if (*w == '=') break;
                    }
                    if (vl < SFZ16_PATH_LEN - 1) val[vl++] = *p;
                    p++;
                }
                while (vl > 0 && isspace((unsigned char)val[vl-1])) vl--;
            } else {
                while (*p && !isspace((unsigned char)*p) && vl < SFZ16_PATH_LEN - 1)
                    val[vl++] = *p++;
            }
            val[vl] = 0;

            if (!strcmp(key, "default_path")) {
                s16_strlcpy(default_path, val, SFZ16_PATH_LEN);
                for (char *q = default_path; *q; q++) if (*q == '\\') *q = '/';
                continue;
            }
            if (apply_opcode(inst, &current, key, val, default_path,
                             rep, err, err_len) < 0)
                return -1;

            if (section == S16_GLOBAL) {
                global_defaults = current;
                group_defaults = current;    /* the fix, live */
            }
            else if (section == S16_GROUP) group_defaults = current;
        }
    }

    if (section == S16_REGION) {
        if (inst->region_count >= SFZ16_MAX_REGIONS) {
            snprintf(err, err_len, "trop de regions (%d, max %d)",
                     SFZ16_MAX_REGIONS + 1, SFZ16_MAX_REGIONS);
            return -1;
        }
        inst->regions[inst->region_count++] = current;
    }
    if (rep) rep->regions = inst->region_count;
    return 0;
}

int sfz16_link(sfz16_instrument *inst, char *err, size_t err_len) {
    for (int r = 0; r < inst->region_count; r++) {
        const sfz16_region *reg = &inst->regions[r];
        if (reg->sample_index < 0) continue;         /* sampleless zone */
        const sfz16_sample *s = &inst->samples[reg->sample_index];
        if (!s->data || s->frames == 0) {
            snprintf(err, err_len, "sample manquant : %s", s->path);
            return -1;
        }
    }
    return 0;
}

/* ── engine ───────────────────────────────────────────────────────────────── */

void sfz16_init(sfz16_instrument *inst, float host_sample_rate) {
    memset(inst, 0, sizeof(*inst));
    inst->host_sample_rate = host_sample_rate > 0 ? host_sample_rate : 48000.0f;
}

void sfz16_engine_init(sfz16_engine *eng, float host_sample_rate) {
    memset(eng, 0, sizeof(*eng));
    eng->host_sample_rate = host_sample_rate > 0 ? host_sample_rate : 48000.0f;
}

void sfz16_set_instrument(sfz16_engine *eng, sfz16_instrument *inst) {
    /* sounding voices fast-release against the OLD instrument: the caller
     * keeps it alive until they drained (a few ms) */
    for (int i = 0; i < SFZ16_MAX_VOICES; i++) {
        sfz16_voice_state *v = &eng->voices[i];
        if (!v->active) continue;
        v->held = 0;
        v->releasing = 1;
        v->env_stage = 5;
        const float rel = 0.003f;
        v->env_inc = -v->env / (rel * eng->host_sample_rate);
    }
    eng->inst = inst;
}

void sfz16_cc(sfz16_engine *eng, uint8_t cc, uint8_t value) {
    eng->cc[cc & 127] = value;
}

int sfz16_active_voices(const sfz16_engine *eng) {
    int n = 0;
    for (int i = 0; i < SFZ16_MAX_VOICES; i++)
        if (eng->voices[i].active) n++;
    return n;
}

int sfz16_needs_render(const sfz16_engine *eng) {
    return eng->inst != NULL || sfz16_active_voices(eng) > 0;
}

/* ── region matching — sparse CC entries instead of the 128-slot walk ────── */

static int region_matches(const sfz16_engine *eng, const sfz16_region *r,
                          uint8_t note, uint8_t vel) {
    if (note < r->lokey || note > r->hikey) return 0;
    if (vel  < r->lovel || vel  > r->hivel) return 0;
    for (int i = 0; i < r->cc_count; i++) {
        const uint8_t v = eng->cc[r->cc[i]];
        if (v < r->cc_lo[i] || v > r->cc_hi[i]) return 0;
    }
    return 1;
}

static sfz16_voice_state *alloc_voice(sfz16_engine *eng) {
    for (int i = 0; i < SFZ16_MAX_VOICES; i++)
        if (!eng->voices[i].active) return &eng->voices[i];
    for (int i = 0; i < SFZ16_MAX_VOICES; i++)
        if (eng->voices[i].releasing) return &eng->voices[i];
    return &eng->voices[0];
}

static int region_seq_ok(sfz16_instrument *inst, const sfz16_region *r, uint8_t note) {
    if (r->seq_length > 1) {
        int pos = (inst->seq_counter[note] % r->seq_length) + 1;
        if (pos != r->seq_position) return 0;
    }
    if (r->rand_hi > r->rand_lo) {
        float roll = (float)rand() / (float)RAND_MAX;
        if (roll < r->rand_lo || roll >= r->rand_hi) return 0;
    }
    return 1;
}

enum { S16_ENV_DELAY = 0, S16_ENV_ATTACK, S16_ENV_HOLD, S16_ENV_DECAY,
       S16_ENV_SUSTAIN, S16_ENV_RELEASE };

void sfz16_note_on(sfz16_engine *eng, uint8_t note, uint8_t velocity) {
    sfz16_instrument *inst = eng->inst;
    if (!inst) return;

    /* retrigger: quick-release existing held voices on the same note */
    for (int i = 0; i < SFZ16_MAX_VOICES; i++) {
        sfz16_voice_state *v = &eng->voices[i];
        if (v->active && v->note == note && v->held) {
            v->held = 0;
            v->releasing = 1;
            v->env_stage = S16_ENV_RELEASE;
            const float rel = 0.005f;
            v->env_inc = -v->env / (rel * eng->host_sample_rate);
        }
    }

    int any_fired = 0;
    for (int r = 0; r < inst->region_count; r++) {
        const sfz16_region *reg = &inst->regions[r];
        if (!region_matches(eng, reg, note, velocity)) continue;
        if (reg->sample_index < 0) continue;
        if (!region_seq_ok(inst, reg, note)) continue;
        const sfz16_sample *smp = &inst->samples[reg->sample_index];
        if (!smp->data || smp->frames == 0) continue;

        /* exclusion groups */
        if (reg->group != 0) {
            for (int i = 0; i < SFZ16_MAX_VOICES; i++) {
                sfz16_voice_state *ov = &eng->voices[i];
                if (ov->active && ov->region && ov->region->off_by == reg->group) {
                    ov->held = 0;
                    ov->releasing = 1;
                    ov->env_stage = S16_ENV_RELEASE;
                    const float rel = 0.003f;
                    ov->env_inc = -ov->env / (rel * eng->host_sample_rate);
                }
            }
        }

        sfz16_voice_state *v = alloc_voice(eng);
        v->active = 1;
        v->note = note;
        v->velocity = velocity;
        v->releasing = 0;
        v->held = 1;
        v->region = reg;
        v->sample = smp;

        uint32_t last = smp->frames > 0 ? smp->frames - 1 : 0;
        v->play_end = (reg->end >= 0 && (uint32_t)reg->end <= last)
                      ? (uint32_t)reg->end : last;
        v->ipos = (reg->offset > 0 && (uint32_t)reg->offset <= last)
                  ? (uint32_t)reg->offset : 0;
        v->fpos = 0;
        v->loop_start = (reg->loop_start >= 0) ? (uint32_t)reg->loop_start : 0;
        v->loop_end   = (reg->loop_end >= 0 && (uint32_t)reg->loop_end <= last)
                        ? (uint32_t)reg->loop_end : last;
        if (v->loop_end <= v->loop_start) { v->loop_start = 0; v->loop_end = last; }

        /* pitch ratio — the same double math as the reference core (note_on
         * is rare: 50 us measured over 256 regions), decomposed once into
         * the 32-bit integer/fractional advance pair */
        double kt = (double)reg->pitch_keytrack / 100.0;
        double semis = ((double)note - (double)reg->pitch_keycenter) * kt
                     + (double)reg->transpose
                     + (double)reg->tune / 100.0;
        double pitch_ratio = pow(2.0, semis / 12.0);
        double sr_ratio = (double)smp->sample_rate / (double)eng->host_sample_rate;
        double inc = pitch_ratio * sr_ratio;
        if (inc < 0.0) inc = 0.0;
        if (inc > 1.0e9) inc = 1.0e9;                /* absurd pitch: clamp */
        v->inc_int = (uint32_t)inc;
        v->inc_frac = (uint32_t)((inc - (double)v->inc_int) * 4294967296.0 + 0.5);

        /* amplitude: velocity tracking × volume(dB) — same formulas */
        float veltrack = reg->amp_veltrack;
        float vel_gain = 1.0f - (veltrack / 100.0f) * (1.0f - (float)velocity / 127.0f);
        float vol_lin  = powf(10.0f, reg->volume_db / 20.0f);
        v->amp = vel_gain * vol_lin;

        float p = reg->pan; if (p < -100.0f) p = -100.0f; if (p > 100.0f) p = 100.0f;
        float theta = (p + 100.0f) / 200.0f * 1.57079633f;
        v->gain_l = cosf(theta);
        v->gain_r = sinf(theta);

        /* DAHDSR init */
        v->sustain_level = reg->ampeg_sustain;
        v->env = 0.0f;
        const float sr = eng->host_sample_rate;
        if (reg->ampeg_delay > 0.0f) {
            v->env_stage = S16_ENV_DELAY;
            v->stage_samples = (uint32_t)(reg->ampeg_delay * sr);
            v->env_inc = 0.0f;
        } else {
            v->env_stage = S16_ENV_ATTACK;
            float atk = reg->ampeg_attack < 0.0001f ? 0.0001f : reg->ampeg_attack;
            v->env_inc = 1.0f / (atk * sr);
        }
        any_fired = 1;
    }
    if (any_fired || inst->seq_counter[note]) inst->seq_counter[note]++;
}

void sfz16_note_off(sfz16_engine *eng, uint8_t note) {
    for (int i = 0; i < SFZ16_MAX_VOICES; i++) {
        sfz16_voice_state *v = &eng->voices[i];
        if (!v->active || v->note != note || !v->held) continue;
        v->held = 0;
        if (v->region->loop_mode == SFZ16_LOOP_ONESHOT) continue;
        v->releasing = 1;
        v->env_stage = S16_ENV_RELEASE;
        float rel = v->region->ampeg_release < 0.0001f ? 0.0001f
                                                        : v->region->ampeg_release;
        v->env_inc = -v->env / (rel * eng->host_sample_rate);
    }
}

void sfz16_all_notes_off(sfz16_engine *eng) {
    for (int i = 0; i < SFZ16_MAX_VOICES; i++) {
        sfz16_voice_state *v = &eng->voices[i];
        if (!v->active) continue;
        v->held = 0;
        v->releasing = 1;
        v->env_stage = S16_ENV_RELEASE;
        const float rel = 0.005f;
        v->env_inc = -v->env / (rel * eng->host_sample_rate);
    }
}

/* ── envelope — the reference env_step, unchanged (parity) ───────────────── */

static inline void env_step16(sfz16_voice_state *v, float sr) {
    switch (v->env_stage) {
        case S16_ENV_DELAY:
            if (v->stage_samples > 0) { v->stage_samples--; }
            if (v->stage_samples == 0) {
                v->env_stage = S16_ENV_ATTACK;
                const sfz16_region *reg = v->region;
                float atk = reg->ampeg_attack < 0.0001f ? 0.0001f : reg->ampeg_attack;
                v->env_inc = 1.0f / (atk * sr);
            }
            break;
        case S16_ENV_ATTACK:
            v->env += v->env_inc;
            if (v->env >= 1.0f) {
                v->env = 1.0f;
                const sfz16_region *reg = v->region;
                if (reg->ampeg_hold > 0.0f) {
                    v->env_stage = S16_ENV_HOLD;
                    v->stage_samples = (uint32_t)(reg->ampeg_hold * sr);
                } else {
                    goto enter_decay;
                }
            }
            break;
        case S16_ENV_HOLD:
            if (v->stage_samples > 0) v->stage_samples--;
            if (v->stage_samples == 0) {
              enter_decay: {
                const sfz16_region *reg = v->region;
                if (reg->ampeg_decay > 0.0f && v->sustain_level < 1.0f) {
                    v->env_stage = S16_ENV_DECAY;
                    v->env_inc = -(1.0f - v->sustain_level) / (reg->ampeg_decay * sr);
                } else {
                    v->env_stage = S16_ENV_SUSTAIN;
                    v->env = v->sustain_level;
                }
              }
            }
            break;
        case S16_ENV_DECAY:
            v->env += v->env_inc;
            if (v->env <= v->sustain_level) {
                v->env = v->sustain_level;
                v->env_stage = S16_ENV_SUSTAIN;
            }
            break;
        case S16_ENV_SUSTAIN:
            break;
        case S16_ENV_RELEASE:
            v->env += v->env_inc;
            if (v->env <= 0.0f) { v->env = 0.0f; v->active = 0; }
            break;
    }
}

/* ── render — the measured loop: int16 reads, split fixed-point playhead ─── */

void sfz16_render(sfz16_engine *eng, float *out_l, float *out_r, uint32_t frames) {
    const float sr = eng->host_sample_rate;
    for (int vi = 0; vi < SFZ16_MAX_VOICES; vi++) {
        sfz16_voice_state *v = &eng->voices[vi];
        if (!v->active) continue;
        const sfz16_sample *s = v->sample;
        const sfz16_region *reg = v->region;
        if (!s || !s->data || !reg) { v->active = 0; continue; }

        const int looping = (reg->loop_mode == SFZ16_LOOP_CONTINUOUS ||
                             reg->loop_mode == SFZ16_LOOP_SUSTAIN);
        const int16_t *d = s->data;
        const uint32_t frames_s = s->frames;
        const uint32_t stride = s->channels;
        uint32_t ipos = v->ipos, fpos = v->fpos;
        const uint32_t inc_int = v->inc_int, inc_frac = v->inc_frac;
        const uint32_t ls = v->loop_start, le = v->loop_end, pe = v->play_end;

        for (uint32_t n = 0; n < frames; n++) {
            env_step16(v, sr);
            if (!v->active) break;

            /* THE sample-data seam (heads + streaming plug here) */
            uint32_t i1 = ipos + 1;
            if (i1 >= frames_s) i1 = ipos;
            const int32_t w = (int32_t)(fpos >> 17);       /* 15-bit weight */
            float l, r;
            if (stride == 1) {
                const int32_t a = d[ipos], b = d[i1];
                const float x = (float)(a + ((w * (b - a)) >> 15))
                                * (1.0f / 32768.0f);
                l = x; r = x;
            } else {
                const int32_t al = d[ipos * 2], ar = d[ipos * 2 + 1];
                const int32_t bl = d[i1 * 2],    br = d[i1 * 2 + 1];
                l = (float)(al + ((w * (bl - al)) >> 15)) * (1.0f / 32768.0f);
                r = (float)(ar + ((w * (br - ar)) >> 15)) * (1.0f / 32768.0f);
            }

            const float g = v->env * v->amp;
            out_l[n] += l * g * v->gain_l;
            out_r[n] += r * g * v->gain_r;

            /* advance: 32-bit integer part + carry from the 2^-32 fraction */
            fpos += inc_frac;
            ipos += inc_int + (fpos < inc_frac ? 1u : 0u);

            if (looping && v->held && ipos >= le) {
                ipos -= (le - ls);                        /* fraction kept */
            } else if (ipos >= pe) {
                v->active = 0;
                break;
            }
        }
        v->ipos = ipos;
        v->fpos = fpos;
    }
}
