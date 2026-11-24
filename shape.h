/* SPDX-License-Identifier: GPL-3.0-only
 * Polycercle: the geometry of a rhythm – analysis and generator (shape.c). */
#ifndef SHAPE_H
#define SHAPE_H
#include "poly.h"

enum { SHAPE_MAX_N = PR_STEPS, SHAPE_GEN_MAX_N = 20 };
/* properties asked of the generator */
enum { SHAPE_EVEN = 1, SHAPE_BALANCE = 2, SHAPE_AREA = 4, SHAPE_ODD = 8, SHAPE_OPEN = 16, SHAPE_CLOSE = 32, SHAPE_ALL = 63 };
enum { SHAPE_SYM_ANY, SHAPE_SYM_WITH, SHAPE_SYM_WITHOUT };
struct shape {
    unsigned n, k;
    float even;          /* 0..1, 1 = a Euclidean rhythm (maximally even) */
    float balance;       /* 0..1, 1 = the centre of gravity on the centre */
    float area;          /* polygon area / area of the regular k-gon */
    float cx, cy;        /* centre of gravity (unit circle, y up) */
    unsigned opposite;   /* pairs of hits facing each other */
    int axis;            /* axis of symmetry ×2 in steps (c: t -> c - t), -1 none */
    uint8_t euclid, balanced, large, odd, opening, closure, symmetric;
    unsigned score;      /* properties held, 0..7 */
};
void shape_analyse(const uint8_t *steps, unsigned n, unsigned beats, struct shape *s);
/* The best rhythms of k hits on n steps (n <= SHAPE_GEN_MAX_N) starting on step 0: masks, best first. */
unsigned shape_generate(unsigned n, unsigned k, unsigned beats, unsigned want, unsigned symmetry,
                        uint32_t *out, float *rating, unsigned max);
#endif
