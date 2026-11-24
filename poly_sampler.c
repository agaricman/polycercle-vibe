/* SPDX-License-Identifier: GPL-3.0-only
 * Polycercle: the SAMPLE bank – sounds recorded from the VIBE's IN 1 (or the computer's input in the browser,
 * the plugin's input in a DAW), sixteen slots shared by every circle.
 *
 * Recording (audio flow): armed, it waits for the input to cross a threshold, keeps the few milliseconds before
 * (pre-roll), records until the sound has died away for 0.4 s or the slot is full, then stops. The control flow
 * takes the recording (ss_take: trimmed, normalised, faded out, stored as 16 bits) and releases the buffer.
 *
 * Playback (audio flow): two voices per circle, played at any pitch (the tune, the step's transposition), with
 * the circle's decay (a fade), tone (low-pass below 64, high-pass above), drive and the step's length. */
#include "poly.h"

int16_t smp_data[SMP_SLOTS][SMP_MAX];
volatile uint32_t smp_len[SMP_SLOTS];
float smp_rate[SMP_SLOTS];
float smp_rec[SMP_MAX];

enum { VOICES = 2, PRE = 1024 };
struct sv {
    uint8_t on, slot, releasing;
    double pos, inc;
    float gain, env, env_mul, rel_mul;
    float lp, hp_state, coef, hp;      /* tone: one-pole low-pass (coef) or high-pass (hp = 1) */
    float drive;
    uint32_t gate, age;
};
static struct sv voices[PR_RINGS][VOICES];
static uint8_t next_voice[PR_RINGS];

static inline float s_exp2(float x)
{
    if (x < -30.0f) return 0.0f;
    if (x > 30.0f) x = 30.0f;
    int i = (int)(x < 0 ? x - 1.0f : x);
    float f = x - (float)i, p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    union { float f; uint32_t u; } u = { p };
    u.u += (uint32_t)((int32_t)i << 23);
    return u.f;
}

void ss_init(void)
{
    memset(voices, 0, sizeof voices);
}

void ss_trigger(unsigned ring, unsigned slot, float velocity,
    unsigned tune, unsigned decay, unsigned tone, unsigned drive, int semitones, uint32_t gate)
{
    if (ring >= PR_RINGS || slot >= SMP_SLOTS || !(velocity > 0)) return;
    uint32_t len = smp_len[slot];
    VIBE_ACQUIRE();
    if (len < 2 || !(smp_rate[slot] > 0)) return;
    unsigned v = next_voice[ring];
    next_voice[ring] = (uint8_t)((v + 1) % VOICES);
    struct sv *s = &voices[ring][v];
    float semis = (float)((int)tune - 64 + semitones);
    semis = semis < -48 ? -48 : semis > 48 ? 48 : semis;
    s->inc = (double)smp_rate[slot] / (double)pr_sr * (double)s_exp2(semis * (1.0f / 12.0f));
    s->pos = 0.0; s->slot = (uint8_t)slot; s->on = 1; s->releasing = 0; s->age = 0;
    s->gain = velocity * 0.75f; s->env = 1.0f;
    /* decay below 64 fades the sound out: 30 ms at 0, ~7 s at 63; 64 and above: the sample as it is */
    int d = (int)decay - 64;
    s->env_mul = d >= 0 ? 1.0f : s_exp2(-1.4427f / (0.03f * s_exp2((float)(d + 64) * 0.125f) * pr_sr));
    s->rel_mul = s_exp2(-1.4427f / (0.008f * pr_sr));                 /* 8 ms release at the step's end */
    int t = (int)tone - 64;
    s->lp = 0; s->hp_state = 0; s->hp = 0;
    if (t < 0) {          /* low-pass: 20 kHz at 64 down to ~80 Hz at 0 */
        float fc = 20000.0f * s_exp2((float)t * 0.125f), w = 6.2831853f * fc / pr_sr;
        s->coef = w > 1.0f ? 1.0f : w;
    } else if (t > 0) {   /* high-pass: 20 Hz .. ~2.6 kHz */
        float fc = 20.0f * s_exp2((float)t / 9.0f), w = 6.2831853f * fc / pr_sr;
        s->coef = w > 1.0f ? 1.0f : w; s->hp = 1;
    } else s->coef = 1.0f;
    s->drive = 1.0f + (float)drive * (1.0f / 16.0f);
    s->gate = gate;
}

static inline float soft(float x) { if (x > 3) x = 3; else if (x < -3) x = -3; return x * (27 + x * x) / (27 + 9 * x * x); }

void ss_render(unsigned ring, float *mono, unsigned count)
{
    if (ring >= PR_RINGS) return;
    for (unsigned v = 0; v < VOICES; ++v) {
        struct sv *s = &voices[ring][v];
        if (!s->on) continue;
        uint32_t len = smp_len[s->slot];
        VIBE_ACQUIRE();
        const int16_t *d = smp_data[s->slot];
        for (unsigned i = 0; i < count; ++i) {
            uint32_t k = (uint32_t)s->pos;
            if (k + 1 >= len) { s->on = 0; break; }
            float f = (float)(s->pos - (double)k);
            float x = ((float)d[k] + ((float)d[k + 1] - (float)d[k]) * f) * (1.0f / 32768.0f);
            s->lp += (x - s->lp) * s->coef;
            x = s->hp ? x - s->lp : s->lp;
            if (s->drive > 1.0f) x = soft(x * s->drive) / soft(s->drive);
            if (s->gate && ++s->age >= s->gate) s->releasing = 1;
            if (s->releasing) { s->env *= s->rel_mul; if (s->env < 0.0005f) { s->on = 0; break; } }
            else s->env *= s->env_mul;
            if (s->env < 0.0002f) { s->on = 0; break; }
            mono[i] += x * s->gain * s->env;
            s->pos += s->inc;
        }
    }
}

unsigned ss_active(unsigned ring)
{
    if (ring >= PR_RINGS) return 0;
    for (unsigned v = 0; v < VOICES; ++v) if (voices[ring][v].on) return 1;
    return 0;
}

void ss_panic(unsigned ring)
{
    if (ring < PR_RINGS) for (unsigned v = 0; v < VOICES; ++v) voices[ring][v].on = 0;
}

/* ---------------- recording (audio flow) ---------------- */
static unsigned rec_state, rec_slot;
static uint32_t rec_len, rec_quiet, rec_max;
static float rec_threshold = 0.03f, in_peak = -1.0f;
static float pre[PRE];
static unsigned pre_at;

void ss_rec_arm(unsigned slot, float threshold)
{
    if (rec_state == REC_DONE) return;              /* the last take is not taken yet */
    rec_slot = slot % SMP_SLOTS; rec_threshold = threshold; rec_len = 0; rec_quiet = 0;
    rec_max = (uint32_t)(pr_sr * 4.0f); if (rec_max > SMP_MAX) rec_max = SMP_MAX;
    rec_state = REC_ARMED;
}
void ss_rec_stop(void)
{
    if (rec_state == REC_RECORDING && rec_len > 64) rec_state = REC_DONE;
    else if (rec_state != REC_DONE) rec_state = REC_IDLE;
}
void ss_rec_release(void) { if (rec_state == REC_DONE) rec_state = REC_IDLE; }
unsigned ss_rec_state(void) { return rec_state; }
unsigned ss_rec_slot(void) { return rec_slot; }
uint32_t ss_rec_length(void) { return rec_len; }
float ss_in_peak(void) { return in_peak; }

void ss_rec_block(const float *in, unsigned frames)
{
    if (!in) { in_peak = -1.0f; return; }
    float peak = 0.0f;
    for (unsigned i = 0; i < frames; ++i) { float a = in[i] < 0 ? -in[i] : in[i]; if (a > peak) peak = a; }
    in_peak = peak;
    unsigned i = 0;
    if (rec_state == REC_ARMED) {
        for (; i < frames; ++i) {
            float a = in[i] < 0 ? -in[i] : in[i];
            if (a >= rec_threshold) break;
            pre[pre_at] = in[i]; pre_at = (pre_at + 1) % PRE;
        }
        if (i == frames) return;
        /* the sound starts: 5 ms of what came before, then the recording */
        unsigned back = (unsigned)(pr_sr * 0.005f); if (back > PRE) back = PRE;
        for (unsigned k = 0; k < back; ++k) smp_rec[k] = pre[(pre_at + PRE - back + k) % PRE];
        rec_len = back; rec_quiet = 0; rec_state = REC_RECORDING;
    }
    if (rec_state != REC_RECORDING) return;
    uint32_t hold = (uint32_t)(pr_sr * 0.4f), least = (uint32_t)(pr_sr * 0.05f);
    for (; i < frames; ++i) {
        float x = in[i];
        if (!(x == x)) x = 0.0f;
        smp_rec[rec_len++] = x;
        float a = x < 0 ? -x : x;
        rec_quiet = a < rec_threshold * 0.3f ? rec_quiet + 1u : 0u;
        if (rec_len >= rec_max || (rec_quiet > hold && rec_len > least)) {
            VIBE_RELEASE();
            rec_state = REC_DONE;
            return;
        }
    }
}

/* ---------------- control flow ---------------- */
static void slot_store(unsigned slot, uint32_t length, float rate)
{
    VIBE_RELEASE();
    smp_rate[slot] = rate;
    VIBE_RELEASE();
    smp_len[slot] = length;
}
void ss_set(unsigned slot, const int16_t *data, uint32_t length, float rate)
{
    if (slot >= SMP_SLOTS) return;
    if (length > SMP_MAX) length = SMP_MAX;
    smp_len[slot] = 0;                               /* playing voices of this slot stop at once */
    VIBE_RELEASE();
    if (length) memcpy(smp_data[slot], data, length * sizeof(int16_t));
    slot_store(slot, length, rate);
}
int ss_take(unsigned slot, uint32_t length, float rate)
{
    if (slot >= SMP_SLOTS || length < 64) return -1;
    if (length > SMP_MAX) length = SMP_MAX;
    VIBE_ACQUIRE();
    /* the end: the last sample above 1/30 of the peak, plus a 20 ms fade */
    float peak = 0.0f;
    for (uint32_t i = 0; i < length; ++i) { float a = smp_rec[i] < 0 ? -smp_rec[i] : smp_rec[i]; if (a > peak) peak = a; }
    if (!(peak > 0.001f)) return -1;
    uint32_t end = length;
    while (end > 1) { float a = smp_rec[end - 1] < 0 ? -smp_rec[end - 1] : smp_rec[end - 1]; if (a > peak * (1.0f / 30.0f)) break; --end; }
    uint32_t fade = (uint32_t)(rate * 0.02f);
    end = end + fade < length ? end + fade : length;
    float gain = 0.9f / peak;
    smp_len[slot] = 0;
    VIBE_RELEASE();
    int16_t *d = smp_data[slot];
    for (uint32_t i = 0; i < end; ++i) {
        float x = smp_rec[i] * gain;
        if (i < 32) x *= (float)i / 32.0f;                              /* no click at the start */
        if (end - i <= fade && fade) x *= (float)(end - i) / (float)fade;
        x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
        d[i] = (int16_t)(x * 32767.0f);
    }
    slot_store(slot, end, rate);
    return 0;
}
