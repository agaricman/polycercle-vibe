/* SPDX-License-Identifier: MIT
 * Copyright (c) 2020 Electrosmith, Corp, Emilie Gillet; 2026 M-VAVE; 2026 Polycercle.
 * Drum voices for Polycercle, taken from the FM1 Open SDK 808 kit
 * (third_party/synth/drum808.c: DaisySP/Plaits bass and snare excitation,
 * metallic noise and FM1 supplementary percussion, MIT licence).
 * Changes: one kit per circle, direct trigger of any of the 16 sounds with
 * per-circle tune/decay/tone/drive, mono render, private symbol names;
 * VIBE FM: every per-sample constant follows the host's sample rate (pd_rate),
 * and at 44.1 kHz the voices are those of the FM-1.
 * See THIRD_PARTY.md for the MIT terms of the original files. */
#include "poly.h"

/* Size: helpers used at trigger time stay out of line (flash is nearly full). */
#define FM1_NOINLINE __attribute__((noinline))
/* ---- math helpers (from third_party/synth/dsp_math.h, MIT) ---- */
#define M_PI_F 3.14159265358979323846f
/* Audio code avoids libc calls: bit casts use a union, clears use loops. */
typedef union { float f; uint32_t u; } bits_t;
static inline float m_abs(float x) { return x < 0 ? -x : x; }
static inline float m_clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float m_min(float a, float b) { return a < b ? a : b; }
static inline float m_max(float a, float b) { return a > b ? a : b; }
static FM1_NOINLINE float m_exp2(float x)
{
    x = m_clamp(x, -120.0f, 120.0f);
    int n = (int)(x + (x < 0 ? -0.5f : 0.5f));
    float a = (x - (float)n) * 0.6931471805599453f;
    float p = 1.0f + a * (1.0f + a * (0.5f + a * (1.0f / 6.0f + a * (1.0f / 24.0f + a * (1.0f / 120.0f + a / 720.0f)))));
    bits_t scale; scale.u = (uint32_t)(n + 127) << 23;
    return p * scale.f;
}
static inline float m_exp(float x) { return m_exp2(x * 1.4426950408889634f); }
/* ---- the sample rate: constants written for 44.1 kHz, recomputed for the host's rate ---- */
static struct {
    float sr, k;                        /* k = 44100 / sr: one 44.1 kHz sample in samples at this rate */
    float noise;                        /* white noise keeps its level per Hz: sqrt(sr / 44100) */
    float bd_pulse, lp_01ms, bd_retrig, sd_pulse, sd_lp, transient, cp_pulse, release;
    unsigned ms1, ms6, ms10, ms20, ms30, ms50, life;
} R = {44100.0f, 1.0f, 1.0f, .886621315f, .22675737f, .999546485f, .77324263f, .75f, .9994962f, .990f, .9985f,
       44, 265, 441, 882, 1323, 2205, 44100u * 16u};
static inline float m_decay(float seconds) { return m_exp(-1.0f / (R.sr * m_max(seconds, 0.00001f))); }
static inline float m_sin(float x)
{
    x *= 1.0f / (2.0f * M_PI_F); x -= (float)(int)x;
    if (x > .5f) x -= 1; if (x < -.5f) x += 1;
    float a = x * (2.0f * M_PI_F);
    if (a > M_PI_F * .5f) a = M_PI_F - a; if (a < -M_PI_F * .5f) a = -M_PI_F - a;
    float q = a * a;
    return a * (1.0f + q * (-1.0f / 6.0f + q * (1.0f / 120.0f + q * (-1.0f / 5040.0f + q * (1.0f / 362880.0f - q / 39916800.0f)))));
}
static inline float m_cos(float x) { return m_sin(x + M_PI_F * .5f); }
static FM1_NOINLINE float m_tan(float x) { return m_sin(x) / m_cos(x); }
static FM1_NOINLINE float m_sqrt(float x)
{
    if (x <= 0) return 0;
    bits_t b; b.f = x; b.u = (b.u >> 1) + 0x1fc00000u;
    float y = b.f;
    for (unsigned i = 0; i < 4; i++) y = .5f * (y + x / y);
    return y;
}
static FM1_NOINLINE float m_log2(float x)
{
    bits_t b; b.f = m_max(x, 1e-30f);
    int e = (int)(b.u >> 23) - 127; b.u = (b.u & 0x7fffffu) | 0x3f800000u;
    float m = b.f;
    if (m > 1.41421356f) { m *= .5f; e++; }
    float z = (m - 1) / (m + 1), q = z * z;
    return (float)e + 2.8853900817779268f * z * (1 + q * (1.0f / 3 + q * (1.0f / 5 + q * (1.0f / 7 + q / 9))));
}
static inline float m_noise(uint32_t *state)
{
    uint32_t n = *state; n ^= n << 13; n ^= n >> 17; n ^= n << 5; *state = n;
    return (float)(n >> 8) * (1.0f / 8388608.0f) - 1.0f;
}
typedef struct { float low, band, g, r, h; } svf_t;
static FM1_NOINLINE void svf_set(svf_t *s, float hz, float q)
{
    s->g = m_tan(M_PI_F * m_clamp(hz, 1.0f, m_min(18000.0f, R.sr * .45f)) / R.sr);
    s->r = 1.0f / m_max(q, .1f); s->h = 1.0f / (1.0f + s->r * s->g + s->g * s->g);
}
static inline float svf(svf_t *s, float x, unsigned mode)
{
    float hp = (x - (s->r + s->g) * s->band - s->low) * s->h;
    float bp = s->g * hp + s->band; s->band = bp + s->g * hp;
    float lp = s->g * bp + s->low; s->low = lp + s->g * bp;
    return mode == 0 ? lp : mode == 1 ? bp : mode == 2 ? hp : lp + hp;
}

/* ---- voices (from drum808.c) ---- */
enum { S_BD, S_SD, S_LT, S_MT, S_HT, S_LC, S_MC, S_HC, S_RS, S_CL, S_MA, S_CP, S_CB, S_CH, S_OH, S_CY, S_NUM };
enum { P_LEVEL, P_TUNE, P_DECAY, P_DRIVE, P_DIST, P_X1, P_X2, P_NUM };
typedef struct {
    uint8_t active, sound;
    unsigned age, quiet, pulse_count, fm_count;
    uint32_t random, phase[6], increment[6];
    float level, env, decay, pulse, pulse_lp, fm_lp, retrigger, low, tone;
    float frequency, q, accent, attack, snappy, gain, velocity, drive, drive_norm, crush_step;
    float peak, last, tail_step, tone_coefficient, pulse_gain, transient_decay;
    float transpose, fade;  /* Polycercle: step transposition (semitones), release after the gate */
    uint32_t gate;
    svf_t filter[4];
} voice_t;
struct pd_kit {
    uint8_t pot[S_NUM][P_NUM];
    voice_t bd, sd, tom[3], rs, cl, ma, cp, cb, ch, oh, cy;
};

static const uint8_t decay_defaults[S_NUM] = {87, 108, 68, 56, 53, 58, 36, 34, 40, 64, 51, 64, 62, 47, 74, 70};
/* X1 = tone (kick), snappy (snare), attack (maracas). X2 = attack (kick), tone (snare). */
static const uint8_t x1_defaults[S_NUM] = {42, 89, 0, 0, 0, 0, 0, 0, 0, 0, 32, 32, 0, 0, 0, 0};
static const uint8_t x2_defaults[S_NUM] = {24, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static voice_t *voice_of(struct pd_kit *d, unsigned s)
{
    if (s == S_BD) return &d->bd;
    if (s == S_SD) return &d->sd;
    if (s >= S_LT && s <= S_HC) return &d->tom[(s - S_LT) % 3];
    switch (s) {
    case S_RS: return &d->rs; case S_CL: return &d->cl; case S_MA: return &d->ma; case S_CP: return &d->cp;
    case S_CB: return &d->cb; case S_CH: return &d->ch; case S_OH: return &d->oh; default: return &d->cy;
    }
}
/* c per 44.1 kHz sample, as the same decay per sample at this rate */
static float per_sample(float c) { return R.k == 1.0f ? c : m_exp2(R.k * m_log2(c)); }
static unsigned samples(float seconds) { return (unsigned)(seconds * R.sr + .5f); }
void pd_rate(float sr)
{
    if (!(sr >= 8000.0f && sr <= 384000.0f)) sr = 44100.0f;
    R.sr = sr; R.k = 44100.0f / sr; R.noise = sr == 44100.0f ? 1.0f : m_sqrt(sr / 44100.0f);
    R.bd_pulse = 1.0f - 1.0f / (0.2e-3f * sr);       /* Plaits: pulse decay 0.2 ms, filter 0.1 ms, retrigger 50 ms */
    R.lp_01ms = 1.0f / (0.1e-3f * sr);
    R.bd_retrig = 1.0f - 1.0f / (0.05f * sr);
    R.sd_pulse = 1.0f - 1.0f / (0.1e-3f * sr);
    R.sd_lp = 1.0f - per_sample(.25f);
    R.transient = per_sample(.9994962f);
    R.cp_pulse = per_sample(.990f);
    R.release = per_sample(.9985f);
    if (sr == 44100.0f) {                             /* the FM-1's own figures, bit for bit */
        R.bd_pulse = .886621315f; R.lp_01ms = .22675737f; R.bd_retrig = .999546485f; R.sd_pulse = .77324263f; R.sd_lp = .75f;
    }
    R.ms1 = samples(.001f); R.ms6 = samples(.006f); R.ms10 = samples(.01f); R.ms20 = samples(.02f);
    R.ms30 = 3 * R.ms10; R.ms50 = samples(.05f); R.life = (unsigned)sr * 16u;
}
static struct pd_kit kits[PR_RINGS] __attribute__((aligned(8)));
struct pd_kit *pd_kit_get(unsigned index) { return &kits[index % PR_RINGS]; }
void pd_kit_init(struct pd_kit *d)
{
    memset(d, 0, sizeof *d);
    for (unsigned s = 0; s < S_NUM; s++) {
        d->pot[s][P_LEVEL] = 64; d->pot[s][P_TUNE] = 64; d->pot[s][P_DECAY] = decay_defaults[s];
        d->pot[s][P_X1] = x1_defaults[s]; d->pot[s][P_X2] = x2_defaults[s];
    }
    for (unsigned s = 0; s < S_NUM; s++) voice_of(d, s)->random = 0x682f0745u + s * 0x7193u;
}
static float diode(float x) { return x >= 0 ? x : 1.4f * x / (1 + 2 * m_abs(x)); }
static float softclip(float x) { x = m_clamp(x, -3, 3); return x * (27 + x * x) / (27 + 9 * x * x); }
static void configure(struct pd_kit *d, voice_t *v)
{
    unsigned s = v->sound;
    const uint8_t *p = d->pot[s];
    static const float frequencies[] = {61, 175, 84, 122, 175, 207, 304, 407, 506, 2517, 11000, 1000, 540, 414, 414, 414};
    static const float durations[] = {.50f, .27f, .41f, .29f, .27f, .31f, .17f, .16f, .017f, .03f, .04f, .40f, .62f, .07f, .40f, 1.53f};
    static const float trims[] = {1.63f, .74f, .388f, .383f, .394f, .333f, .359f, .354f, .712f, .520f, .555f, 2.55f, .925f, 1.94f, .866f, .58f};
    float tune = m_exp2(((int)p[P_TUNE] - 64) / (s >= S_LT && s <= S_HC ? 384.0f : 64.0f) + v->transpose * (1.0f / 12));
    v->frequency = frequencies[s] * tune;
    float seconds = durations[s] * m_exp2(((int)p[P_DECAY] - decay_defaults[s]) / 32.0f);
    if (s == S_BD) seconds = (.0163f + .00106f * p[P_DECAY]) * 4.605170186f / tune;
    v->decay = m_decay(seconds / 4.605170186f); v->tail_step = 1 / (R.sr * seconds);
    v->transient_decay = R.transient;
    v->accent = v->velocity;
    float balance = s == S_BD ? 1.2589254f : s == S_LT ? 1.7782794f : s >= S_MT && s <= S_HT ? 1.9952623f : 1;
    v->gain = trims[s] * (p[P_LEVEL] / 64.0f) * v->accent * balance;
    float drive = p[P_DRIVE] / 127.0f, group = s <= S_HC || s == S_CB ? 1 : 2;
    float boost = m_exp2(5 * drive);
    int crush = p[P_DIST] == 6;
    v->drive = (crush ? 1 : .7f * boost) * trims[s] / group;
    v->drive_norm = group * (p[P_LEVEL] / 64.0f) * v->accent / (crush ? 1 : m_sqrt(boost)) * balance;
    v->crush_step = .04f + .62f * drive * drive;
    v->tone = p[P_X1] / 127.0f; v->snappy = p[P_X1] / 127.0f; v->attack = p[P_X2] / 127.0f;
    v->q = m_max(1, M_PI_F * v->frequency * seconds / 4.605170186f);
    v->tone_coefficient = m_min(4 * v->frequency * m_exp2(v->tone * 9) / R.sr, 1);
    v->pulse_gain = 44.1f / v->frequency;
    svf_set(&v->filter[0], v->frequency, v->q);
    svf_set(&v->filter[1], 2 * v->frequency, s == S_BD ? .85f : .25f * v->q);
    svf_set(&v->filter[2], s == S_BD ? 28 : s == S_SD ? 6500 : s >= S_LT && s <= S_HT ? 4000 : 8000, .7071f);
    svf_set(&v->filter[3], s == S_CP ? 1000 : 10000, .7f);
    static const float ratios[] = {1, 1.304f, 1.466f, 1.787f, 1.932f, 2.536f};
    static const float metal[] = {207.0f / 414, 307.4f / 414, 370.3f / 414, 523.8f / 414, 544.3f / 414, 807.3f / 414};
    for (unsigned j = 0; j < 6; j++)
        v->increment[j] = (uint32_t)(m_min(v->frequency * (s >= S_CH ? metal[j] : ratios[j]) / R.sr, .49f) * 4294967296.0f);
    if (s == S_RS) v->increment[1] = (uint32_t)(v->frequency * 3.4f * (4294967296.0f / R.sr));
    if (s == S_CB) {
        v->increment[0] = (uint32_t)(540 * tune * (4294967296.0f / R.sr));
        v->increment[1] = (uint32_t)(810 * tune * (4294967296.0f / R.sr));
        svf_set(&v->filter[0], 1600 * tune, .8f);
    }
    if (s >= S_CH) {
        svf_set(&v->filter[2], s == S_CY ? 7100 : s == S_CH ? 6000 : 6200, s == S_CY ? 1.5f : 2.0f);
        svf_set(&v->filter[3], s == S_CH ? 9500 : s == S_OH ? 5500 : 5000, .7071f);
    }
    if (s == S_SD) {
        float body = (.0118f + .00028f * p[P_DECAY]) / tune;
        float q = M_PI_F * v->frequency * body;
        svf_set(&v->filter[0], v->frequency, q);
        svf_set(&v->filter[1], 1.94f * v->frequency, .55f * q);
        v->pulse_gain = m_exp2(.9f * m_log2(.042f / (body * tune)));
        svf_set(&v->filter[2], 2500, .7071f);
        v->decay = m_decay(.02f + .000512f * p[P_DECAY]);
    }
    if (s >= S_CH) svf_set(&v->filter[0], 3000, .7071f);
    if (s == S_CH) { v->decay = m_decay(seconds * .32f); v->tail_step /= 1.12f; }
    if (s == S_CY) { v->decay = m_decay(seconds * .065f); v->tail_step /= 1.06f; }
    if (s == S_CP) { svf_set(&v->filter[3], 900 * tune, 1.2f); v->decay = m_decay(seconds * .09f); v->transient_decay = m_decay(seconds * .57f); }
}
void pd_trigger(struct pd_kit *d, unsigned s, float velocity,
    unsigned tune, unsigned decay, unsigned tone, unsigned drive, int semitones, uint32_t gate)
{
    if (s >= S_NUM || !(velocity > 0)) return;
    uint8_t *p = d->pot[s];
    p[P_TUNE] = (uint8_t)(tune > 127 ? 127 : tune);
    p[P_DECAY] = (uint8_t)(decay > 127 ? 127 : decay);
    p[P_DRIVE] = (uint8_t)(drive > 127 ? 127 : drive);
    p[P_DIST] = 3;                       /* saturation */
    if (s == S_BD || s == S_SD || s == S_MA || s == S_CP) p[P_X1] = (uint8_t)(tone > 127 ? 127 : tone);
    voice_t *v = voice_of(d, s);
    uint32_t rng = v->random;
    { uint32_t *w = (uint32_t *)(void *)v; for (unsigned i = 0; i < sizeof *v / 4; i++) w[i] = 0; }
    v->random = rng ? rng : 0x716fbc31u;
    v->sound = (uint8_t)s; v->active = 1; v->env = 1; v->level = 1;
    v->velocity = m_clamp(velocity, 0, 1); v->pulse_count = R.ms1; v->fm_count = R.ms6;
    v->transpose = (float)semitones; v->gate = gate; v->fade = 1;
    configure(d, v);
}
static inline __attribute__((always_inline)) float percussion(voice_t *v)
{
    unsigned s = v->sound; float result = 0;
    if (s == S_BD) {
        float pulse;
        if (v->pulse_count) { --v->pulse_count; pulse = 10 - (v->pulse_count ? 0 : 1); v->pulse = pulse; }
        else { v->pulse *= R.bd_pulse; pulse = v->pulse; }
        v->pulse_lp += (pulse - v->pulse_lp) * R.lp_01ms;
        pulse = diode(pulse - v->pulse_lp + .044f * pulse);
        float fm = v->fm_count ? 1 : 0; if (v->fm_count && !--v->fm_count) v->retrigger = -.8f;
        if (!v->fm_count) v->retrigger *= R.bd_retrig;
        v->fm_lp += (fm - v->fm_lp) * R.lp_01ms;
        float f = v->frequency * (.958f + (.053f + .08f * v->accent) * v->env + v->fm_lp * v->attack * 2.0f);
        if (!(v->age & 3)) svf_set(&v->filter[0], f, v->q);
        float bp = svf(&v->filter[0], (pulse - .2f * v->retrigger) * v->pulse_gain, 1); v->low = v->filter[0].low;
        v->last += (bp + .08f * (v->tone + .25f) * pulse - v->last) * v->tone_coefficient;
        float body = 1.38f * (.5f + .5f * v->level) * svf(&v->filter[1], m_abs(v->last), 1);
        result = svf(&v->filter[2], v->last + body, 2);
    } else if (s == S_SD) {
        float pulse;
        if (v->pulse_count) { --v->pulse_count; v->pulse = 10 - (v->pulse_count ? 0 : 1); pulse = v->pulse; }
        else { v->pulse *= R.sd_pulse; pulse = v->pulse; }
        v->pulse_lp += (pulse - v->pulse_lp) * R.sd_lp;
        float a = svf(&v->filter[0], pulse - v->pulse_lp + .006f * pulse, 1);
        float b = svf(&v->filter[1], .026f * pulse, 1);
        float tone = m_clamp(v->attack, 0, 1), shell = softclip((1.5f + 4.5f * (1 - tone) * (1 - tone)) * a + (.15f + 2 * tone) * b);
        float noise = (m_noise(&v->random) * R.noise);
        result = 5.4f * v->pulse_gain * (1 - v->snappy) * shell + .40f * v->snappy * svf(&v->filter[2], noise * v->env, 2);
    } else if (s >= S_LT && s <= S_HC) {
        float pitch = v->frequency * (1 + (s <= S_HT ? .035f : .008f) * v->level);
        v->phase[0] += (uint32_t)(pitch * (4294967296.0f / R.sr));
        float wave = m_sin((float)(v->phase[0] >> 8) * (2 * M_PI_F / 16777216));
        result = wave * v->env;
        if (s <= S_HT) {
            float square = wave * wave, fade = m_min(1, (float)v->age * R.k / 96);
            result += (.22f * (1 - 2 * square) * (.65f + .35f * v->level) + .14f * wave * (3 - 4 * square) * v->level) * v->env * fade;
            float l3 = v->level * v->level * v->level;
            result += .05f * svf(&v->filter[2], (m_noise(&v->random) * R.noise), 0) * l3 * l3;
        }
    } else if (s == S_RS || s == S_CL) {
        v->phase[0] += v->increment[0]; v->phase[1] += v->increment[1];
        float phase = (float)(v->phase[0] >> 8) * (2 * M_PI_F / 16777216);
        result = (m_sin(phase) + (s == S_RS ? .8f * m_sin((float)(v->phase[1] >> 8) * (2 * M_PI_F / 16777216)) : 0)) * v->env;
        if (s == S_RS) result += .16f * (m_noise(&v->random) * R.noise) * v->env;
    } else if (s == S_MA) {
        float attack = m_min(1, (float)(v->age + 1) * R.k / (1 + v->tone * 100));
        result = svf(&v->filter[2], (m_noise(&v->random) * R.noise), 2) * v->env * attack;
    } else if (s == S_CP) {
        if (v->age < R.ms30) { if (!(v->age % R.ms10)) v->pulse = 1; v->pulse *= R.cp_pulse; }
        else if (v->age == R.ms30) { v->env = 1; v->pulse = 0; }
        float e = (v->age >= R.ms30 ? v->env : 0) + v->pulse + .037f * v->level;
        result = svf(&v->filter[3], (m_noise(&v->random) * R.noise), 1) * e;
    } else if (s == S_CB) {
        v->phase[0] += v->increment[0]; v->phase[1] += v->increment[1];
        float square = (float)(v->phase[0] >> 31) + (float)(v->phase[1] >> 31) - 1;
        result = svf(&v->filter[0], square, 1) * v->env * (.24f + .76f * v->level * v->level * v->level);
    } else {
        unsigned total = 0; for (unsigned j = 0; j < 6; j++) { v->phase[j] += v->increment[j]; total += v->phase[j] >> 31; }
        float metal = .33f * total - 1;
        float colored = svf(&v->filter[2], metal, 1);
        float e = v->env;
        if (s == S_OH) { float u = (float)v->age * v->tail_step; e = m_max(0, 1 - u * u * u); e = e * e * m_min(1, (float)v->age * R.k / 400); }
        if (s == S_CH) { float u = (float)v->age * v->tail_step; e = m_max(0, 1 - u * u * u * u) * v->env; }
        if (s == S_CY) e = 1.2f * e + .23f * m_max(0, 1 - (float)v->age * v->tail_step);
        float swing = colored * (colored > 0 ? 10 : .1f); swing = swing / (1 + m_abs(swing));
        result = svf(&v->filter[0], svf(&v->filter[3], (swing + 1) * e, 2), 2);
    }
    v->age++; v->env *= v->decay; v->level *= v->transient_decay;
    return result;
}
unsigned pd_active(const struct pd_kit *d)
{
    return d->bd.active || d->sd.active || d->tom[0].active || d->tom[1].active || d->tom[2].active ||
        d->rs.active || d->cl.active || d->ma.active || d->cp.active || d->cb.active ||
        d->ch.active || d->oh.active || d->cy.active;
}
void pd_panic(struct pd_kit *d)
{
    for (unsigned s = 0; s < S_NUM; s++) voice_of(d, s)->active = 0;
}
void pd_render(struct pd_kit *d, float *l, unsigned count)
{
    static const uint8_t slots[13] = {S_BD, S_SD, S_LT, S_MT, S_HT, S_RS, S_CL, S_MA, S_CP, S_CB, S_CH, S_OH, S_CY};
    for (unsigned j = 0; j < 13; j++) {
        voice_t *v = voice_of(d, slots[j]);
        if (!v->active) continue;
        unsigned driving = d->pot[v->sound][P_DRIVE];
        for (unsigned i = 0; i < count; i++) {
            float x = percussion(v), a = m_abs(x); if (a > v->peak) v->peak = a;
            if (a < m_max(.00001f, v->peak * .001f)) v->quiet++; else v->quiet = 0;
            if ((v->quiet > R.ms20 && v->age > R.ms50) || v->age > R.life) { v->active = 0; break; }
            if (driving) { x *= v->drive; x = softclip(x * .85f); x *= v->drive_norm; }
            else x *= v->gain;
            if (v->gate && v->age > v->gate) {   /* note value reached: about 100 ms release */
                v->fade *= R.release; x *= v->fade;
                if (v->fade < .0005f) { v->active = 0; break; }
            }
            l[i] += x;
        }
    }
}
