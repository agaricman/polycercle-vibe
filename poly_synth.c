/* SPDX-License-Identifier: GPL-3.0-only
 * Polycercle: tonal voices for the sound banks after the 808 kit
 * (basses, new-age melodic percussion, world percussion, synth).
 *
 * One compact model per sound: a sine, triangle, saw or square oscillator
 * with phase-modulation FM, up to three partials (modal bars, bowls, bells,
 * detuned or chord oscillators), a pitch envelope, a resonant low-pass with
 * envelope and LFO, a band-pass noise burst that can ratchet, and soft drive.
 * Each circle owns two voices so that long tails overlap; bass sounds are
 * monophonic. Runs in the audio flow: no libc, no waits. VIBE FM: the times and
 * rates follow the host's sample rate (ps_rate); at 44.1 kHz they are the FM-1's. */
#include "poly.h"

/* ---- sound table ---- */
enum { W_SINE, W_TRI, W_SAW, W_SQR, W_SHAPE = 3, PW = 4, MONO = 8, NOTONE = 16 };
/* Times as 16 * log2(milliseconds): 0 = 1 ms, 160 = 1 s, 208 = 8 s. */
#define ILOG2(x) ((x) >= 32768 ? 15 : (x) >= 16384 ? 14 : (x) >= 8192 ? 13 : (x) >= 4096 ? 12 : (x) >= 2048 ? 11 : \
    (x) >= 1024 ? 10 : (x) >= 512 ? 9 : (x) >= 256 ? 8 : (x) >= 128 ? 7 : (x) >= 64 ? 6 : (x) >= 32 ? 5 : \
    (x) >= 16 ? 4 : (x) >= 8 ? 3 : (x) >= 4 ? 2 : (x) >= 2 ? 1 : 0)
#define MS(x) ((uint8_t)((x) < 1 ? 0 : 16 * ILOG2(x) + (((x) * 16) >> ILOG2(x)) - 16))
#define R(x) ((uint16_t)((x) * 256.0 + .5))      /* partial frequency ratio */
#define G(x) ((uint8_t)((x) * 255.0 + .5))       /* partial or noise level */
#define FMR(x) ((uint8_t)((x) * 16.0 + .5))      /* modulator ratio */
#define FMI(x) ((uint8_t)((x) * 16.0 + .5))      /* modulation index, radians */
#define Q(x) ((uint8_t)((x) * 16.0 + .5))        /* filter resonance */
#define NQ(x) ((uint8_t)((x) * 8.0 + .5))        /* noise band resonance */

struct ps_def {
    uint8_t note, wave, attack, decay;          /* MIDI note, W_* | flags, times */
    uint16_t ratio[3];                          /* partials (0 = unused) */
    uint8_t gain[3], pdecay;                    /* partial levels; decay x/64 of the body */
    uint8_t fm_ratio, fm_index, fm_decay;       /* phase modulation on the body */
    int8_t pitch; uint8_t pitch_time;           /* semitones at the strike, glide time */
    int8_t cutoff; uint8_t reso, fenv, fdecay;  /* low-pass: semitones above the note, Q, env */
    uint8_t noise, noise_note, noise_q, noise_decay;
    uint8_t lfo, lfo_depth;                     /* rate /8 Hz; depth: 0x80 | semitones to cutoff, else tremolo */
    uint8_t repeat, drive, level;               /* noise ratchet count<<4 | gap (2 ms steps); drive; trim /64 */
};

static const struct ps_def defs[PR_SYNTH_BANKS][PR_SOUNDS] = {
{   /* BASSES (C2) */
    {.note = 36, .wave = W_SINE | MONO, .attack = MS(3), .decay = MS(1500), .level = 72},
    {.note = 36, .wave = W_SINE | MONO, .attack = MS(1), .decay = MS(2200), .pitch = 12, .pitch_time = MS(220), .drive = 70, .level = 100},
    {.note = 36, .wave = W_SAW | MONO, .decay = MS(600), .cutoff = 12, .reso = Q(6), .fenv = 36, .fdecay = MS(260), .drive = 40},
    {.note = 36, .wave = W_SQR | MONO, .decay = MS(600), .cutoff = 10, .reso = Q(6), .fenv = 32, .fdecay = MS(260), .drive = 40},
    {.note = 36, .wave = W_SAW | PW | MONO, .attack = MS(2), .decay = MS(1000), .ratio = {R(.5), R(1.004)}, .gain = {G(.7), G(.5)},
     .cutoff = 7, .reso = Q(1.3), .fenv = 26, .fdecay = MS(400), .drive = 30, .level = 96},
    {.note = 36, .wave = W_SAW | PW | MONO, .attack = MS(5), .decay = MS(2600), .ratio = {R(1.0068), R(.9932)}, .gain = {G(.9), G(.9)},
     .cutoff = 18, .reso = Q(1), .lfo = 3, .lfo_depth = 0x80 | 6, .drive = 20, .level = 110},
    {.note = 36, .wave = W_SINE | MONO, .decay = MS(900), .fm_ratio = FMR(1), .fm_index = FMI(3.5), .fm_decay = MS(320)},
    {.note = 36, .wave = W_SINE | MONO, .decay = MS(650), .ratio = {R(2)}, .gain = {G(.35)}, .pdecay = 16,
     .fm_ratio = FMR(1), .fm_index = FMI(5), .fm_decay = MS(90), .noise = G(.45), .noise_note = 96, .noise_q = NQ(1), .noise_decay = MS(15)},
    {.note = 36, .wave = W_SAW | MONO, .attack = MS(1), .decay = MS(550), .cutoff = 5, .reso = Q(.9), .fenv = 30, .fdecay = MS(120)},
    {.note = 36, .wave = W_TRI | MONO, .attack = MS(4), .decay = MS(1300), .ratio = {R(2)}, .gain = {G(.15)}, .level = 80},
    {.note = 36, .wave = W_SAW | MONO, .attack = MS(3), .decay = MS(900), .cutoff = 10, .reso = Q(.8), .fenv = 12, .fdecay = MS(200),
     .noise = G(.15), .noise_note = 72, .noise_q = NQ(1), .noise_decay = MS(20)},
    {.note = 36, .wave = W_SINE | MONO, .attack = MS(8), .decay = MS(1600), .ratio = {R(2), R(3)}, .gain = {G(.3), G(.15)},
     .pitch = -1, .pitch_time = MS(200)},
    {.note = 36, .wave = W_SAW | PW | MONO, .attack = MS(5), .decay = MS(2200), .ratio = {R(1.005)}, .gain = {G(.7)},
     .cutoff = 10, .reso = Q(4), .lfo = 24, .lfo_depth = 0x80 | 24, .drive = 50, .level = 100},
    {.note = 36, .wave = W_SINE | MONO, .attack = MS(6), .decay = MS(2600), .ratio = {R(2), R(3)}, .gain = {G(.25), G(.1)},
     .pitch = 2, .pitch_time = MS(120), .cutoff = 20, .reso = Q(1), .drive = 30, .level = 90},
    {.note = 36, .wave = W_SQR | PW | MONO, .decay = MS(700), .ratio = {R(.5)}, .gain = {G(.5)},
     .cutoff = 14, .reso = Q(2), .fenv = 20, .fdecay = MS(180), .level = 52},
    {.note = 36, .wave = W_SINE | MONO, .decay = MS(1300), .pitch = 24, .pitch_time = MS(260), .drive = 50,
     .noise = G(.4), .noise_note = 100, .noise_q = NQ(.5), .noise_decay = MS(10), .level = 90},
},
{   /* NEW AGE */
    {.note = 72, .wave = W_SINE, .decay = MS(1800), .ratio = {R(5.95)}, .gain = {G(.45)}, .pdecay = 8,
     .fm_ratio = FMR(3), .fm_index = FMI(1.2), .fm_decay = MS(40), .noise = G(.1), .noise_note = 108, .noise_q = NQ(.5), .noise_decay = MS(5)},
    {.note = 57, .wave = W_SINE, .attack = MS(14), .decay = MS(9000), .ratio = {R(2.71), R(5.15), R(8.28)}, .gain = {G(.55), G(.3), G(.15)},
     .pdecay = 40, .lfo = 12, .lfo_depth = 40},
    {.note = 72, .wave = W_SINE, .decay = MS(6000), .ratio = {R(2), R(3), R(4.2)}, .gain = {G(.5), G(.4), G(.3)}, .pdecay = 32,
     .fm_ratio = FMR(3.5), .fm_index = FMI(2), .fm_decay = MS(60)},
    {.note = 84, .wave = W_SINE, .decay = MS(2500), .ratio = {R(2.76), R(5.4)}, .gain = {G(.35), G(.15)}, .pdecay = 24,
     .noise = G(.08), .noise_note = 110, .noise_q = NQ(.5), .noise_decay = MS(4)},
    {.note = 60, .wave = W_SINE, .attack = MS(2), .decay = MS(900), .ratio = {R(3.93), R(9.5)}, .gain = {G(.4), G(.15)}, .pdecay = 12,
     .noise = G(.12), .noise_note = 80, .noise_q = NQ(.5), .noise_decay = MS(10)},
    {.note = 72, .wave = W_SINE, .attack = MS(1), .decay = MS(450), .ratio = {R(3), R(6)}, .gain = {G(.45), G(.2)}, .pdecay = 20,
     .noise = G(.15), .noise_note = 100, .noise_q = NQ(.5), .noise_decay = MS(6)},
    {.note = 65, .wave = W_SINE, .attack = MS(2), .decay = MS(3500), .ratio = {R(4)}, .gain = {G(.25)}, .pdecay = 20,
     .fm_ratio = FMR(1), .fm_index = FMI(.4), .fm_decay = MS(40), .lfo = 44, .lfo_depth = 45},
    {.note = 62, .wave = W_SINE, .attack = MS(6), .decay = MS(3500), .ratio = {R(2), R(3), R(5.4)}, .gain = {G(.45), G(.25), G(.08)},
     .pdecay = 40, .fm_ratio = FMR(1), .fm_index = FMI(.8), .fm_decay = MS(100)},
    {.note = 72, .wave = W_SINE, .attack = MS(2), .decay = MS(1500), .ratio = {R(2), R(3), R(4.1)}, .gain = {G(.6), G(.25), G(.12)},
     .pdecay = 48, .fm_ratio = FMR(1), .fm_index = FMI(1.5), .fm_decay = MS(120)},
    {.note = 84, .wave = W_SINE, .attack = MS(1), .decay = MS(2200), .ratio = {R(4), R(6.3)}, .gain = {G(.2), G(.08)}, .pdecay = 16,
     .noise = G(.06), .noise_note = 115, .noise_q = NQ(.5), .noise_decay = MS(3)},
    {.note = 72, .wave = W_SINE, .decay = MS(1600), .ratio = {R(2), R(4)}, .gain = {G(.3), G(.2)}, .pdecay = 24,
     .fm_ratio = FMR(4), .fm_index = FMI(.8), .fm_decay = MS(30)},
    {.note = 60, .wave = W_SINE, .decay = MS(7000), .ratio = {R(.5), R(1.19), R(2)}, .gain = {G(.5), G(.6), G(.5)}, .pdecay = 56,
     .fm_ratio = FMR(2.8), .fm_index = FMI(1.5), .fm_decay = MS(150)},
    {.note = 48, .wave = W_SINE, .attack = MS(25), .decay = MS(8000), .ratio = {R(2.27), R(3.61)}, .gain = {G(.4), G(.25)},
     .fm_ratio = FMR(1.4), .fm_index = FMI(4), .fm_decay = MS(4000), .noise = G(.12), .noise_note = 70, .noise_q = NQ(1), .noise_decay = MS(400)},
    {.note = 60, .wave = W_SINE, .attack = MS(1), .decay = MS(2200), .ratio = {R(2), R(3)}, .gain = {G(.4), G(.2)}, .pdecay = 20,
     .fm_ratio = FMR(1), .fm_index = FMI(1.5), .fm_decay = MS(25)},
    {.note = 84, .wave = W_SINE, .attack = MS(4), .decay = MS(4000), .ratio = {R(2.32), R(4.25)}, .gain = {G(.4), G(.2)}, .pdecay = 40,
     .lfo = 20, .lfo_depth = 20},
    {.note = 48, .wave = W_SAW | PW, .attack = MS(700), .decay = MS(7000), .ratio = {R(1.006), R(2.003)}, .gain = {G(.9), G(.4)},
     .cutoff = 24, .reso = Q(.8), .lfo = 3, .lfo_depth = 0x80 | 8, .level = 100},
},
{   /* MONDE */
    {.note = 38, .wave = W_SINE, .decay = MS(450), .ratio = {R(1.59)}, .gain = {G(.2)}, .pdecay = 16, .pitch = 5, .pitch_time = MS(150),
     .noise = G(.12), .noise_note = 60, .noise_q = NQ(1), .noise_decay = MS(30), .drive = 30, .level = 96},
    {.note = 57, .wave = W_SINE, .decay = MS(300), .ratio = {R(1.59), R(2.14)}, .gain = {G(.4), G(.25)}, .pdecay = 24,
     .pitch = 2, .pitch_time = MS(80), .noise = G(.15), .noise_note = 84, .noise_q = NQ(2), .noise_decay = MS(15)},
    {.note = 64, .wave = W_SINE, .decay = MS(150), .ratio = {R(1.59), R(2.65)}, .gain = {G(.3), G(.2)}, .pdecay = 12,
     .pitch = 3, .pitch_time = MS(30), .noise = G(.6), .noise_note = 99, .noise_q = NQ(1.5), .noise_decay = MS(60)},
    {.note = 43, .wave = W_SINE, .decay = MS(500), .ratio = {R(1.59)}, .gain = {G(.3)}, .pdecay = 20, .pitch = 4, .pitch_time = MS(110),
     .noise = G(.08), .noise_note = 70, .noise_q = NQ(1), .noise_decay = MS(20)},
    {.note = 79, .wave = W_SINE, .decay = MS(90), .ratio = {R(1.59)}, .gain = {G(.5)}, .pdecay = 32,
     .noise = G(.5), .noise_note = 103, .noise_q = NQ(2), .noise_decay = MS(35)},
    {.note = 67, .wave = W_SINE, .decay = MS(700), .ratio = {R(2), R(3), R(4)}, .gain = {G(.5), G(.3), G(.2)}, .pdecay = 40,
     .fm_ratio = FMR(1), .fm_index = FMI(.6), .fm_decay = MS(50), .noise = G(.24), .noise_note = 96, .noise_q = NQ(1), .noise_decay = MS(10)},
    {.note = 40, .wave = W_SINE, .decay = MS(900), .ratio = {R(2)}, .gain = {G(.2)}, .pitch = -3, .pitch_time = MS(500),
     .noise = G(.1), .noise_note = 60, .noise_q = NQ(1), .noise_decay = MS(25)},
    {.note = 43, .wave = W_SINE, .decay = MS(250), .pitch = 6, .pitch_time = MS(90),
     .noise = G(.35), .noise_note = 92, .noise_q = NQ(1), .noise_decay = MS(120)},
    {.note = 45, .wave = W_SINE, .attack = MS(4), .decay = MS(600), .ratio = {R(1.5)}, .gain = {G(.3)}, .pitch = 7, .pitch_time = MS(320)},
    {.note = 69, .wave = W_SINE, .decay = MS(160), .ratio = {R(1.59)}, .gain = {G(.35)}, .pdecay = 20, .pitch = 1, .pitch_time = MS(40),
     .noise = G(.2), .noise_note = 96, .noise_q = NQ(.5), .noise_decay = MS(8)},
    {.note = 84, .wave = W_SINE, .decay = MS(70), .ratio = {R(2.6)}, .gain = {G(.3)}, .pdecay = 16,
     .noise = G(.24), .noise_note = 108, .noise_q = NQ(1), .noise_decay = MS(5)},
    {.note = 33, .wave = W_SINE, .decay = MS(1600), .ratio = {R(1.59)}, .gain = {G(.25)}, .pdecay = 20, .pitch = 4, .pitch_time = MS(260),
     .noise = G(.28), .noise_note = 55, .noise_q = NQ(.5), .noise_decay = MS(80), .drive = 40, .level = 100},
    {.note = 60, .wave = NOTONE, .noise = G(.55), .noise_note = 116, .noise_q = NQ(3), .noise_decay = MS(220), .repeat = 2 << 4 | 4},
    {.note = 60, .wave = NOTONE, .attack = MS(18), .noise = G(1), .noise_note = 112, .noise_q = NQ(.75), .noise_decay = MS(90), .level = 110},
    {.note = 79, .wave = W_SINE, .decay = MS(600), .ratio = {R(1.48), R(2.5)}, .gain = {G(.5), G(.3)}, .pdecay = 40,
     .fm_ratio = FMR(2.1), .fm_index = FMI(1), .fm_decay = MS(40)},
    {.note = 60, .wave = NOTONE, .noise = G(.7), .noise_note = 100, .noise_q = NQ(4), .noise_decay = MS(10), .repeat = 10 << 4 | 7},
},
{   /* SYNTH */
    {.note = 84, .wave = W_SQR, .decay = MS(110), .level = 48},
    {.note = 72, .wave = W_SINE, .decay = MS(220), .pitch = 36, .pitch_time = MS(160)},
    {.note = 84, .wave = W_SAW, .decay = MS(500), .pitch = 24, .pitch_time = MS(400), .cutoff = 12, .reso = Q(6), .fenv = 24, .fdecay = MS(300), .level = 24},
    {.note = 60, .wave = W_SAW | PW, .decay = MS(650), .ratio = {R(1.189), R(1.498), R(2)}, .gain = {G(1), G(1), G(.6)},
     .cutoff = 24, .reso = Q(1.5), .fenv = 24, .fdecay = MS(220)},
    {.note = 60, .wave = W_SAW | PW, .decay = MS(650), .ratio = {R(1.26), R(1.498), R(2)}, .gain = {G(1), G(1), G(.6)},
     .cutoff = 24, .reso = Q(1.5), .fenv = 24, .fdecay = MS(220)},
    {.note = 60, .wave = W_SAW | PW, .decay = MS(650), .ratio = {R(1.335), R(1.498), R(2)}, .gain = {G(1), G(1), G(.6)},
     .cutoff = 24, .reso = Q(1.5), .fenv = 24, .fdecay = MS(220)},
    {.note = 60, .wave = W_SAW | PW, .decay = MS(700), .ratio = {R(1.007)}, .gain = {G(.8)}, .cutoff = 6, .reso = Q(2), .fenv = 36, .fdecay = MS(150), .level = 44},
    {.note = 72, .wave = W_SINE, .decay = MS(3000), .fm_ratio = FMR(3.5), .fm_index = FMI(4), .fm_decay = MS(2000)},
    {.note = 60, .wave = W_SINE, .attack = MS(5), .decay = MS(1500), .ratio = {R(2), R(3), R(4)}, .gain = {G(.7), G(.5), G(.4)}, .level = 56},
    {.note = 72, .wave = W_SQR | PW, .decay = MS(900), .ratio = {R(1.005)}, .gain = {G(.7)}, .cutoff = 20, .reso = Q(2), .fenv = 12, .fdecay = MS(300),
     .level = 40},
    {.note = 60, .wave = W_SAW | PW, .attack = MS(25), .decay = MS(700), .ratio = {R(1.004)}, .gain = {G(.8)}, .cutoff = 4, .reso = Q(1.4), .fenv = 30,
     .fdecay = MS(250)},
    {.note = 60, .wave = NOTONE, .attack = MS(300), .noise = G(.7), .noise_note = 92, .noise_q = NQ(3), .noise_decay = MS(900), .level = 100},
    {.note = 72, .wave = W_SINE, .decay = MS(150), .pitch = -12, .pitch_time = MS(90)},
    {.note = 48, .wave = W_SAW | PW, .attack = MS(40), .decay = MS(3000), .ratio = {R(1.5), R(2.01)}, .gain = {G(.6), G(.5)},
     .cutoff = 12, .reso = Q(8), .lfo = 10, .lfo_depth = 0x80 | 30, .level = 32},
    {.note = 84, .wave = W_SINE, .decay = MS(120), .ratio = {R(2)}, .gain = {G(.2)}, .pitch = -7, .pitch_time = MS(50)},
    {.note = 24, .wave = W_SINE, .decay = MS(650), .pitch = 36, .pitch_time = MS(300), .drive = 80,
     .noise = G(.25), .noise_note = 110, .noise_q = NQ(.5), .noise_decay = MS(4), .level = 96},
},
};

/* ---- voices ---- */
enum { VOICES = 2 };
struct ps_voice {
    uint32_t ph[4], inc[4], base[4], mph, minc, mbase, lfo_ph, lfo_inc, rnd, age, repeat_at, repeat_gap, gate;
    float env, env_mul, att, att_step, penv[3], pmul[3], pgain[3];
    float fm, fm_mul, pitch, pitch_mul, fe, fe_mul, fenv, cut, g, gmax, r, low, band;
    float nenv, nmul, nlevel, ng, nr, nlow, nband, gain, drive, norm, tone;
    uint8_t active, wave, repeat_left, lfo_depth, filter, released;
    const struct ps_def *def;
};
static struct ps_voice voices[PR_RINGS][VOICES];
static float sine[1025];
/* the sample rate: samples per millisecond, phase per Hz, 1/rate, and the per-sample factors written for 44.1 kHz */
static struct { float sr, ms, q32, inv, bass_fade, release, noise; uint32_t ms2, life; } SR =
    {44100.0f, 44.1f, 4294967296.0f / 44100, 1.0f / 44100, .985f, .9985f, 1.0f / 2147483648.0f, 88, 44100u * 30u};

static inline float s_exp2(float x)
{
    if (x < -60) x = -60;
    if (x > 60) x = 60;
    int n = (int)(x + (x < 0 ? -0.5f : 0.5f));
    float a = (x - (float)n) * 0.6931471805599453f;
    float p = 1.0f + a * (1.0f + a * (0.5f + a * (1.0f / 6.0f + a * (1.0f / 24.0f + a / 120.0f))));
    union { float f; uint32_t u; } s; s.u = (uint32_t)(n + 127) << 23;
    return p * s.f;
}
static inline float sinq(uint32_t ph)
{
    uint32_t i = ph >> 22;
    float a = sine[i];
    return a + (sine[i + 1] - a) * (float)(int32_t)(ph & 0x3fffffu) * (1.0f / 4194304.0f);
}
static inline float blep(float t, float dt)
{
    dt = dt > 1e-6f ? dt : 1e-6f;   /* no division by zero, even computed ahead of the test */
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}
static inline float osc(unsigned wave, uint32_t ph, uint32_t inc)
{
    float t = (float)(int32_t)(ph >> 8) * (1.0f / 16777216.0f), dt = (float)(int32_t)(inc >> 8) * (1.0f / 16777216.0f);
    switch (wave & W_SHAPE) {
    case W_SINE: return sinq(ph);
    case W_TRI: return t < .5f ? 4 * t - 1 : 3 - 4 * t;
    case W_SAW: return t + t - 1 - blep(t, dt);
    default: {
        float u = t + .5f; if (u >= 1) u -= 1;
        return (t < .5f ? 1.0f : -1.0f) + blep(t, dt) - blep(u, dt);
    }
    }
}
/* -60 dB per `code` (16 * log2 ms), scaled. */
static float coef(unsigned code, float scale)
{
    float samples = SR.ms * s_exp2((float)code * (1.0f / 16)) * scale;
    return s_exp2(-9.9657842f / (samples < 1 ? 1 : samples));
}
static float hz(float note) { return 440.0f * s_exp2((note - 69) * (1.0f / 12)); }
/* Float to phase step, clamped before the conversion. */
/* The value is clamped before the conversion: converting an out-of-range float
 * is an invalid operation, and the compiler may convert before testing. */
static uint32_t step_of(float f)
{
    f *= SR.q32;
    f = f > 1.9e9f ? 0 : f; f = f < 0 ? 0 : f;
    return (uint32_t)(int32_t)pr_fix(f);
}
static uint32_t scale_step(uint32_t step, float m)
{
    float f = (float)(int32_t)(step >> 1) * 2 * m;
    f = f > 1.9e9f ? 1.9e9f : f; f = f < 0 ? 0 : f;
    return (uint32_t)(int32_t)pr_fix(f);
}
static float loudness(const struct ps_voice *v) { return (v->wave & NOTONE ? 0 : v->env) * v->gain + v->nenv; }
/* The filters are Chamberlin state-variable filters (hp = x - r*band - low;
 * band += g*hp; low += g*band). They are stable only while g*g + 2*r*g < 4:
 * past that the signal grows without bound and the FPU overflows. The largest
 * coefficient keeps a 10 % margin below that limit. */
static float g_limit(float r)
{
    float x = r * r + 4, y = x > 4 ? x : 4;
    for (unsigned k = 0; k < 4; ++k) y = .5f * (y + x / y);   /* sqrt, y >= 2 */
    return .9f * (y - r);
}
static void bandpass(float f, float q, float *g, float *r)
{
    float top = SR.sr >= 44100.0f ? 16000 : SR.sr * .3628f;
    float w = 3.14159265f * (f > top ? top : f) / SR.sr;
    *r = 1 / (q > .05f ? q : .05f);
    float c = w * (1 + w * w * (1.0f / 3 + w * w * (2.0f / 15))), m = g_limit(*r);
    *g = c < m ? c : m;
}

/* Once, from vibe_on_init. */
void ps_tables(void)
{
    for (unsigned i = 0; i <= 1024; ++i) {
        float x = (float)i * (6.28318531f / 1024), y = x, term = x;   /* Taylor series around 0..2pi, folded */
        if (x > 3.14159265f) { x -= 6.28318531f; y = x; term = x; }
        if (x > 1.57079633f) { x = 3.14159265f - x; y = x; term = x; }
        if (x < -1.57079633f) { x = -3.14159265f - x; y = x; term = x; }
        for (unsigned k = 1; k < 8; ++k) { term *= -x * x / (float)((2 * k) * (2 * k + 1)); y += term; }
        sine[i] = y;
    }
}
void ps_rate(float sr)
{
    if (!(sr >= 8000.0f && sr <= 384000.0f)) sr = 44100.0f;
    float k = 44100.0f / sr;
    SR.sr = sr; SR.ms = sr / 1000.0f; SR.q32 = 4294967296.0f / sr; SR.inv = 1.0f / sr;
    SR.bass_fade = k == 1.0f ? .985f : s_exp2(k * -0.02180437f);     /* log2(.985) */
    SR.release = k == 1.0f ? .9985f : s_exp2(k * -0.00216565f);      /* log2(.9985) */
    SR.ms2 = (uint32_t)(sr * .002f + .5f); SR.life = (uint32_t)sr * 30u;
    {   /* white noise keeps its level per Hz: x sqrt(sr / 44100) */
        float x = sr / 44100.0f, y = x > 1 ? x : 1;
        for (unsigned i = 0; i < 6; ++i) y = .5f * (y + x / y);
        SR.noise = (k == 1.0f ? 1.0f : y) * (1.0f / 2147483648.0f);
    }
}
void ps_init(void)
{
    for (unsigned r = 0; r < PR_RINGS; ++r) for (unsigned v = 0; v < VOICES; ++v) voices[r][v].rnd = 0x9e3779b9u * (r * VOICES + v + 1);
}
unsigned ps_melodic(unsigned bank) { return bank == 1 || bank == 2 || bank == 4; }
unsigned ps_base_note(unsigned bank, unsigned sound)
{
    return bank && bank <= PR_SYNTH_BANKS ? defs[bank - 1][sound % PR_SOUNDS].note : 60;
}

void ps_trigger(unsigned ring, unsigned bank, unsigned sound, float velocity,
    unsigned tune, unsigned decay, unsigned tone, unsigned drive, int semitones, uint32_t gate)
{
    if (ring >= PR_RINGS || !bank || bank > PR_SYNTH_BANKS || !(velocity > 0)) return;
    const struct ps_def *d = &defs[bank - 1][sound % PR_SOUNDS];
    struct ps_voice *vs = voices[ring], *v = &vs[0];
    for (unsigned i = 0; i < VOICES; ++i) {   /* a free voice, else the quietest one */
        if (!vs[i].active) { v = &vs[i]; break; }
        if (loudness(&vs[i]) < loudness(v)) v = &vs[i];
    }
    if (d->wave & MONO)                       /* basses: the previous note fades in 10 ms */
        for (unsigned i = 0; i < VOICES; ++i) if (&vs[i] != v && vs[i].active) { vs[i].env_mul = SR.bass_fade; vs[i].nenv = 0; }
    uint32_t rnd = v->rnd;
    { uint32_t *w = (uint32_t *)(void *)v; for (unsigned i = 0; i < sizeof *v / 4; ++i) w[i] = 0; }
    v->rnd = rnd ? rnd : 0x2545f491u;
    v->def = d; v->wave = d->wave; v->active = 1; v->gate = gate;
    float t = ((float)(int)(tone > 127 ? 127 : tone) - 64) * (1.0f / 64);       /* -1..1 brightness */
    float span = s_exp2(((float)(int)(decay > 127 ? 127 : decay) - 64) * (1.0f / 32));  /* x0.25..x4 */
    int shift = (int)(tune > 127 ? 127 : tune) - 64 + semitones;
    float note = (float)d->note + (float)shift;
    float vel = velocity > 1 ? 1 : velocity;
    float f = hz(note);
    v->tone = t;
    v->base[0] = step_of(f);
    float sum = d->wave & NOTONE ? 0 : 1;
    for (unsigned k = 0; k < 3; ++k) if (d->ratio[k] && d->gain[k]) {
        v->base[k + 1] = step_of(f * (float)d->ratio[k] * (1.0f / 256));
        float g = (float)d->gain[k] * (1.0f / 255) * s_exp2(t * (float)(k + 1) * .5f);
        if (!v->base[k + 1]) g = 0;
        v->pgain[k] = g; sum += g; v->penv[k] = 1;
        v->pmul[k] = coef(d->decay, span * (float)(d->pdecay ? d->pdecay : 64) * (1.0f / 64));
    }
    for (unsigned k = 0; k < 4; ++k) v->inc[k] = v->base[k];
    v->env = 1; v->env_mul = coef(d->decay, span);
    v->att = d->attack ? 0 : 1;
    v->att_step = d->attack ? 1 / (SR.ms * s_exp2((float)d->attack * (1.0f / 16))) : 1;
    if (d->fm_ratio) {
        v->mbase = v->minc = step_of(f * (float)d->fm_ratio * (1.0f / 16));
        float index = (float)d->fm_index * (1.0f / 16) * (.6f + .4f * vel) * s_exp2(t * 1.5f);
        v->fm = (index > 6 ? 6 : index) * 341782637.8f;    /* radians to half phase turns, Q31 */
        v->fm_mul = coef(d->fm_decay, span);
    }
    if (d->pitch) { v->pitch = (float)d->pitch; v->pitch_mul = coef(d->pitch_time, 1.0f / 16); }   /* per 16 samples */
    if (d->reso) {
        v->filter = 1; v->r = 16.0f / (float)(d->reso ? d->reso : 1); v->gmax = g_limit(v->r);
        v->cut = note + (float)d->cutoff + t * 24;
        v->fenv = (float)d->fenv * (.5f + .5f * vel); v->fe = 1; v->fe_mul = coef(d->fdecay, span);
    }
    if (d->lfo) { v->lfo_inc = step_of((float)d->lfo * .125f); v->lfo_depth = d->lfo_depth; }
    if (d->noise) {
        v->nlevel = (float)d->noise * (1.0f / 255) * s_exp2(t);
        v->nenv = v->nlevel;
        v->nmul = coef(d->noise_decay, d->wave & NOTONE ? span : 1);
        float q = (float)(d->noise_q ? d->noise_q : 8) * .125f;
        bandpass(hz((float)(d->noise_note + shift)), q, &v->ng, &v->nr);
        float root = q > 1 ? q : 1; for (unsigned k = 0; k < 3; ++k) root = .5f * (root + q / root);   /* sqrt(q) */
        v->nlevel *= 2 / root; v->nenv = v->nlevel;
        v->repeat_left = d->repeat >> 4;
        v->repeat_gap = SR.ms2 * ((d->repeat & 15u) + 1u);
        v->repeat_at = v->repeat_gap;
    }
    v->norm = (float)(d->level ? d->level : 64) * (1.0f / 64) * .7f / (sum > 1 ? sum : 1);
    v->gain = v->norm * (.3f + .7f * vel);
    v->drive = ((float)d->drive + (float)(drive > 127 ? 127 : drive)) * (1.0f / 32);
}

static void control(struct ps_voice *v)
{
    if (v->pitch != 0) {
        float m = s_exp2(v->pitch * (1.0f / 12));
        for (unsigned k = 0; k < 4; ++k) v->inc[k] = scale_step(v->base[k], m);
        v->minc = scale_step(v->mbase, m);
        v->pitch *= v->pitch_mul;
        if (v->pitch < .01f && v->pitch > -.01f) {
            v->pitch = 0;
            for (unsigned k = 0; k < 4; ++k) v->inc[k] = v->base[k];
            v->minc = v->mbase;
        }
    }
    float lfo = 0;
    if (v->lfo_inc) { lfo = sinq(v->lfo_ph); v->lfo_ph += v->lfo_inc * 16; }
    if (v->filter) {
        float n = v->cut + v->fenv * v->fe;
        if (v->lfo_depth & 0x80) n += lfo * (float)(v->lfo_depth & 127);
        float w = 3.14159265f * hz(n) * SR.inv;
        if (w > 1.2f) w = 1.2f;
        float g = w * (1 + w * w * (1.0f / 3 + w * w * (2.0f / 15)));
        v->g = g < v->gmax ? g : v->gmax;
    }
}

unsigned ps_active(unsigned ring)
{
    return ring < PR_RINGS && (voices[ring][0].active || voices[ring][1].active);
}
void ps_panic(unsigned ring)
{
    if (ring < PR_RINGS) for (unsigned v = 0; v < VOICES; ++v) voices[ring][v].active = 0;
}
void ps_render(unsigned ring, float *out, unsigned count)
{
    if (ring >= PR_RINGS) return;
    for (unsigned n = 0; n < VOICES; ++n) {
        struct ps_voice *v = &voices[ring][n];
        if (!v->active) continue;
        const unsigned wave = v->wave, tonal = !(wave & NOTONE), parts = wave & PW;
        const unsigned trem = v->lfo_inc && !(v->lfo_depth & 0x80);
        for (unsigned i = 0; i < count; ++i) {
            if (!(v->age & 15)) {
                control(v);
                if (v->gate && v->age >= v->gate && !v->released) {   /* note value reached: about 100 ms release */
                    const float rel = SR.release;
                    v->released = 1;
                    if (v->env_mul > rel) v->env_mul = rel;
                    if (v->nmul > rel) v->nmul = rel;
                    for (unsigned k = 0; k < 3; ++k) if (v->pmul[k] > rel) v->pmul[k] = rel;
                    v->repeat_left = 0;
                }
                if ((v->age > 2048 && loudness(v) < 2e-5f && !v->repeat_left && v->att >= 1) || v->age > SR.life) {
                    v->active = 0; break;
                }
            }
            float a = 1;
            if (v->att < 1) { v->att += v->att_step; a = v->att < 1 ? v->att : 1; }
            float x = 0;
            if (tonal) {
                uint32_t pm = 0;
                if (v->minc) { v->mph += v->minc; pm = (uint32_t)(int32_t)(sinq(v->mph) * v->fm) << 1; v->fm *= v->fm_mul; }
                v->ph[0] += v->inc[0];
                x = osc(wave, v->ph[0] + pm, v->inc[0]);
                for (unsigned k = 0; k < 3; ++k) {
                    if (v->pgain[k] == 0) continue;
                    v->ph[k + 1] += v->inc[k + 1];
                    x += v->pgain[k] * v->penv[k] * (parts ? osc(wave, v->ph[k + 1], v->inc[k + 1]) : sinq(v->ph[k + 1]));
                    v->penv[k] *= v->pmul[k];
                }
                x *= v->env;
                v->env *= v->env_mul;
            }
            if (v->nenv >= 2e-5f) {
                uint32_t r = v->rnd; r ^= r << 13; r ^= r >> 17; r ^= r << 5; v->rnd = r;
                float noise = (float)(int32_t)r * SR.noise;
                float hp = noise - v->nr * v->nband - v->nlow;
                v->nband += v->ng * hp; v->nlow += v->ng * v->nband;
                x += v->nband * v->nenv;
                v->nenv *= v->nmul;
            }
            if (v->repeat_left && v->age == v->repeat_at) { v->nenv = v->nlevel; --v->repeat_left; v->repeat_at += v->repeat_gap; }
            if (v->filter) {
                float hp = x - v->r * v->band - v->low;
                v->band += v->g * hp; v->low += v->g * v->band;
                x = v->low;
                v->fe *= v->fe_mul;
            }
            x *= a;
            if (trem) x *= 1 - (float)v->lfo_depth * (1.0f / 255) * (1 + sinq(v->lfo_ph + (v->age & 15) * v->lfo_inc));
            if (v->drive > 0) { x *= 1 + v->drive; x = x / (1 + (x < 0 ? -x : x)) * (1 + .25f * v->drive) / (1 + v->drive * .75f); }
            out[i] += x * v->gain;
            ++v->age;
        }
    }
}
