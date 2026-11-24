/* SPDX-License-Identifier: GPL-3.0-only
 * Polycercle: the geometry of a rhythm (G. Toussaint, "The Geometry of Musical Rhythm").
 *
 * A rhythm of k hits on a circle of n steps is a polygon inscribed in the circle.
 * Seven properties describe the timelines that people keep (son clave, bossa nova…):
 *   evenness   – the hits are spread as far apart as they can be (a Euclidean rhythm is the most even)
 *   balance    – the centre of gravity of the hits lies on the centre of the circle
 *   area       – the polygon is large (compared with the regular polygon of k corners)
 *   oddity     – no two hits face each other across the circle (the turn never splits in two equal halves)
 *   opening    – the second hit falls off the beat: tension
 *   closure    – the last hit falls on a beat: resolution
 *   symmetry   – the polygon has an axis of symmetry (bossa nova has one, soukous none)
 * shape_analyse() measures a circle; shape_generate() lists the rhythms of k hits on n steps that
 * start on the first step, best first, for the properties asked for. Control flow only. */
#include "shape.h"
#include "vibe_dsp.h"

static float chord_of(unsigned d, unsigned n) { return 2.0f * vd_sin(0.5f * (float)d / (float)n); }
static unsigned on_beat(unsigned t, unsigned n, unsigned beats) { return (t * beats) % (n ? n : 1u) == 0; }

/* Evenness: the sum of the chords between every pair of hits. */
static float evenness_of(const unsigned *t, unsigned k, unsigned n)
{
    float e = 0.0f;
    for (unsigned i = 0; i < k; ++i)
        for (unsigned j = i + 1; j < k; ++j) e += chord_of(t[j] - t[i], n);
    return e;
}
static float evenness_max(unsigned k, unsigned n)
{
    unsigned t[SHAPE_MAX_N], m = 0;
    for (unsigned i = 0; i < k; ++i) t[m++] = (i * n) / (k ? k : 1u);   /* Bjorklund's spacing, up to rotation */
    return evenness_of(t, m, n);
}
/* The axis of symmetry: t -> (c - t) mod n maps the hits onto themselves; c / 2 is the axis (in steps). */
static int axis_of(const uint8_t *on, unsigned n)
{
    for (unsigned c = 0; c < n; ++c) {
        unsigned ok = 1;
        for (unsigned t = 0; t < n && ok; ++t) if (on[t] && !on[(c + n - t) % n]) ok = 0;
        if (ok) return (int)c;
    }
    return -1;
}
void shape_analyse(const uint8_t *steps, unsigned n, unsigned beats, struct shape *s)
{
    memset(s, 0, sizeof *s);
    s->axis = -1;
    if (n < 2 || n > SHAPE_MAX_N) return;
    uint8_t on[SHAPE_MAX_N];
    unsigned t[SHAPE_MAX_N], k = 0;
    for (unsigned i = 0; i < n; ++i) { on[i] = steps[i] != 0; if (on[i]) t[k++] = i; }
    s->n = n; s->k = k;
    if (k < 2) return;
    /* evenness */
    float e = evenness_of(t, k, n), em = evenness_max(k, n);
    s->even = em > 0.0f ? e / em : 0.0f;
    if (s->even > 1.0f) s->even = 1.0f;
    s->euclid = s->even > 0.9995f;
    /* balance: 1 - the distance of the centre of gravity from the centre */
    float x = 0.0f, y = 0.0f;
    for (unsigned i = 0; i < k; ++i) { float a = (float)t[i] / (float)n; x += vd_sin(a); y += vd_sin(a + 0.25f); }
    s->cx = x / (float)k; s->cy = y / (float)k;
    float off = vd_sqrt(s->cx * s->cx + s->cy * s->cy);
    s->balance = 1.0f - off;
    s->balanced = off < 0.02f;
    /* area: shoelace on the unit circle, against the regular polygon of k corners */
    if (k >= 3) {
        float a = 0.0f;
        for (unsigned i = 0; i < k; ++i) { unsigned d = (t[(i + 1) % k] + n - t[i]) % n; a += vd_sin((float)d / (float)n); }
        float regular = (float)k * vd_sin(1.0f / (float)k);
        s->area = regular > 0.0f ? a / regular : 0.0f;
        s->large = s->area >= 0.85f;
    }
    /* oddity: no pair of hits facing each other (only an even n can be split in two) */
    s->opposite = 0;
    if (!(n & 1u)) for (unsigned i = 0; i < n / 2; ++i) if (on[i] && on[i + n / 2]) ++s->opposite;
    s->odd = s->opposite == 0;
    /* opening and closure: the second hit off the beat, the last one on a beat */
    if (!beats) beats = 4;
    s->opening = !on_beat(t[1], n, beats);
    s->closure = on_beat(t[k - 1], n, beats) && t[k - 1] != 0;
    s->axis = axis_of(on, n);
    s->symmetric = s->axis >= 0;
    s->score = s->euclid + s->balanced + s->large + s->odd + s->opening + s->closure + s->symmetric;
}

/* ---- generator ---- */
static float rate(const struct shape *s, unsigned want)
{
    float r = 0.0f;
    if (want & SHAPE_EVEN) r += s->even * s->even;
    if (want & SHAPE_BALANCE) r += s->balance;
    if (want & SHAPE_AREA) r += s->area;
    if (want & SHAPE_ODD) r += s->odd ? 1.0f : 0.0f;
    if (want & SHAPE_OPEN) r += s->opening ? 1.0f : 0.0f;
    if (want & SHAPE_CLOSE) r += s->closure ? 1.0f : 0.0f;
    return r;
}
unsigned shape_generate(unsigned n, unsigned k, unsigned beats, unsigned want, unsigned symmetry,
                        uint32_t *out, float *rating, unsigned max)
{
    if (n < 2 || n > SHAPE_GEN_MAX_N || k < 2 || k > n || !max) return 0;
    unsigned count = 0;
    uint8_t steps[SHAPE_MAX_N];
    /* every set of k - 1 hits among steps 1..n-1, the first hit on step 0 (Gosper's hack) */
    uint32_t m = (1u << (k - 1)) - 1u, top = 1u << (n - 1);
    struct shape s;
    while (m < top) {
        uint32_t mask = 1u | (m << 1);
        for (unsigned i = 0; i < n; ++i) steps[i] = (uint8_t)((mask >> i) & 1u);
        shape_analyse(steps, n, beats, &s);
        if (!(symmetry == SHAPE_SYM_WITH && !s.symmetric) && !(symmetry == SHAPE_SYM_WITHOUT && s.symmetric)) {
            float r = rate(&s, want);
            if (count < max || r > rating[count - 1]) {
                unsigned i = count < max ? count++ : max - 1;
                while (i && rating[i - 1] < r) { rating[i] = rating[i - 1]; out[i] = out[i - 1]; --i; }
                rating[i] = r; out[i] = mask;
            }
        }
        if (!m) break;
        uint32_t c = m & (0u - m), r2 = m + c;
        m = (((r2 ^ m) >> 2) / c) | r2;
    }
    return count;
}
