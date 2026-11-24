/* SPDX-License-Identifier: GPL-3.0-only
 * Polycercle audio engine: a sample-accurate circular sequencer (VIBE FM).
 *
 * One cycle (one turn of the hand) is shared by every circle. A circle of n
 * steps places its hits at k/n of the cycle, so 3 and 2 steps play 3:2.
 * The phase is a count of cycles and a 64-bit fraction of the cycle (the tempo
 * holds to a billionth over hours); each audio block finds the step boundaries
 * it contains and triggers the voice at the exact frame.
 *
 * Runs in the audio flow (VIBE_PARALLEL_AUDIO): it owns the voices, the phase
 * and all of the MIDI out. The control flow talks to it through a published
 * state (double buffer + counter), a command queue (one writer, one reader)
 * and reads a status snapshot (a counter around it) – the two-flow rules of
 * the VIBE FM Protocol (6.2).
 *
 * While a DAW plays (or a MIDI clock runs on the VIBE and in the browser) the
 * circles follow its song position: a jump or a loop moves the hand there at
 * once, small differences are pulled in smoothly so nothing doubles or drops. */
#include "poly.h"

float pr_sr = 44100.0f;
volatile float pr_volume = 1.0f;

enum { MAXB = VIBE_MAX_BLOCK, CMD_SIZE = 64, NOTES_OUT = 48 };
enum { CMD_PLAY = 1, CMD_STOP, CMD_AUDITION, CMD_PANIC, CMD_SONG_MODE, CMD_CLOCK, CMD_REC_ARM, CMD_REC_STOP, CMD_REC_RELEASE };
struct command { uint8_t type, a, b, c; int8_t d; };
struct event { uint16_t offset; uint8_t ring, step, velocity, pattern; int8_t pitch; uint8_t has_pitch; };

/* Publication: the control flow fills the idle buffer, then advances the counter; the audio adopts it
 * at the next block and says so (adopted_seq) – only then may the other buffer be filled again. */
static struct pr_state publication[2];
static volatile uint32_t published_seq, adopted_seq;
static uint32_t current_seq;
static struct command commands[CMD_SIZE];
static volatile uint32_t command_write, command_read;

#define KIT(i) pd_kit_get(i)
static float mix_left[MAXB], mix_right[MAXB], ring_buffer[MAXB];
static float pan_left[128], pan_right[128];
static struct event events[PR_EVENTS];

static uint32_t phase_hi;                /* whole cycles */
static uint64_t phase_lo;                /* fraction of the cycle, 2^64 = one cycle */
static uint8_t playing, song_mode, finished, following, want_song, clock_on = 1;
/* A cycle boundary fell inside the last frame computed: the next segment (in this block or the next one)
 * catches the hits at the very start of the cycle, which lie just behind its first frame. */
static uint8_t crossed;
static volatile uint32_t status_seq;
static struct pr_status status;
static uint8_t initialised;
static uint16_t ring_hits[PR_RINGS];
static uint8_t ring_last[PR_RINGS];
static struct { uint8_t on, ch, note; uint32_t left; } notes_out[NOTES_OUT];

/* ---------- control flow API ---------- */
static void push(uint8_t type, uint8_t a, uint8_t b, uint8_t c, int8_t d)
{
    uint32_t w = command_write;
    if (w - command_read >= CMD_SIZE) return;          /* full: drop the request */
    struct command *m = &commands[w % CMD_SIZE];
    m->type = type; m->a = a; m->b = b; m->c = c; m->d = d;
    VIBE_RELEASE();
    command_write = w + 1u;
}
int pr_publish(const struct pr_state *state)
{
    uint32_t seq = published_seq;
    if (adopted_seq != seq) return 1;
    VIBE_ACQUIRE();
    publication[(seq + 1u) & 1u] = *state;
    VIBE_RELEASE();
    published_seq = seq + 1u;
    return 0;
}
void pr_play(unsigned song, unsigned from_block) { push(CMD_PLAY, (uint8_t)(song != 0), (uint8_t)from_block, 0, 0); }
void pr_stop(void) { push(CMD_STOP, 0, 0, 0, 0); }
void pr_song_mode(unsigned song) { push(CMD_SONG_MODE, (uint8_t)(song != 0), 0, 0, 0); }
void pr_clock(unsigned on) { push(CMD_CLOCK, (uint8_t)(on != 0), 0, 0, 0); }
void pr_panic(void) { push(CMD_PANIC, 0, 0, 0, 0); }
void pr_rec_arm(unsigned slot) { push(CMD_REC_ARM, (uint8_t)(slot % SMP_SLOTS), 0, 0, 0); }
void pr_rec_stop(void) { push(CMD_REC_STOP, 0, 0, 0, 0); }
void pr_rec_release(void) { push(CMD_REC_RELEASE, 0, 0, 0, 0); }
void pr_audition(unsigned ring, unsigned velocity) { push(CMD_AUDITION, (uint8_t)ring, (uint8_t)(velocity > 127 ? 127 : velocity), 0, 0); }
void pr_audition_step(unsigned ring, unsigned step, unsigned velocity)
{
    push(CMD_AUDITION, (uint8_t)ring, (uint8_t)(velocity > 127 ? 127 : velocity), (uint8_t)(step < PR_STEPS ? step + 1 : 0), 0);
}
void pr_audition_note(unsigned ring, int semitones, unsigned velocity)
{
    semitones = semitones < -48 ? -48 : semitones > 48 ? 48 : semitones;
    push(CMD_AUDITION, (uint8_t)ring, (uint8_t)(velocity > 127 ? 127 : velocity), 255, (int8_t)semitones);
}
void pr_status_read(struct pr_status *out)
{
    for (unsigned tries = 0; tries < 16; ++tries) {
        uint32_t before = status_seq;
        VIBE_ACQUIRE();
        *out = status;
        VIBE_ACQUIRE();
        if (!(before & 1u) && before == status_seq) return;
    }
}
void pr_rate(float sr)
{
    if (!(sr >= 8000.0f && sr <= 384000.0f)) sr = 44100.0f;
    pr_sr = sr;
    pd_rate(sr);
    ps_rate(sr);
}
void pr_engine_init(void)
{
    if (initialised) return;
    for (unsigned i = 0; i < PR_RINGS; ++i) pd_kit_init(KIT(i));
    ps_init();
    ps_tables();
    ss_init();
    /* Constant-power pan law, 0..127 with 64 at the centre (both 0.707). */
    for (unsigned p = 0; p < 128; ++p) {
        float x = (float)p / 127.0f;
        float a = x * 1.5707963f, q = a * a;
        float s = a * (1 - q / 6 * (1 - q / 20 * (1 - q / 42)));
        float b = 1.5707963f - a, r = b * b;
        float c = b * (1 - r / 6 * (1 - r / 20 * (1 - r / 42)));
        pan_left[p] = c; pan_right[p] = s;
    }
    status.scene = 255;
    VIBE_RELEASE();
    initialised = 1;
}

/* ---------- audio flow ---------- */
static inline unsigned beats_of(const struct pr_pattern *p) { return p->beats && p->beats <= 16 ? p->beats : 4; }
static inline const struct pr_pattern *pattern_at(const struct pr_state *st, uint32_t cycle,
                                                  unsigned *block, unsigned *rep, unsigned *scene)
{
    if (!song_mode) { *block = 0; *rep = 0; *scene = 255; return &st->cur; }
    const struct pr_song *s = &st->song;
    uint32_t total = 0;
    for (unsigned i = 0; i < s->length && i < PR_CHAIN; ++i) total += s->reps[i];
    if (!total) return 0;
    if (cycle >= total) {
        if (!s->loop) return 0;
        cycle %= pr_nz(total);
    }
    for (unsigned i = 0; i < s->length && i < PR_CHAIN; ++i) {
        if (cycle < s->reps[i]) {
            *block = i; *rep = cycle; *scene = s->scene[i] % PR_SCENES;
            return &st->scene[*scene];
        }
        cycle -= s->reps[i];
    }
    return 0;
}
/* Q64 cycles per frame at `bpm` (quarter notes) with the pattern's beats per cycle. */
static inline uint64_t increment_at(const struct pr_pattern *p, float bpm)
{
    bpm = bpm < 20.0f ? 20.0f : bpm > 400.0f ? 400.0f : bpm;
    double inc = (double)bpm / 60.0 / (double)beats_of(p) / (double)pr_sr * 18446744073709551616.0;
    return inc < 1.0 ? 1u : (uint64_t)inc;
}
static inline float own_bpm(const struct pr_pattern *p)
{
    unsigned bpm10 = p->bpm10 < 300 ? 300 : p->bpm10 > 3000 ? 3000 : p->bpm10;
    return (float)bpm10 * 0.1f;
}
/* The position of a host's beat (quarter notes since the song's start) in cycles.
 * 0 = found, -1 = past the end of a song that does not loop. */
static int target_of(const struct pr_state *st, double beat, uint32_t *hi, uint64_t *lo)
{
    if (!(beat > 0.0)) beat = 0.0;
    double cyc = 0.0;
    if (!song_mode) cyc = beat / (double)beats_of(&st->cur);
    else {
        const struct pr_song *s = &st->song;
        double total = 0.0;
        for (unsigned i = 0; i < s->length && i < PR_CHAIN; ++i) total += (double)s->reps[i] * beats_of(&st->scene[s->scene[i] % PR_SCENES]);
        if (!(total > 0.0)) return -1;
        if (beat >= total) {
            if (!s->loop) return -1;
            double laps = (double)(int64_t)(beat / total);
            beat -= laps * total;
            if (beat < 0.0) beat = 0.0;
            if (beat >= total) beat = 0.0;
        }
        for (unsigned i = 0; i < s->length && i < PR_CHAIN; ++i) {
            double b = (double)beats_of(&st->scene[s->scene[i] % PR_SCENES]), len = (double)s->reps[i] * b;
            if (beat < len) { cyc += beat / b; break; }
            beat -= len; cyc += (double)s->reps[i];
        }
    }
    if (cyc > 4.0e9) cyc = 0.0;
    uint32_t h = (uint32_t)(int64_t)cyc;
    double f = (cyc - (double)h) * 18446744073709551616.0;
    *hi = h; *lo = f >= 18446744073709549568.0 ? 18446744073709549568u : f > 0.0 ? (uint64_t)f : 0u;
    return 0;
}
static inline void add_event(unsigned offset, unsigned ring, unsigned step, unsigned velocity, unsigned pattern,
                             int pitch, unsigned has_pitch, unsigned *count)
{
    if (*count >= PR_EVENTS) return;
    unsigned i = (*count)++;
    while (i && events[i - 1].offset > offset) { events[i] = events[i - 1]; --i; }
    events[i].offset = (uint16_t)offset; events[i].ring = (uint8_t)ring; events[i].step = (uint8_t)step;
    events[i].velocity = (uint8_t)velocity; events[i].pattern = (uint8_t)pattern;
    events[i].pitch = (int8_t)pitch; events[i].has_pitch = (uint8_t)has_pitch;
}
static inline unsigned ring_audible(const struct pr_pattern *p, unsigned r)
{
    unsigned solo = 0;
    for (unsigned i = 0; i < p->rings && i < PR_RINGS; ++i) solo |= p->ring[i].flags & PR_SOLO;
    const struct pr_ring *ring = &p->ring[r];
    if (ring->flags & PR_MUTE) return 0;
    return !solo || (ring->flags & PR_SOLO);
}
static inline float soft(float x)
{
    if (x > 3) x = 3; else if (x < -3) x = -3;
    return x * (27 + x * x) / (27 + 9 * x * x);
}

/* ---- MIDI out: note-ons at their frame, note-offs when their length has passed (frames) ---- */
static void note_off_at(unsigned i, int frame)
{
    if (!notes_out[i].on) return;
    vibe_midi_send_at((uint8_t)(0x80 | notes_out[i].ch), notes_out[i].note, 0, frame);
    notes_out[i].on = 0;
}
static void all_notes_off(int frame)
{
    for (unsigned i = 0; i < NOTES_OUT; ++i) note_off_at(i, frame);
}
/* The offs due inside this block, in frame order, before the ons of the same frames. */
static void offs_due(int frames, int upto)
{
    for (;;) {
        int best = -1; uint32_t when = 0;
        for (unsigned i = 0; i < NOTES_OUT; ++i)
            if (notes_out[i].on && notes_out[i].left < (uint32_t)frames && (int)notes_out[i].left <= upto &&
                (best < 0 || notes_out[i].left < when)) { best = (int)i; when = notes_out[i].left; }
        if (best < 0) return;
        note_off_at((unsigned)best, (int)when);
    }
}
static void note_on_at(unsigned ch, unsigned note, unsigned velocity, uint32_t length, int frame)
{
    ch &= 15u; note &= 127u; velocity = velocity < 1 ? 1 : velocity > 127 ? 127 : velocity;
    int slot = -1;
    for (unsigned i = 0; i < NOTES_OUT; ++i) {
        if (notes_out[i].on && notes_out[i].ch == ch && notes_out[i].note == note) note_off_at(i, frame);   /* retrigger */
        if (!notes_out[i].on && slot < 0) slot = (int)i;
    }
    if (slot < 0) {                                     /* every slot busy: end the one closest to its end */
        slot = 0;
        for (unsigned i = 1; i < NOTES_OUT; ++i) if (notes_out[i].left < notes_out[slot].left) slot = (int)i;
        note_off_at((unsigned)slot, frame);
    }
    if (vibe_midi_send_at((uint8_t)(0x90 | ch), (uint8_t)note, (uint8_t)velocity, frame) != 0) return;
    notes_out[slot].on = 1; notes_out[slot].ch = (uint8_t)ch; notes_out[slot].note = (uint8_t)note;
    notes_out[slot].left = (uint32_t)frame + (length ? length : 1u);
}
static void notes_age(int frames)
{
    for (unsigned i = 0; i < NOTES_OUT; ++i)
        if (notes_out[i].on) notes_out[i].left = notes_out[i].left > (uint32_t)frames ? notes_out[i].left - (uint32_t)frames : 0u;
}

void pr_render(const float *in, float *out_l, float *out_r, int frames_in)
{
    unsigned frames = frames_in < 1 ? 0u : frames_in > MAXB ? (unsigned)MAXB : (unsigned)frames_in;
    for (unsigned i = 0; i < frames; ++i) { out_l[i] = 0; out_r[i] = 0; }
    if (!frames || !initialised) return;
    VIBE_ACQUIRE();
    uint32_t seq = published_seq;
    VIBE_ACQUIRE();
    if (seq != current_seq) { current_seq = seq; VIBE_RELEASE(); adopted_seq = seq; }
    const struct pr_state *st = &publication[current_seq & 1u];
    unsigned count = 0;
    uint32_t frame0 = (uint32_t)vibe_frames();

    /* ---- requests from the control flow ---- */
    while (command_read != command_write) {
        VIBE_ACQUIRE();
        uint32_t r = command_read;
        struct command c = commands[r % CMD_SIZE];
        VIBE_RELEASE();
        command_read = r + 1u;
        if (c.type == CMD_PLAY) {
            if (following) continue;                     /* the host leads: its PLAY, not ours */
            song_mode = c.a; want_song = c.a;
            uint32_t start = 0;
            if (c.a) for (unsigned i = 0; i < c.b && i < st->song.length && i < PR_CHAIN; ++i) start += st->song.reps[i];
            phase_hi = start; phase_lo = 0; crossed = 0;
            if (!playing) vibe_transport_out(1);
            finished = 0; playing = 1;
        } else if (c.type == CMD_STOP) {
            if (following) continue;
            if (playing) { all_notes_off(0); vibe_transport_out(0); }
            playing = 0; phase_hi = 0; phase_lo = 0;
        } else if (c.type == CMD_SONG_MODE) {
            want_song = c.a;
        } else if (c.type == CMD_CLOCK) {
            clock_on = c.a; vibe_clock_out(c.a);
        } else if (c.type == CMD_AUDITION && c.a < PR_RINGS) {
            if (c.c == 255) add_event(0, c.a, 255u, c.b, 255, c.d, 1, &count);
            else add_event(0, c.a, c.c ? (unsigned)c.c - 1u : 255u, c.b, 255, 0, 0, &count);
        } else if (c.type == CMD_REC_ARM) {
            ss_rec_arm(c.a, 0.03f);
        } else if (c.type == CMD_REC_STOP) {
            ss_rec_stop();
        } else if (c.type == CMD_REC_RELEASE) {
            ss_rec_release();
        } else if (c.type == CMD_PANIC) {
            for (unsigned i = 0; i < PR_RINGS; ++i) { pd_panic(KIT(i)); ps_panic(i); ss_panic(i); }
            all_notes_off(0);
            uint16_t used = 0;
            for (unsigned q = 0; q < PR_RINGS; ++q) if (st->cur.ring[q].midi[0]) used |= (uint16_t)(1u << (st->cur.ring[q].midi[1] & 15u));
            for (unsigned ch = 0; ch < 16; ++ch) if (used >> ch & 1u) vibe_midi_send((uint8_t)(0xB0 | ch), 123, 0);
        }
    }

    /* ---- IN 1: the input level, and the recording of a sample ---- */
    ss_rec_block(in, frames);

    /* ---- who leads: a DAW or a MIDI clock (its song position), else our own transport ---- */
    const vibe_transport_info *tp = vibe_transport();
    float host_bpm = tp->has && tp->bpm > 0.0f ? tp->bpm : 0.0f;
    int lead = tp->has && tp->playing && host_bpm > 0.0f;
    int jumped = 0;
    int64_t correction = 0;
    if (lead) {
        if (!following) {                                /* the host starts: our own run ends, its position rules */
            following = 1; playing = 1; finished = 0; song_mode = want_song; jumped = 1;
        }
        uint32_t hi = 0; uint64_t lo = 0;
        if (target_of(st, tp->beat, &hi, &lo) < 0) { finished = 1; }
        else {
            finished = 0;
            /* the difference in 2^-32 cycles (enough: the tolerance is a few hundred frames) */
            int64_t diff = ((int64_t)(int32_t)(hi - phase_hi) << 32) + (int64_t)(lo >> 32) - (int64_t)(phase_lo >> 32);
            int64_t inc = (int64_t)(increment_at(&st->cur, host_bpm) >> 32);
            int64_t tolerance = (inc ? inc : 1) * 256;
            if (jumped || diff > tolerance || diff < -tolerance) { phase_hi = hi; phase_lo = lo; jumped = 1; }
            else correction = diff;
        }
    } else if (following) {                              /* the host stopped */
        following = 0; playing = 0; phase_hi = 0; phase_lo = 0; all_notes_off(0);
    }
    if (!lead && !playing && song_mode != want_song) song_mode = want_song;

    /* ---- the hits of this block ---- */
    unsigned block = 0, rep = 0, scene = 255;
    const struct pr_pattern *mixp = &st->cur;
    uint64_t inc_shown = 0;
    float bpm_now = host_bpm > 0.0f ? host_bpm : own_bpm(&st->cur);
    /* The block is cut at each cycle boundary so that a scene keeps its own tempo up to the exact
     * frame where the next scene starts. */
    if (jumped) crossed = 0;
    for (unsigned cursor = 0; playing && !finished && cursor < frames;) {
        const struct pr_pattern *p = pattern_at(st, phase_hi, &block, &rep, &scene);
        if (!p) {
            if (following) { finished = 1; break; }
            playing = 0; finished = 1; song_mode = want_song; phase_hi = phase_lo = 0; scene = 255;
            all_notes_off((int)cursor); vibe_transport_out(0);
            break;
        }
        mixp = p;
        float bpm = following ? host_bpm : own_bpm(p);
        bpm_now = bpm;
        uint64_t inc = increment_at(p, bpm);
        if (correction) {                                /* pull a small difference in over ~8 blocks */
            int64_t c = correction / (int64_t)(8u * frames), lim = (int64_t)(inc >> 34);
            c = c > lim ? lim : c < -lim ? -lim : c;
            inc = (uint64_t)((int64_t)inc + c * 4294967296LL);
            if (!inc) inc = 1;
        }
        inc_shown = inc;
        unsigned remaining = frames - cursor, seg = remaining;
        uint64_t from = phase_lo, span = inc * remaining, to = from + span;
        unsigned crosses = to < from;                     /* the cycle ends inside this segment */
        if (crosses) {
            uint64_t dist = 0u - from;
            seg = (unsigned)(dist / inc + (dist % inc ? 1u : 0u));
            if (seg > remaining) seg = remaining;
            if (!seg) seg = 1;
        }
        unsigned pindex = p == &st->cur ? 254 : (unsigned)(p - st->scene);
        for (unsigned r = 0; r < p->rings && r < PR_RINGS; ++r) {
            const struct pr_ring *ring = &p->ring[r];
            unsigned n = ring->n;
            if (!n || n > PR_STEPS || !ring_audible(p, r)) continue;
            uint64_t step = 0xffffffffffffffffull / pr_nz(n) + 1u;
            unsigned swing = (p->swing > 50 && p->swing <= 75 && !(n & 1u)) ? p->swing - 50u : 0;
            for (unsigned k = 0; k < n; ++k) {
                uint8_t v = ring->steps[k];
                if (!v) continue;
                uint64_t at = k * step;
                if (swing && (k & 1u)) at += swing * (step / 50u);   /* late off-beats, even n only */
                if ((at < from && !crossed) || (!crosses && at >= to)) continue;
                unsigned offset = at < from ? cursor : cursor + (unsigned)((at - from) / inc);
                if (offset >= cursor + seg || offset >= frames) continue;
                add_event(offset, r, k, v == 2 ? 127 : 90, pindex, 0, 0, &count);
            }
        }
        if (crosses) { phase_hi += 1; phase_lo = from + seg * inc; crossed = 1; }
        else { phase_lo = to; crossed = 0; }
        cursor += seg;
    }
    if (!following) vibe_tempo_set(bpm_now);

    /* ---- MIDI of this block, in frame order ---- */
    for (unsigned e = 0; e < count; ++e) {
        const struct pr_pattern *p = events[e].pattern >= 254 ? &st->cur : &st->scene[events[e].pattern % PR_SCENES];
        const struct pr_ring *ring = &p->ring[events[e].ring];
        unsigned mode = ring->midi[0];
        if (mode != PR_MIDI_NOTE && mode != PR_MIDI_STEPS) continue;
        offs_due((int)frames, events[e].offset);
        uint8_t note = events[e].step < PR_STEPS ? ring->note[events[e].step] : 0;
        int pitch = events[e].has_pitch ? events[e].pitch : pr_note_pitch(note);
        unsigned len = pr_note_len(note);
        float beat = pr_sr * 60.0f / (bpm_now > 1.0f ? bpm_now : 120.0f);    /* frames per quarter note */
        float g = len == PR_LEN_FREE || len >= PR_LENGTHS ? beat * .25f :
                  len >= PR_LEN_QUARTER ? beat * (float)(1u << (len - PR_LEN_QUARTER)) : beat / (float)(1u << (PR_LEN_QUARTER - len));
        uint32_t gate = (uint32_t)(g * .9f) + 1u;
        int n = (int)ring->midi[2] + (mode == PR_MIDI_STEPS ? pitch : 0);
        note_on_at(ring->midi[1] & 15u, (unsigned)(n < 0 ? 0 : n > 127 ? 127 : n), events[e].velocity, gate, events[e].offset);
    }
    offs_due((int)frames, (int)frames);
    notes_age((int)frames);

    /* ---- the voices ---- */
    for (unsigned i = 0; i < frames; ++i) { mix_left[i] = 0; mix_right[i] = 0; }
    unsigned any = 0;
    for (unsigned r = 0; r < PR_RINGS; ++r) {
        struct pd_kit *kit = KIT(r);
        unsigned has = 0;
        for (unsigned e = 0; e < count; ++e) if (events[e].ring == r) { has = 1; break; }
        /* Both engines render: a tail keeps ringing after the circle changes bank. */
        unsigned drums = pd_active(kit), tonal = ps_active(r), sampled = ss_active(r);
        if (!has && !drums && !tonal && !sampled) continue;
        for (unsigned i = 0; i < frames; ++i) ring_buffer[i] = 0;
        unsigned at = 0;
        for (unsigned e = 0; e < count; ++e) {
            if (events[e].ring != r) continue;
            unsigned off = events[e].offset;
            if (off > at) {
                if (drums) pd_render(kit, ring_buffer + at, off - at);
                if (tonal) ps_render(r, ring_buffer + at, off - at);
                if (sampled) ss_render(r, ring_buffer + at, off - at);
                at = off;
            }
            const struct pr_pattern *p = events[e].pattern >= 254 ? &st->cur : &st->scene[events[e].pattern % PR_SCENES];
            const struct pr_ring *ring = &p->ring[r];
            float velocity = (float)events[e].velocity / 127.0f;
            /* The step's own transposition and length (a note value of the pattern's beat). */
            uint8_t note = events[e].step < PR_STEPS ? ring->note[events[e].step] : 0;
            int pitch = events[e].has_pitch ? events[e].pitch : pr_note_pitch(note);
            unsigned len = pr_note_len(note);
            uint32_t gate = 0;
            if (len && len < PR_LENGTHS) {
                float beat = pr_sr * 60.0f / (bpm_now > 1.0f ? bpm_now : 120.0f);
                float g = len >= PR_LEN_QUARTER ? beat * (float)(1u << (len - PR_LEN_QUARTER)) : beat / (float)(1u << (PR_LEN_QUARTER - len));
                gate = (uint32_t)g;
            }
            unsigned silent = ring->midi[0] && (ring->midi[3] & PR_MIDI_ONLY);   /* MIDI only */
            if (silent) { /* routed to MIDI only */ }
            else if (ring->bank == PR_BANK_SAMPLE) {
                ss_trigger(r, ring->sound, velocity, ring->tune, ring->decay, ring->tone, ring->drive, pitch, gate);
                sampled = 1;
            } else if (ring->bank) {
                ps_trigger(r, ring->bank, ring->sound, velocity, ring->tune, ring->decay, ring->tone, ring->drive, pitch, gate);
                tonal = 1;
            } else {
                pd_trigger(kit, ring->sound, velocity, ring->tune, ring->decay, ring->tone, ring->drive, pitch, gate);
                drums = 1;
            }
            if (events[e].step != 255) { ring_last[r] = events[e].step; ++ring_hits[r]; }
        }
        if (at < frames) {
            if (drums) pd_render(kit, ring_buffer + at, frames - at);
            if (tonal) ps_render(r, ring_buffer + at, frames - at);
            if (sampled) ss_render(r, ring_buffer + at, frames - at);
        }
        const struct pr_ring *ring = &mixp->ring[r];
        float level = (float)ring->level * (1.0f / 127.0f);
        float gl = pan_left[ring->pan & 127] * level, gr = pan_right[ring->pan & 127] * level;
        for (unsigned i = 0; i < frames; ++i) { mix_left[i] += ring_buffer[i] * gl; mix_right[i] += ring_buffer[i] * gr; }
        any = 1;
    }
    float peak_l = 0, peak_r = 0;
    if (any) {
        float vol = pr_volume;
        vol = vol > 0.0f ? (vol < 1.5f ? vol : 1.5f) : 0.0f;
        for (unsigned i = 0; i < frames; ++i) {
            float l = soft(mix_left[i] * 0.8f) * vol, rr = soft(mix_right[i] * 0.8f) * vol;
            if (!(l == l)) l = 0;                        /* never a NaN out */
            if (!(rr == rr)) rr = 0;
            out_l[i] = l; out_r[i] = rr;
            float a = l < 0 ? -l : l, b = rr < 0 ? -rr : rr;
            if (a > peak_l) peak_l = a;
            if (b > peak_r) peak_r = b;
        }
    }

    /* ---- what the display shows ---- */
    uint32_t s0 = status_seq + 1u;
    status_seq = s0;
    VIBE_RELEASE();
    status.phase_hi = phase_hi; status.phase_lo = (uint32_t)(phase_lo >> 32); status.inc = (uint32_t)(inc_shown >> 32);
    status.frame_lo = frame0 + frames;
    status.playing = playing && !finished; status.song = song_mode; status.block = (uint8_t)block; status.rep = (uint8_t)rep;
    status.scene = (uint8_t)(playing && song_mode ? scene : 255); status.finished = finished;
    status.host = (uint8_t)(following || host_bpm > 0.0f); status.song_mode = want_song;
    {
        float b10 = bpm_now * 10.0f + .5f;
        status.bpm10 = (uint16_t)(b10 < 100.0f ? 100.0f : b10 > 4000.0f ? 4000.0f : b10);
    }
    for (unsigned r = 0; r < PR_RINGS; ++r) { status.hits[r] = ring_hits[r]; status.last_step[r] = ring_last[r]; }
    status.peak_l = peak_l; status.peak_r = peak_r;
    status.in_peak = ss_in_peak(); status.rec_state = (uint8_t)ss_rec_state(); status.rec_slot = (uint8_t)ss_rec_slot();
    status.rec_len = ss_rec_length();
    VIBE_RELEASE();
    status_seq = s0 + 1u;
    (void)clock_on;
}
