/* SPDX-License-Identifier: GPL-3.0-only
 * Polycercle: pattern model, rhythm library and helpers (control flow). */
#include "poly.h"
#define PR_SIZE

const char *const pd_sound_names[PR_SOUNDS] = {
    "BASS DRUM", "SNARE DRUM", "LOW TOM", "MID TOM", "HIGH TOM", "LOW CONGA",
    "MID CONGA", "HIGH CONGA", "RIMSHOT", "CLAVES", "MARACAS", "HAND CLAP", "COWBELL",
    "CLOSED HAT", "OPEN HAT", "CYMBAL"};
const char *const pd_sound_short[PR_SOUNDS] = {
    "KICK", "SNARE", "TOM L", "TOM M", "TOM H", "CONGA L", "CONGA M", "CONGA H",
    "RIM", "CLAVES", "MARACAS", "CLAP", "COWBELL", "HAT C", "HAT O", "CYMBAL"};
const uint8_t pd_decay_default[PR_SOUNDS] = {87, 108, 68, 56, 53, 58, 36, 34, 40, 64, 51, 64, 62, 47, 74, 70};
const uint8_t pd_tone_default[PR_SOUNDS] = {42, 89, 64, 64, 64, 64, 64, 64, 64, 64, 32, 32, 64, 64, 64, 64};

const char *const pr_bank_names[PR_BANKS] = {"808 KIT", "BASS", "NEW AGE", "WORLD", "SYNTH", "SAMPLE"};
static const char *const sample_names[PR_SOUNDS][2] = {
    {"SAMPLE 1", "SMP 1"}, {"SAMPLE 2", "SMP 2"}, {"SAMPLE 3", "SMP 3"}, {"SAMPLE 4", "SMP 4"},
    {"SAMPLE 5", "SMP 5"}, {"SAMPLE 6", "SMP 6"}, {"SAMPLE 7", "SMP 7"}, {"SAMPLE 8", "SMP 8"},
    {"SAMPLE 9", "SMP 9"}, {"SAMPLE 10", "SMP 10"}, {"SAMPLE 11", "SMP 11"}, {"SAMPLE 12", "SMP 12"},
    {"SAMPLE 13", "SMP 13"}, {"SAMPLE 14", "SMP 14"}, {"SAMPLE 15", "SMP 15"}, {"SAMPLE 16", "SMP 16"}};
/* Tonal banks: full name (sound page, banners) and short name (corner, 8 chars). */
static const char *const bank_names[PR_SYNTH_BANKS][PR_SOUNDS][2] = {
    {{"SUB", "SUB"}, {"808", "808"}, {"ACID", "ACID"}, {"ACID SQUARE", "ACID SQ"}, {"MOOG", "MOOG"},
     {"REESE", "REESE"}, {"FM BASS", "FM BASS"}, {"SLAP", "SLAP"}, {"PICK", "PICK"}, {"ROUND", "ROUND"},
     {"UPRIGHT", "UPRIGHT"}, {"FRETLESS", "FRETLESS"}, {"WOBBLE", "WOBBLE"}, {"DUB", "DUB"},
     {"SYNTH BASS", "SYN BASS"}, {"BOOM", "BOOM"}},
    {{"KALIMBA", "KALIMBA"}, {"SINGING BOWL", "BOWL"}, {"TUBULAR BELL", "TUBULAR"}, {"GLOCKENSPIEL", "GLOCK"},
     {"MARIMBA", "MARIMBA"}, {"XYLOPHONE", "XYLO"}, {"VIBRAPHONE", "VIBRA"}, {"HANG", "HANG"},
     {"STEEL DRUM", "STEEL"}, {"MUSIC BOX", "MUSICBOX"}, {"CELESTA", "CELESTA"}, {"CHIMES", "CHIMES"},
     {"GONG", "GONG"}, {"HARP", "HARP"}, {"CRYSTAL", "CRYSTAL"}, {"PAD", "PAD"}},
    {{"DJEMBE BASS", "DJEM B"}, {"DJEMBE TONE", "DJEM T"}, {"DJEMBE SLAP", "DJEM S"}, {"DOUM", "DOUM"},
     {"TEK", "TEK"}, {"TABLA NA", "NA"}, {"TABLA GE", "GE"}, {"CAJON", "CAJON"}, {"UDU", "UDU"},
     {"BONGO", "BONGO"}, {"WOOD BLOCK", "WOODBLK"}, {"TAIKO", "TAIKO"}, {"TAMBOURINE", "TAMBOUR"},
     {"SHAKER", "SHAKER"}, {"AGOGO", "AGOGO"}, {"GUIRO", "GUIRO"}},
    {{"BLIP", "BLIP"}, {"ZAP", "ZAP"}, {"LASER", "LASER"}, {"MINOR CHORD", "MINOR"},
     {"MAJOR CHORD", "MAJOR"}, {"SUS CHORD", "SUS"}, {"PLUCK", "PLUCK"}, {"FM BELL", "FM BELL"},
     {"ORGAN", "ORGAN"}, {"LEAD", "LEAD"}, {"BRASS", "BRASS"}, {"BREATH", "BREATH"}, {"BUBBLE", "BUBBLE"},
     {"COSMIC", "COSMIC"}, {"DROP", "DROP"}, {"SYNTH KICK", "KICK SYN"}},
};
PR_SIZE const char *pr_sound_name(unsigned bank, unsigned sound)
{
    sound %= PR_SOUNDS;
    if (bank == PR_BANK_SAMPLE) return sample_names[sound][0];
    return bank && bank <= PR_SYNTH_BANKS ? bank_names[bank - 1][sound][0] : pd_sound_names[sound];
}
PR_SIZE const char *pr_sound_short(unsigned bank, unsigned sound)
{
    sound %= PR_SOUNDS;
    if (bank == PR_BANK_SAMPLE) return sample_names[sound][1];
    return bank && bank <= PR_SYNTH_BANKS ? bank_names[bank - 1][sound][1] : pd_sound_short[sound];
}
PR_SIZE void pr_sound_defaults(struct pr_ring *r)
{
    if (r->bank >= PR_BANKS) r->bank = 0;
    r->sound %= PR_SOUNDS;
    r->decay = r->bank ? 64 : pd_decay_default[r->sound];
    r->tone = r->bank ? 64 : pd_tone_default[r->sound];
}
const char *const pr_length_names[PR_LENGTHS] = {"FREE", "1/16", "1/8", "1/4", "1/2", "WHOLE"};
const char *const pr_length_short[PR_LENGTHS] = {"", "16", "8", "4", "2", "1"};
PR_SIZE void pr_tune_text(char *out, const struct pr_ring *r, int semitones)
{
    int t = (int)r->tune - 64 + semitones;
    if (!ps_melodic(r->bank)) {
        unsigned n = 0;
        if (t > 0) out[n++] = '+';
        if (t < 0) { out[n++] = '-'; t = -t; }
        if (t >= 10) out[n++] = (char)('0' + t / 10);
        out[n++] = (char)('0' + t % 10); out[n] = 0;
        return;
    }
    static const char names[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int note = (int)ps_base_note(r->bank, r->sound) + t;
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    unsigned n = 0;
    for (const char *c = names[note % 12]; *c; ++c) out[n++] = *c;
    int octave = note / 12 - 1;
    if (octave < 0) { out[n++] = '-'; octave = -octave; }
    out[n++] = (char)('0' + octave); out[n] = 0;
}

/* Same library as the Polycercle web version, plus a few circle classics. */
const struct pr_preset pr_presets[] = {
    {"TRESILLO", "10010010"}, {"CINQUILLO", "10110110"}, {"SON 3-2", "1001001000101000"},
    {"SON 2-3", "0010100010010010"}, {"RUMBA", "1001000100101000"}, {"BOSSA NOVA", "1001001000100100"},
    {"SHIKO", "1000101000101000"}, {"SOUKOUS", "1001001000110000"}, {"GAHU", "1001001000100010"},
    {"SAMBA", "1001010100101010"}, {"FUME-FUME", "101010010100"}, {"BEMBE", "101011010101"},
    {"CLAPPING MUSIC", "111011010110"}, {"AKSAK 2+2+3", "1010100"}, {"PULSE 2", "10001000"},
    {"PULSE 3", "100100100"}, {"PULSE 4", "1000100010001000"}, {"EIGHTHS", "11111111"},
    {"OFFBEATS", "01010101"}, {"SIXTEENTHS", "1111111111111111"}, {"TRIPLETS", "111111111111"},
    {"3 AGAINST 2", "111"}, {"5 PULSES", "11111"}, {"ROCK KICK", "10001100"}, {"ROCK SNARE", "00100010"},
    {"HOUSE HAT", "0010001000100010"}, {"BACKBEAT", "0101"}, {"BULERIA", "001001010101"},
};
const unsigned pr_preset_count = sizeof pr_presets / sizeof pr_presets[0];

PR_SIZE void pr_ring_default(struct pr_ring *r, unsigned sound, unsigned n)
{
    memset(r, 0, sizeof *r);
    r->sound = (uint8_t)(sound % PR_SOUNDS);
    r->n = (uint8_t)(n < 1 ? 1 : n > PR_STEPS ? PR_STEPS : n);
    r->level = 100; r->pan = 64; r->tune = 64;
    pr_sound_defaults(r);
    r->drive = 0;
}
PR_SIZE void pr_pattern_default(struct pr_pattern *p)
{
    static const uint8_t sounds[PR_RINGS] = {PS_KICK, PS_SNARE, PS_HAT_CL, PS_CLAP, PS_CONGA_MID, PS_COWBELL};
    memset(p, 0, sizeof *p);
    p->bpm10 = 1200; p->beats = 4; p->swing = 50; p->rings = 3;
    for (unsigned i = 0; i < PR_RINGS; i++) pr_ring_default(&p->ring[i], sounds[i], i == 2 ? 16 : 8);
}
PR_SIZE static void set_steps(struct pr_ring *r, const char *s)
{
    unsigned n = (unsigned)strlen(s);
    if (n > PR_STEPS) n = PR_STEPS;
    r->n = (uint8_t)n;
    memset(r->steps, 0, sizeof r->steps);
    for (unsigned k = 0; k < n; k++) r->steps[k] = (uint8_t)(s[k] == '2' ? 2 : s[k] == '1' ? 1 : 0);
}
/* ---- the groove heard at the first start ----
 * C minor pentatonic over a tresillo kick: a clap on 2 and 4, swung hats, a bass that follows the kick,
 * and two triplet layers (a marimba of 12 and a shaker of 6) that turn 3 against the 4 beats – the circles
 * at work. Scenes: A groove, B lift, C break (no kick, no clap), D a pure polyrhythm (3, 4, 5, 7) to explore.
 * Song: A x4, B x4, C x2, B x4, looping. */
struct demo_ring { uint8_t bank, sound, level, pan; const char *steps; const int8_t *pitch; const uint8_t *len; };
static void demo_set(struct pr_pattern *p, unsigned bpm10, unsigned swing, const struct demo_ring *d, unsigned rings)
{
    pr_pattern_default(p);
    p->bpm10 = (uint16_t)bpm10; p->swing = (uint8_t)swing; p->rings = (uint8_t)rings;
    for (unsigned i = 0; i < rings; i++) {
        struct pr_ring *r = &p->ring[i];
        pr_ring_default(r, d[i].sound, 8);
        r->bank = d[i].bank; pr_sound_defaults(r);
        set_steps(r, d[i].steps);
        r->level = d[i].level; r->pan = d[i].pan;
        for (unsigned k = 0, h = 0; k < r->n; k++) {
            if (!r->steps[k]) continue;
            r->note[k] = pr_note_make(d[i].pitch ? d[i].pitch[h] : 0, d[i].len ? d[i].len[h] : 0);
            h++;
        }
    }
}
PR_SIZE void pr_state_demo(struct pr_state *s)
{
    memset(s, 0, sizeof *s);
    enum { KIT = 0, BASS = 1, AGE = 2, WORLD = 3, L8 = PR_LEN_8TH, L16 = PR_LEN_16TH };
    static const int8_t mar_a[] = {0, 7, 3, 10, 12, 7, 3, 5}, mar_b[] = {12, 10, 7, 5, 3, 0, 3, 7};
    static const int8_t bass_a[] = {0, 0, -5, -2}, bass_b[] = {0, 0, -5, -2, 3};
    static const uint8_t bass_l[] = {L8, L8, L16, L16, L16};
    const struct demo_ring a[6] = {
        {KIT, PS_KICK, 112, 64, "2000001000100000", 0, 0},
        {KIT, PS_CLAP, 84, 60, "0101", 0, 0},
        {KIT, PS_HAT_CL, 46, 74, "12121212", 0, 0},
        {AGE, 4, 84, 80, "101101101101", mar_a, 0},
        {BASS, 0, 92, 64, "1000001000100010", bass_a, bass_l},
        {WORLD, 13, 44, 44, "211211", 0, 0},
    };
    struct demo_ring b[6], c[6];
    memcpy(b, a, sizeof a);
    b[0].steps = "2000001000100010";
    b[2].steps = "1121112111211121"; b[2].level = 40;
    b[3].steps = "110110110110"; b[3].pitch = mar_b;
    b[4].steps = "1000001000100110"; b[4].pitch = bass_b;
    memcpy(c, a, sizeof a);
    c[2].steps = "00100010"; c[3].level = 96; c[5].level = 58;
    static const struct demo_ring d[4] = {
        {KIT, PS_CLAVE, 92, 40, "211", 0, 0},
        {KIT, PS_COWBELL, 62, 88, "2111", 0, 0},
        {KIT, PS_RIM, 80, 64, "21111", 0, 0},
        {WORLD, 9, 84, 52, "2101101", 0, 0},
    };
    demo_set(&s->scene[0], 1040, 55, a, 6);
    demo_set(&s->scene[1], 1040, 55, b, 6);
    demo_set(&s->scene[2], 1040, 55, c, 6);
    s->scene[2].ring[0].flags = PR_MUTE; s->scene[2].ring[1].flags = PR_MUTE;
    demo_set(&s->scene[3], 960, 50, d, 4);
    s->song.used = 0x0f; s->song.loop = 1;
    static const uint8_t chain[] = {0, 1, 2, 1}, reps[] = {4, 4, 2, 4};
    s->song.length = 4;
    memcpy(s->song.scene, chain, sizeof chain); memcpy(s->song.reps, reps, sizeof reps);
    s->cur = s->scene[0];
}
PR_SIZE void pr_euclid(uint8_t *steps, unsigned k, unsigned n)
{
    /* Bresenham form of Bjorklund: identical onsets up to rotation, starts on a hit. */
    memset(steps, 0, PR_STEPS);
    if (!n) return;
    if (k > n) k = n;
    if (!k) return;
    for (unsigned i = 0; i < n; i++) {
        unsigned a = (i * k) % pr_nz(n);
        steps[i] = (uint8_t)(a < k);
    }
}
PR_SIZE unsigned pr_onsets(const struct pr_ring *r)
{
    unsigned c = 0;
    for (unsigned k = 0; k < r->n; k++) c += r->steps[k] != 0;
    return c;
}
PR_SIZE unsigned pr_is_euclid(const struct pr_ring *r)
{
    unsigned n = r->n, k = pr_onsets(r);
    if (k < 2 || k >= n) return 0;
    uint8_t e[PR_STEPS];
    pr_euclid(e, k, n);
    for (unsigned rot = 0; rot < n; rot++) {
        unsigned same = 1;
        for (unsigned i = 0; i < n && same; i++) same = (e[(i + rot) % pr_nz(n)] != 0) == (r->steps[i] != 0);
        if (same) return 1;
    }
    return 0;
}
PR_SIZE void pr_rotate(struct pr_ring *r, int delta)
{
    unsigned n = r->n;
    if (n < 2) return;
    uint8_t t[PR_STEPS];
    unsigned sn = pr_nz(n);
    int d = delta % (int)sn; if (d < 0) d += (int)n;
    for (unsigned k = 0; k < n; k++) t[(k + (unsigned)d) % sn] = r->steps[k];
    memcpy(r->steps, t, n);
    for (unsigned k = 0; k < n; k++) t[(k + (unsigned)d) % sn] = r->note[k];
    memcpy(r->note, t, n);
}
PR_SIZE void pr_resize(struct pr_ring *r, unsigned n)
{
    if (n < 1) n = 1;
    if (n > PR_STEPS) n = PR_STEPS;
    unsigned o = r->n;
    if (n == o) return;
    uint8_t t[PR_STEPS], u[PR_STEPS];
    memset(t, 0, sizeof t); memset(u, 0, sizeof u);
    unsigned so = pr_nz(o), sn = pr_nz(n);
    if (o && (n % so == 0 || o % sn == 0)) {
        /* Multiples keep the rhythm in time; onsets off the coarser grid drop. */
        for (unsigned k = 0; k < o && k < PR_STEPS; k++)
            if (r->steps[k] && (k * n) % so == 0) { t[k * n / so] = r->steps[k]; u[k * n / so] = r->note[k]; }
    } else {
        for (unsigned k = 0; k < o && k < n; k++) { t[k] = r->steps[k]; u[k] = r->note[k]; }
    }
    memcpy(r->steps, t, sizeof t); memcpy(r->note, u, sizeof u);
    r->n = (uint8_t)n;
}
PR_SIZE uint32_t pr_crc32(uint32_t crc, const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (bytes--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
PR_SIZE unsigned pr_song_cycles(const struct pr_song *song)
{
    unsigned c = 0;
    for (unsigned i = 0; i < song->length && i < PR_CHAIN; i++) c += song->reps[i];
    return c;
}
PR_SIZE uint32_t pr_swing_q32(unsigned swing, unsigned n, unsigned k)
{
    if (swing <= 50 || (n & 1u) || !(k & 1u) || !n) return 0;
    /* (swing/50 - 1)/n of a cycle, in Q32. */
    uint64_t num = (uint64_t)(swing - 50) << 32;
    return (uint32_t)(num / (50u * pr_nz(n)));
}

/* ---- MIDI out helpers ---- */
static const uint8_t gm_of_sound[PR_SOUNDS] = {36, 38, 45, 47, 50, 64, 63, 62, 37, 75, 70, 39, 56, 42, 46, 49};
PR_SIZE unsigned pr_midi_suggest(const struct pr_ring *r, unsigned *mode, unsigned *channel)
{
    if (!r->bank) { *mode = PR_MIDI_NOTE; *channel = 9; return gm_of_sound[r->sound % PR_SOUNDS]; }
    int note = (int)ps_base_note(r->bank, r->sound) + (int)r->tune - 64;
    *mode = PR_MIDI_STEPS; *channel = 0;
    return (unsigned)(note < 0 ? 0 : note > 127 ? 127 : note);
}
PR_SIZE void pr_note_name(char *out, int note)
{
    static const char names[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    note = note < 0 ? 0 : note > 127 ? 127 : note;
    unsigned n = 0;
    for (const char *c = names[note % 12]; *c; ++c) out[n++] = *c;
    int octave = note / 12 - 1;
    if (octave < 0) { out[n++] = '-'; octave = -octave; }
    out[n++] = (char)('0' + octave); out[n] = 0;
}
PR_SIZE const char *pr_gm_drum_name(unsigned note)
{
    static const char *const gm[47] = {
        "KICK 2", "KICK", "SIDE STICK", "SNARE", "CLAP", "SNARE 2", "LOW TOM 2", "CLOSED HAT", "LOW TOM", "PEDAL HAT",
        "MID TOM 2", "OPEN HAT", "MID TOM", "HIGH TOM 2", "CRASH", "HIGH TOM", "RIDE", "CHINA", "RIDE BELL", "TAMBOURINE",
        "SPLASH", "COWBELL", "CRASH 2", "VIBRASLAP", "RIDE 2", "HIGH BONGO", "LOW BONGO", "MUTE CONGA", "OPEN CONGA",
        "LOW CONGA", "HIGH TIMBALE", "LOW TIMBALE", "HIGH AGOGO", "LOW AGOGO", "CABASA", "MARACAS", "SHORT WHISTLE",
        "LONG WHISTLE", "SHORT GUIRO", "LONG GUIRO", "CLAVES", "HIGH WOOD", "LOW WOOD", "MUTE CUICA", "OPEN CUICA",
        "MUTE TRIANGLE", "OPEN TRIANGLE"};
    return note >= 35 && note <= 81 ? gm[note - 35] : 0;
}
