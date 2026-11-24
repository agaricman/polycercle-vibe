/* SPDX-License-Identifier: GPL-3.0-only
 * Polycercle for VIBE FM: circular polyrhythm sequencer.
 * Shared model between the control flow (events, display) and the audio flow.
 * Ported from Polycercle for the M-VAVE FM-1: the same patterns, songs and voices. */
#ifndef POLY_H
#define POLY_H
#include <stdint.h>
#include <stddef.h>
#include "vibe.h"

/* The SDK compiles these into every firmware (no libc headers in the freestanding build). */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
size_t strlen(const char *s);
char *strcpy(char *d, const char *s);

/* Division by a variable: a WebAssembly integer division by zero stops the firmware.
 * Every divisor that is not a constant goes through pr_nz() or pr_nzi(). */
static inline unsigned pr_nz(unsigned d) { return d ? d : 1u; }
static inline int pr_nzi(int d) { return d ? d : 1; }
/* (On the FM-1 this pinned a clamped float against early conversion; WebAssembly converts safely.) */
static inline float pr_fix(float f) { return f; }

enum {
    PR_RINGS = 8,       /* concentric circles (six on the FM-1) */
    PR_STEPS = 48,      /* steps per circle */
    PR_SCENES = 8,      /* stored scenes A..H */
    PR_CHAIN = 32,      /* song blocks */
    PR_SOUNDS = 16,     /* sounds per bank */
    PR_BANKS = 6,       /* 0 drums (808 kit), 1 basses, 2 new age, 3 world, 4 synth, 5 samples (IN 1) */
    PR_SYNTH_BANKS = 4, /* the synthesised banks 1..4 */
    PR_BANK_SAMPLE = 5,
    PR_EVENTS = 160     /* scheduled hits per audio block (up to 256 frames) */
};

/* Sounds follow the 808 model order. */
enum {
    PS_KICK, PS_SNARE, PS_TOM_LO, PS_TOM_MID, PS_TOM_HI, PS_CONGA_LO, PS_CONGA_MID, PS_CONGA_HI,
    PS_RIM, PS_CLAVE, PS_MARACAS, PS_CLAP, PS_COWBELL, PS_HAT_CL, PS_HAT_OP, PS_CYMBAL
};

/* Ring flags. */
enum { PR_MUTE = 1, PR_SOLO = 2 };

struct pr_ring {
    uint8_t sound;          /* 0..15 */
    uint8_t n;              /* 1..48 steps */
    uint8_t level;          /* 0..127 */
    uint8_t pan;            /* 0..127, 64 = centre */
    uint8_t flags;          /* PR_MUTE | PR_SOLO */
    uint8_t tune, decay, tone, drive; /* 0..127, 64 = sound default */
    uint8_t bank;           /* 0..PR_BANKS-1 */
    uint8_t midi[4];        /* MIDI out: [0] mode (PR_MIDI_*), [1] channel 0..15, [2] note (fixed, or the root of
                               the step notes), [3] flags (PR_MIDI_ONLY: Polycercle stays silent for this circle) */
    uint8_t steps[PR_STEPS];/* 0 off, 1 hit, 2 accent */
    uint8_t note[PR_STEPS]; /* per step: bits 0-4 transposition in semitones (-16..15), bits 5-7 length */
};
/* Step lengths: free (the sound's own decay), then note values of the beat. */
enum { PR_LEN_FREE, PR_LEN_16TH, PR_LEN_8TH, PR_LEN_QUARTER, PR_LEN_HALF, PR_LEN_WHOLE, PR_LENGTHS };
static inline int pr_note_pitch(uint8_t n) { int p = n & 31; return p & 16 ? p - 32 : p; }
static inline unsigned pr_note_len(uint8_t n) { return n >> 5; }
static inline uint8_t pr_note_make(int pitch, unsigned len) { return (uint8_t)(((unsigned)pitch & 31u) | (len << 5)); }

struct pr_pattern {
    uint16_t bpm10;         /* quarter-note tempo x10, 300..3000 */
    uint8_t beats;          /* beats per cycle, 1..16 */
    uint8_t swing;          /* 50..75 (% of a step pair) */
    uint8_t rings;          /* 1..PR_RINGS */
    uint8_t reserved[3];
    struct pr_ring ring[PR_RINGS];
};

struct pr_song {
    uint8_t length;         /* blocks in use, 0..PR_CHAIN */
    uint8_t loop;           /* 1 = repeat the song */
    uint8_t used;           /* bit per stored scene */
    uint8_t reserved;
    uint8_t scene[PR_CHAIN];
    uint8_t reps[PR_CHAIN]; /* cycles per block, 1..64 */
};

struct pr_state {
    struct pr_pattern cur;                  /* the circles being edited / played */
    struct pr_pattern scene[PR_SCENES];
    struct pr_song song;
};

/* ---- the sample rate (VIBE FM: the host's) ---- */
void pr_rate(float sample_rate);           /* vibe_on_init, before anything else */
extern float pr_sr;

/* ---- audio side (poly_engine.c) ---- */
/* Publish a complete state to the audio flow. 0 = accepted, 1 = the previous one is not picked up yet. */
int pr_publish(const struct pr_state *state);
/* Transport requests from the control flow. */
void pr_play(unsigned song, unsigned from_block);
void pr_stop(void);
void pr_song_mode(unsigned song);           /* what a DAW's or a clock's PLAY starts: the song or the circles */
void pr_clock(unsigned on);                 /* MIDI clock out at the own tempo */
void pr_panic(void);
/* Immediate one-shot (pads, keys); ring settings come from the current pattern. */
void pr_audition(unsigned ring, unsigned velocity_q7);
void pr_audition_step(unsigned ring, unsigned step, unsigned velocity_q7);
void pr_audition_note(unsigned ring, int semitones, unsigned velocity_q7);
/* Audio-owned status, read by the control flow. */
struct pr_status {
    uint32_t phase_hi;      /* whole cycles since start */
    uint32_t phase_lo;      /* fraction of the cycle, 2^32 = one cycle */
    uint32_t inc;           /* phase per frame (Q32 cycle) */
    uint32_t frame_lo;      /* vibe_frames() at the start of the block the phase belongs to */
    uint8_t playing, song, block, rep;
    uint8_t scene;          /* scene played in song mode, 255 outside a song */
    uint8_t finished;       /* a one-shot song reached its end */
    uint8_t host;           /* 1 = a DAW or a MIDI clock gives the tempo */
    uint8_t lead;           /* 1 = the host plays and Polycercle follows it (held: playing 0 – STOP while it plays) */
    uint8_t song_mode;      /* what PLAY from the host starts */
    uint16_t bpm10;         /* the tempo heard now (the host's while it leads) */
    uint16_t hits[PR_RINGS];/* increments on every hit, per ring */
    uint8_t last_step[PR_RINGS];
    float peak_l, peak_r;   /* output level of the last block */
    float in_peak;          /* IN 1 level of the last block, -1 while the input is off */
    uint8_t rec_state, rec_slot;
    uint32_t rec_len;
};
void pr_status_read(struct pr_status *status);
void pr_engine_init(void);
extern volatile float pr_volume;            /* master, 0..1.5 (control flow writes, audio reads) */
/* MIDI out, per circle (ring.midi): any device – a drum machine, a sampler, a synth, a DAW track.
 * PR_MIDI_NOTE plays one note on every hit (a drum sound, a sampler pad: GM drums on channel 10, a Roland P-6's
 * pads are notes 48-95 on channel 11); PR_MIDI_STEPS plays the root note plus each step's transposition, with the
 * step's length (a bass line, a melody). */
enum { PR_MIDI_OFF, PR_MIDI_NOTE, PR_MIDI_STEPS, PR_MIDI_MODES };
enum { PR_MIDI_ONLY = 1 };
/* The note a circle suggests for MIDI: the General MIDI drum note of an 808 sound, else the sound's own note. */
unsigned pr_midi_suggest(const struct pr_ring *r, unsigned *mode, unsigned *channel);
void pr_note_name(char *out, int note);          /* "C3" (MIDI 60 = C4) */
const char *pr_gm_drum_name(unsigned note);      /* "KICK" for 36, 0 if none */
/* The audio flow: one block, `frames` long (1..VIBE_MAX_BLOCK). */
void pr_render(const float *in, float *out_l, float *out_r, int frames);
void pr_rec_arm(unsigned slot);
void pr_rec_stop(void);
void pr_rec_release(void);

/* ---- drum voices (poly_drums.c) ---- */
struct pd_kit;
struct pd_kit *pd_kit_get(unsigned index);  /* one kit per circle */
void pd_kit_init(struct pd_kit *kit);
void pd_rate(float sample_rate);
/* semitones: per-step transposition; gate: frames before a fast release (0 = none). */
void pd_trigger(struct pd_kit *kit, unsigned sound, float velocity,
    unsigned tune, unsigned decay, unsigned tone, unsigned drive, int semitones, uint32_t gate);
void pd_render(struct pd_kit *kit, float *mono, unsigned count);
unsigned pd_active(const struct pd_kit *kit);
void pd_panic(struct pd_kit *kit);
extern const char *const pd_sound_names[PR_SOUNDS];
extern const char *const pd_sound_short[PR_SOUNDS];
extern const uint8_t pd_tone_default[PR_SOUNDS];
extern const uint8_t pd_decay_default[PR_SOUNDS];

/* ---- tonal sound banks 1..4 (poly_synth.c) ---- */
void ps_init(void);
void ps_rate(float sample_rate);
void ps_tables(void);
unsigned ps_melodic(unsigned bank);              /* tune shows a note name */
unsigned ps_base_note(unsigned bank, unsigned sound);
void ps_trigger(unsigned ring, unsigned bank, unsigned sound, float velocity,
    unsigned tune, unsigned decay, unsigned tone, unsigned drive, int semitones, uint32_t gate);
void ps_render(unsigned ring, float *mono, unsigned count);
unsigned ps_active(unsigned ring);
void ps_panic(unsigned ring);

/* ---- samples recorded from IN 1 (poly_sampler.c): bank SAMPLE, 16 slots ---- */
enum { SMP_SLOTS = 16, SMP_MAX = 192000 };          /* up to 4 s at 48 kHz, 2 s at 96 kHz */
extern int16_t smp_data[SMP_SLOTS][SMP_MAX];
extern volatile uint32_t smp_len[SMP_SLOTS];        /* 0 = empty; the control flow sets it last (release) */
extern float smp_rate[SMP_SLOTS];                   /* the rate it was recorded at */
void ss_init(void);
void ss_trigger(unsigned ring, unsigned slot, float velocity,
    unsigned tune, unsigned decay, unsigned tone, unsigned drive, int semitones, uint32_t gate);
void ss_render(unsigned ring, float *mono, unsigned count);
unsigned ss_active(unsigned ring);
void ss_panic(unsigned ring);
/* recording, audio flow: armed -> (the input crosses the threshold) recording -> done; the control flow then takes
 * the take and releases the buffer */
enum { REC_IDLE, REC_ARMED, REC_RECORDING, REC_DONE };
extern float smp_rec[SMP_MAX];
void ss_rec_arm(unsigned slot, float threshold);
void ss_rec_stop(void);
void ss_rec_release(void);
void ss_rec_block(const float *in, unsigned frames);
unsigned ss_rec_state(void);
unsigned ss_rec_slot(void);
uint32_t ss_rec_length(void);
float ss_in_peak(void);                             /* input level of the last block, 0..1 (-1: no input) */
/* control flow: the take into its slot – trimmed, normalised, faded; 0 = kept, -1 = nothing heard */
int ss_take(unsigned slot, uint32_t length, float rate);
/* control flow: a slot loaded from storage (int16 samples) */
void ss_set(unsigned slot, const int16_t *data, uint32_t length, float rate);

/* ---- shared helpers (poly_model.c) ---- */
extern const char *const pr_bank_names[PR_BANKS];
const char *pr_sound_name(unsigned bank, unsigned sound);
const char *pr_sound_short(unsigned bank, unsigned sound);
/* Decay and tone defaults of the ring's sound (drums have their own). */
void pr_sound_defaults(struct pr_ring *r);
/* Tune text: "+3" for drums and world percussion, a note name otherwise; with a step's transposition added. */
void pr_tune_text(char *out, const struct pr_ring *r, int semitones);
extern const char *const pr_length_names[PR_LENGTHS];
extern const char *const pr_length_short[PR_LENGTHS];
void pr_pattern_default(struct pr_pattern *p);
void pr_ring_default(struct pr_ring *r, unsigned sound, unsigned n);
void pr_state_demo(struct pr_state *s);
void pr_euclid(uint8_t *steps, unsigned hits, unsigned n);
unsigned pr_onsets(const struct pr_ring *r);
unsigned pr_is_euclid(const struct pr_ring *r);
void pr_rotate(struct pr_ring *r, int delta);
void pr_resize(struct pr_ring *r, unsigned n);
uint32_t pr_crc32(uint32_t crc, const void *data, size_t bytes);
unsigned pr_song_cycles(const struct pr_song *song);
/* Swing offset of step k in Q32 cycle units (0 when straight or odd n). */
uint32_t pr_swing_q32(unsigned swing, unsigned n, unsigned k);

/* Rhythm library: name, steps as '0'/'1'/'2' characters. */
struct pr_preset { const char *name; const char *steps; };
extern const struct pr_preset pr_presets[];
extern const unsigned pr_preset_count;
#endif
