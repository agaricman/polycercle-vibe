/* SPDX-License-Identifier: GPL-3.0-only
 * Polycercle for VIBE FM – a circular polyrhythm sequencer.
 *
 * Up to eight concentric circles turn together: a circle of n steps spreads its hits
 * evenly over one turn of the hand, so 3 against 2, 5 against 4 or 7 against 8 play
 * by themselves. 80 sounds (an 808 kit, basses, new-age bells, world percussion,
 * synth), per-step notes and lengths, eight scenes chained into a song, MIDI out to
 * a sampler or a DAW track, and the DAW's (or a MIDI clock's) song position.
 *
 * Control flow (this file): events, display, storage, the DAW's parameters.
 * Audio flow (poly_engine.c): the circles and the voices, sample-exact.
 *
 *   K1 SOUND · K2 ENV · K3 MIX · K4 RHYTHM · K5 NOTES · K6 SEQ (SONG, MIDI, SLOTS on its soft keys)
 *   encoders 1-4: the four values above the soft keys   A: the circle   B: its sound
 *   pads: the steps of the circle (16 a page, - / + page through 48)   keys: play the circle
 *   PLAY / STOP: the transport   REC: keys record into the circle while it plays
 *   HOME: what every control does   DATA + ENTER: every control on the display (FOCUS)
 *
 * Ported from Polycercle for the M-VAVE FM-1 (same patterns, songs and voices). */
#include "poly.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"
#include "shape.h"

VIBE_PARALLEL_AUDIO

/* ---------- colours ---------- */
#define BG     0x0B0D12
#define FG     0xE6EAF2
#define DIM    0x8A93A6
#define MUTED  0x5B6577
#define CELL   0x141A26
#define CELL2  0x24314A
#define SEP    0x1C2333
#define RED    0xFF4D4D
#define GREEN  0x6EE7A8
#define DARK   0x111214
static const uint32_t RING_RGB[PR_RINGS] = {0xFF5A2E, 0x5282FF, 0xF6B400, 0xF03A9B, 0x18C4B4, 0x9C6EFF, 0x8FD14F, 0xF2F2F2};
static const uint32_t SCENE_RGB[PR_SCENES] = {0xFF5A2E, 0x5282FF, 0xF6B400, 0xF03A9B, 0x18C4B4, 0x9C6EFF, 0xFF8C14, 0x78C846};
/* the six colour keys (house style), each keeps its colour wherever it sits */
static const uint32_t KEY_RGB[6] = {0xB38CFF, 0xF5C93F, 0x6EE7A8, 0xFF6A1A, 0x3FA9F5, 0xFF5A8A};

enum { PG_SOUND, PG_ENV, PG_MIX, PG_RHYTHM, PG_NOTES, PG_SEQ, PG_SONG, PG_MIDI, PG_SLOTS, PG_SHAPE, PG_COUNT };
static const char *const PAGE_NAME[PG_COUNT] = {"SOUND", "ENV", "MIX", "RHYTHM", "NOTES", "SEQ", "SONG", "MIDI", "SLOTS", "SHAPE"};
static unsigned page_key(unsigned p) { return p < 6 ? p : p == PG_SHAPE ? 3 : 5; }
static uint32_t page_rgb(unsigned p) { return KEY_RGB[page_key(p)]; }

/* ---------- state (control flow) ---------- */
static float SR = 44100.0f;
static struct pr_state st;
static struct pr_pattern undo_pattern, presong;
static struct pr_ring clip_ring;
static int has_undo, has_clip, has_presong, song_was_playing;
static int publish_pending, unsaved, autosave_due;
static uint64_t changed_ms;
static unsigned sel, page = PG_RHYTHM, pad_page, cursor_step, sel_scene, cursor_block, sel_slot, cur_slot;
static int song_on, clock_out = 1, help, shift, rec, shown_scene = -1;
static float volume = 1.0f;
static unsigned preset_idx[PR_RINGS];
static const char *lib_name[PR_RINGS];
static int rot[PR_RINGS];
static struct pr_status stat;
static float enc_acc[VE_COUNT];
static uint64_t tap_ms[5]; static unsigned tap_count;
static uint64_t confirm_ms; static int confirm_what = -1;
static const char *toast_a; static char toast_b[40]; static uint64_t toast_ms;
/* SHAPE: the geometry of the circle's rhythm, and the best rhythms for the properties asked */
enum { CANDIDATES = 64 };
static unsigned shape_want = SHAPE_ALL, shape_sym = SHAPE_SYM_ANY, shape_k, cand_count, cand_idx, cand_n, cand_k, cand_valid;
static uint32_t cand[CANDIDATES]; static float cand_rating[CANDIDATES];
static uint16_t seen_hits[PR_RINGS]; static uint64_t flash_ms[PR_RINGS]; static uint8_t flash_step[PR_RINGS];

static struct pr_ring *ring_sel(void) { return &st.cur.ring[sel]; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint64_t now_ms(void) { return vibe_frames() * 1000u / (uint64_t)(SR > 1.0f ? SR : 44100.0f); }
static char *cat(char *d, const char *s) { size_t n = strlen(d); strcpy(d + n, s); return d; }
static char *catn(char *d, int v) { char t[16]; return cat(d, vg_itoa(v, t)); }
static char *signed_text(char *d, int v) { d[0] = 0; if (v > 0) cat(d, "+"); return catn(d, v); }
static void bpm_text(char *out, unsigned bpm10) { out[0] = 0; catn(out, (int)(bpm10 / 10)); if (bpm10 % 10) { cat(out, "."); catn(out, (int)(bpm10 % 10)); } }
static void toast(const char *a, const char *b) { toast_a = a; toast_b[0] = 0; if (b) { strcpy(toast_b, ""); cat(toast_b, b); } toast_ms = now_ms(); }

/* ---------- the DAW's parameters (permanent ids: automation hangs on them) ---------- */
enum { PA_TEMPO, PA_SWING, PA_BEATS, PA_VOLUME, PA_SELECT, PA_RING0, PA_PER_RING = 8, PA_COUNT = PA_RING0 + PR_RINGS * PA_PER_RING };
enum { RP_LEVEL, RP_PAN, RP_MUTE, RP_SOLO, RP_TUNE, RP_DECAY, RP_TONE, RP_DRIVE };
static const char *const ONOFF[2] = {"Off", "On"};
static const char *const CIRCLE_NAMES[PR_RINGS] = {"1", "2", "3", "4", "5", "6", "7", "8"};
#define RING_PARAMS(n) \
  { "mix_" #n "_level", "Level " #n, 0.0f, 1.0f, 100.0f / 127.0f, "", 0, 0 }, \
  { "mix_" #n "_pan", "Pan " #n, -1.0f, 1.0f, 0.0f, "", 0, 0 }, \
  { "mix_" #n "_mute", "Mute " #n, 0.0f, 1.0f, 0.0f, "", 2, ONOFF }, \
  { "mix_" #n "_solo", "Solo " #n, 0.0f, 1.0f, 0.0f, "", 2, ONOFF }, \
  { "c" #n "_tune", "Tune " #n, -64.0f, 63.0f, 0.0f, "st", 128, 0 }, \
  { "c" #n "_decay", "Decay " #n, -64.0f, 63.0f, 0.0f, "", 128, 0 }, \
  { "c" #n "_tone", "Tone " #n, -64.0f, 63.0f, 0.0f, "", 128, 0 }, \
  { "c" #n "_drive", "Drive " #n, 0.0f, 127.0f, 0.0f, "", 128, 0 }
static const vibe_param PARAMS[PA_COUNT] = {
  { "tempo", "Tempo", 30.0f, 300.0f, 120.0f, "BPM", 0, 0 },
  { "swing", "Swing", 50.0f, 75.0f, 50.0f, "%", 26, 0 },
  { "beats", "Beats per turn", 1.0f, 16.0f, 4.0f, "", 16, 0 },
  { "volume", "Volume", 0.0f, 150.0f, 100.0f, "%", 0, 0 },
  { "mix_select", "Circle", 1.0f, 8.0f, 1.0f, "", 8, CIRCLE_NAMES },
  RING_PARAMS(1), RING_PARAMS(2), RING_PARAMS(3), RING_PARAMS(4), RING_PARAMS(5), RING_PARAMS(6), RING_PARAMS(7), RING_PARAMS(8)
};
#define RING_GROUPS(n) "C" #n " · MIX", "C" #n " · MIX", "C" #n " · MIX", "C" #n " · MIX", \
                       "C" #n " · SOUND", "C" #n " · SOUND", "C" #n " · SOUND", "C" #n " · SOUND"
static const char *const GROUPS[PA_COUNT] = {
  "MAIN · SEQUENCER", "MAIN · SEQUENCER", "MAIN · SEQUENCER", "MAIN · OUTPUT", "MAIN · OUTPUT",
  RING_GROUPS(1), RING_GROUPS(2), RING_GROUPS(3), RING_GROUPS(4), RING_GROUPS(5), RING_GROUPS(6), RING_GROUPS(7), RING_GROUPS(8)
};
static const vibe_card CARDS[] = {
  { "SEQUENCER", 8, "one turn of the hand = beats per turn; every circle spreads its steps over it" },
  { "OUTPUT", 4, 0 }, { "MIX", 6, 0 }, { "SOUND", 6, "64 = the sound as designed" }
};
static float pval[PA_COUNT];

static float param_of(unsigned i)
{
    const struct pr_pattern *p = &st.cur;
    switch (i) {
    case PA_TEMPO: return (float)p->bpm10 * 0.1f;
    case PA_SWING: return (float)p->swing;
    case PA_BEATS: return (float)p->beats;
    case PA_VOLUME: return volume * 100.0f;
    case PA_SELECT: return (float)(sel + 1);
    }
    unsigned r = (i - PA_RING0) / PA_PER_RING, k = (i - PA_RING0) % PA_PER_RING;
    const struct pr_ring *q = &p->ring[r];
    switch (k) {
    case RP_LEVEL: return (float)q->level / 127.0f;
    case RP_PAN: return ((float)q->pan - 64.0f) / 63.0f;
    case RP_MUTE: return (q->flags & PR_MUTE) ? 1.0f : 0.0f;
    case RP_SOLO: return (q->flags & PR_SOLO) ? 1.0f : 0.0f;
    case RP_TUNE: return (float)((int)q->tune - 64);
    case RP_DECAY: return (float)((int)q->decay - 64);
    case RP_TONE: return (float)((int)q->tone - 64);
    default: return (float)q->drive;
    }
}
static void params_sync(void)
{
    for (unsigned i = 0; i < PA_COUNT; ++i) {
        float v = param_of(i);
        if (v != pval[i]) { pval[i] = v; vibe_param_set((int)i, v); }
    }
}

/* ---------- storage: the state, eight song slots, the settings (vibe_store_*) ---------- */
enum { STATE_MAGIC = 0x46564350u /* "PCVF" */, STATE_VERSION = 1, SLOTS = 8, SET_MAGIC = 0x53564350u };
struct file_head { uint32_t magic, version, size, crc; };
static struct { struct file_head h; struct pr_state s; } file_buf;
struct settings { uint32_t magic; float volume; uint8_t clock, song, slot, page, sel, reserved[3]; };
static uint8_t slot_used[SLOTS];
static void slot_name(char *n, unsigned i) { strcpy(n, "slot1.bin"); n[4] = (char)('1' + i % SLOTS); }

static void sanitize_pattern(struct pr_pattern *p)
{
    if (p->rings < 1) p->rings = 1;
    if (p->rings > PR_RINGS) p->rings = PR_RINGS;
    if (p->beats < 1 || p->beats > 16) p->beats = 4;
    if (p->swing < 50 || p->swing > 75) p->swing = 50;
    if (p->bpm10 < 300) p->bpm10 = 300;
    if (p->bpm10 > 3000) p->bpm10 = 3000;
    for (unsigned r = 0; r < PR_RINGS; ++r) {
        struct pr_ring *q = &p->ring[r];
        if (q->n < 1 || q->n > PR_STEPS) q->n = 8;
        q->sound %= PR_SOUNDS; if (q->bank >= PR_BANKS) q->bank = 0;
        if (q->midi[0] >= PR_MIDI_MODES) q->midi[0] = 0;
        q->midi[1] &= 15; q->midi[2] &= 127; q->midi[3] &= PR_MIDI_ONLY; q->level &= 127; q->pan &= 127; q->tune &= 127; q->decay &= 127; q->tone &= 127; q->drive &= 127;
        q->flags &= PR_MUTE | PR_SOLO;
        for (unsigned k = 0; k < PR_STEPS; ++k) {
            if (q->steps[k] > 2 || k >= q->n) q->steps[k] = k < q->n && q->steps[k] ? 1 : 0;
            if (pr_note_len(q->note[k]) >= PR_LENGTHS) q->note[k] &= 31;
        }
    }
}
static void sanitize(void)
{
    sanitize_pattern(&st.cur);
    for (unsigned i = 0; i < PR_SCENES; ++i) sanitize_pattern(&st.scene[i]);
    if (sel >= st.cur.rings) sel = st.cur.rings - 1u;
    if (pad_page * 16u >= ring_sel()->n) pad_page = 0;
    if (cursor_step >= ring_sel()->n) cursor_step = ring_sel()->n - 1u;
    if (st.song.length > PR_CHAIN) st.song.length = PR_CHAIN;
    for (unsigned b = 0; b < PR_CHAIN; ++b) { st.song.scene[b] %= PR_SCENES; if (!st.song.reps[b]) st.song.reps[b] = 1; if (st.song.reps[b] > 64) st.song.reps[b] = 64; }
    if (cursor_block >= st.song.length) cursor_block = st.song.length ? st.song.length - 1u : 0;
}
static int state_write(const char *name)
{
    file_buf.h.magic = STATE_MAGIC; file_buf.h.version = STATE_VERSION; file_buf.h.size = sizeof st;
    file_buf.s = st;
    file_buf.h.crc = pr_crc32(0, &file_buf.s, sizeof file_buf.s);
    return vibe_store_write(name, &file_buf, (int)sizeof file_buf);
}
static int state_read(const char *name, struct pr_state *out)
{
    int n = vibe_store_read(name, &file_buf, (int)sizeof file_buf);
    if (n != (int)sizeof file_buf || file_buf.h.magic != STATE_MAGIC || file_buf.h.size != sizeof *out ||
        file_buf.h.crc != pr_crc32(0, &file_buf.s, sizeof file_buf.s)) return -1;
    *out = file_buf.s;
    return 0;
}
static void settings_write(void)
{
    struct settings s; memset(&s, 0, sizeof s);
    s.magic = SET_MAGIC; s.volume = volume; s.clock = (uint8_t)clock_out; s.song = (uint8_t)song_on;
    s.slot = (uint8_t)cur_slot; s.page = (uint8_t)page; s.sel = (uint8_t)sel;
    vibe_store_write("settings.bin", &s, (int)sizeof s);
}
static void settings_read(void)
{
    struct settings s;
    if (vibe_store_read("settings.bin", &s, (int)sizeof s) != (int)sizeof s || s.magic != SET_MAGIC) return;
    volume = s.volume >= 0.0f && s.volume <= 1.5f ? s.volume : 1.0f;
    clock_out = s.clock != 0; song_on = s.song != 0; cur_slot = s.slot % SLOTS; sel_slot = cur_slot;
    page = s.page < PG_COUNT ? s.page : PG_RHYTHM; sel = s.sel % PR_RINGS;
}
static void slots_scan(void)
{
    char n[16];
    for (unsigned i = 0; i < SLOTS; ++i) { slot_name(n, i); slot_used[i] = vibe_store_size(n) == (int)sizeof file_buf; }
}
static void save_now(void) { if (state_write("state.bin") == 0) autosave_due = 0; settings_write(); }

/* ---------- editing ---------- */
static void publish(void) { if (publish_pending && !pr_publish(&st)) publish_pending = 0; }
static void checkpoint(void) { undo_pattern = st.cur; has_undo = 1; }
static void changed(void)
{
    sanitize();
    if (stat.playing && stat.song && shown_scene >= 0) st.scene[shown_scene] = st.cur;   /* edits go into the scene playing */
    publish_pending = 1; unsaved = 1; autosave_due = 1; changed_ms = now_ms();
    publish();
    params_sync();
}
static void audition(unsigned velocity) { publish(); pr_audition(sel, velocity); }
static void toggle_step(unsigned k, int accent)
{
    struct pr_ring *r = ring_sel();
    if (k >= r->n) return;
    checkpoint();
    uint8_t v = r->steps[k];
    r->steps[k] = (uint8_t)(accent ? (v == 2 ? 1 : 2) : (v ? 0 : accent < 0 ? 2 : 1));
    lib_name[sel] = 0;
    changed();
    if (r->steps[k] && !stat.playing) { publish(); pr_audition_step(sel, k, r->steps[k] == 2 ? 127 : 90); }
}
/* the phase heard at `frame` of the timeline: where a key was played */
static uint32_t phase_at(uint64_t frame)
{
    int64_t d = (int64_t)(uint32_t)((uint32_t)frame - stat.frame_lo);
    if (d > 0x7fffffff) d -= 0x100000000LL;
    return stat.phase_lo + (uint32_t)((int64_t)stat.inc * d);
}
static void record_hit(unsigned ring, int semitones, int accent, uint32_t phase)
{
    if (ring >= st.cur.rings) return;
    struct pr_ring *r = &st.cur.ring[ring];
    unsigned k = (unsigned)((((uint64_t)phase * r->n) + 0x80000000u) >> 32) % pr_nz(r->n);
    checkpoint();
    if (r->steps[k] != 2) r->steps[k] = (uint8_t)(accent ? 2 : 1);
    r->note[k] = pr_note_make(clampi(semitones, -16, 15), pr_note_len(r->note[k]));
    lib_name[ring] = 0;
    changed();
}
static void apply_preset(int delta)
{
    unsigned i = (unsigned)((int)preset_idx[sel] + delta + (int)pr_preset_count * 8) % pr_nz(pr_preset_count);
    preset_idx[sel] = i;
    const char *s = pr_presets[i].steps;
    struct pr_ring *r = ring_sel();
    checkpoint();
    unsigned n = (unsigned)strlen(s);
    r->n = (uint8_t)(n > PR_STEPS ? PR_STEPS : n);
    memset(r->steps, 0, sizeof r->steps);
    for (unsigned k = 0; k < r->n; ++k) r->steps[k] = (uint8_t)(s[k] == '2' ? 2 : s[k] == '1');
    pad_page = 0; rot[sel] = 0; lib_name[sel] = pr_presets[i].name;
    changed();
}
static void set_euclid(int hits)
{
    struct pr_ring *r = ring_sel();
    checkpoint();
    pr_euclid(r->steps, (unsigned)clampi(hits, 0, r->n), r->n);
    if (rot[sel]) pr_rotate(r, rot[sel]);
    lib_name[sel] = 0;
    changed();
}
static void tool(unsigned index)
{
    struct pr_ring *r = ring_sel();
    static const char *const names[6] = {"CLEARED", "INVERTED", "REVERSED", "SHUFFLED", "COPIED", "PASTED"};
    if (index == 5 && !has_clip) { toast("NOTHING COPIED", 0); return; }
    if (index == 4) { clip_ring = *r; has_clip = 1; toast("COPIED", "CIRCLE"); return; }
    checkpoint();
    unsigned n = r->n;
    if (index == 0) { memset(r->steps, 0, sizeof r->steps); memset(r->note, 0, sizeof r->note); }
    else if (index == 1) { for (unsigned k = 0; k < n; ++k) r->steps[k] = (uint8_t)!r->steps[k]; }
    else if (index == 2) {
        uint8_t t[PR_STEPS], u[PR_STEPS];
        for (unsigned k = 0; k < n; ++k) { t[k] = r->steps[(n - k) % pr_nz(n)]; u[k] = r->note[(n - k) % pr_nz(n)]; }
        memcpy(r->steps, t, n); memcpy(r->note, u, n);
    } else if (index == 3) {
        uint32_t seed = (uint32_t)vibe_frames() * 2654435761u + vd_rand();
        for (unsigned k = n - 1; k > 0; --k) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            unsigned j = seed % pr_nz(k + 1); uint8_t t = r->steps[k]; r->steps[k] = r->steps[j]; r->steps[j] = t;
            t = r->note[k]; r->note[k] = r->note[j]; r->note[j] = t;
        }
    } else if (index == 5) {
        uint8_t keep_midi[4]; memcpy(keep_midi, r->midi, 4);
        *r = clip_ring; memcpy(r->midi, keep_midi, 4);
    }
    lib_name[sel] = 0; rot[sel] = 0;
    toast(names[index], 0);
    changed();
}
static void set_sound(int delta)
{
    struct pr_ring *r = ring_sel();
    checkpoint();
    r->sound = (uint8_t)((r->sound + delta + PR_SOUNDS * 4) % PR_SOUNDS);
    pr_sound_defaults(r);
    changed();
    if (!stat.playing) audition(100);
}
static void set_bank(int delta)
{
    struct pr_ring *r = ring_sel();
    checkpoint();
    r->bank = (uint8_t)clampi((int)r->bank + delta, 0, PR_BANKS - 1);
    pr_sound_defaults(r);
    changed();
    if (!stat.playing) audition(100);
}
static void sound_reset(void)
{
    struct pr_ring *r = ring_sel();
    checkpoint();
    r->tune = 64; r->drive = 0; pr_sound_defaults(r);
    toast("SOUND AS DESIGNED", pr_sound_name(r->bank, r->sound));
    changed();
}
static void select_ring(int delta)
{
    sel = (unsigned)clampi((int)sel + delta, 0, (int)st.cur.rings - 1);
    pad_page = 0; cursor_step = 0;
    params_sync();
}
static void flag_toggle(unsigned r, unsigned flag)
{
    if (r >= st.cur.rings) return;
    st.cur.ring[r].flags ^= (uint8_t)flag;
    changed();
}
static void tap_tempo(void)
{
    uint64_t now = now_ms();
    if (tap_count && now - tap_ms[(tap_count - 1) % 5] > 2000u) tap_count = 0;
    tap_ms[tap_count % 5] = now; ++tap_count;
    unsigned used = tap_count < 5 ? tap_count : 5;
    if (used >= 2) {
        uint64_t interval = (tap_ms[(tap_count - 1) % 5] - tap_ms[(tap_count - used) % 5]) / pr_nz(used - 1);
        if (interval > 0) {
            checkpoint();
            st.cur.bpm10 = (uint16_t)clampi((int)(600000u / pr_nz((unsigned)interval)), 300, 3000);
            changed();
            char t[16]; bpm_text(t, st.cur.bpm10);
            toast("TAP TEMPO", t);
            return;
        }
    }
    toast("TAP TEMPO", "...");
}
static void undo(void)
{
    if (!has_undo) { toast("NOTHING TO UNDO", 0); return; }
    struct pr_pattern t = st.cur; st.cur = undo_pattern; undo_pattern = t;
    toast("UNDO", 0);
    changed();
}
/* a second press of the same key within 3 s confirms */
static int confirmed(int what, const char *ask)
{
    uint64_t now = now_ms();
    if (confirm_what == what && now - confirm_ms < 3000u) { confirm_what = -1; return 1; }
    confirm_what = what; confirm_ms = now;
    toast(ask, "PRESS AGAIN");
    return 0;
}
static void slot_save(unsigned i)
{
    char n[16]; slot_name(n, i);
    char t[24] = "SLOT "; catn(t, (int)i + 1);
    if (state_write(n) == 0) { slot_used[i] = 1; cur_slot = i; unsaved = 0; toast("SAVED", t); settings_write(); }
    else toast("COULD NOT SAVE", t);
}
static void slot_load(unsigned i)
{
    char n[16]; slot_name(n, i);
    char t[24] = "SLOT "; catn(t, (int)i + 1);
    struct pr_state s;
    if (state_read(n, &s)) { toast("EMPTY", t); return; }
    if (stat.playing) pr_stop();
    has_presong = 0; has_undo = 0;
    st = s; cur_slot = i;
    for (unsigned r = 0; r < PR_RINGS; ++r) { lib_name[r] = 0; rot[r] = 0; }
    changed(); unsaved = 0;
    toast("LOADED", t);
}

/* ---------- SHAPE: generate and apply ---------- */
static void shape_apply(unsigned index)
{
    struct pr_ring *r = ring_sel();
    if (index >= cand_count) return;
    checkpoint();
    for (unsigned k = 0; k < PR_STEPS; ++k) r->steps[k] = (uint8_t)(k < r->n && ((cand[index] >> k) & 1u) ? (k ? 1 : 2) : 0);
    cand_idx = index; lib_name[sel] = 0; rot[sel] = 0;
    changed();
}
static int shape_generate_now(void)
{
    struct pr_ring *r = ring_sel();
    if (r->n > SHAPE_GEN_MAX_N) { toast("THE GENERATOR GOES UP TO", "20 STEPS"); return -1; }
    if (shape_k < 2) shape_k = pr_onsets(r) >= 2 ? pr_onsets(r) : (r->n >= 5 ? 5 : r->n);
    if (shape_k > r->n) shape_k = r->n;
    cand_count = shape_generate(r->n, shape_k, st.cur.beats, shape_want, shape_sym, cand, cand_rating, CANDIDATES);
    cand_n = r->n; cand_k = shape_k; cand_idx = 0; cand_valid = 1;
    if (!cand_count) { toast("NO RHYTHM HAS THESE PROPERTIES", 0); return -1; }
    return 0;
}
static void shape_regenerate_apply(void) { if (!shape_generate_now()) shape_apply(0); }

/* ---------- the song ---------- */
static void song_edit(unsigned action)
{
    struct pr_song *s = &st.song;
    char t[2] = {(char)('A' + sel_scene), 0};
    if (action == 0) {          /* store the circles into the selected scene */
        unsigned was = (s->used >> sel_scene) & 1u;
        st.scene[sel_scene] = st.cur; s->used |= (uint8_t)(1u << sel_scene);
        if (!was && s->length < PR_CHAIN) { s->scene[s->length] = (uint8_t)sel_scene; s->reps[s->length] = 2; cursor_block = s->length++; }
        toast("SCENE STORED", t);
    } else if (action == 1) {   /* recall it */
        if (!((s->used >> sel_scene) & 1u)) { toast("EMPTY SCENE", t); return; }
        if (stat.playing && stat.song) { toast("THE SONG PLAYS", 0); return; }
        checkpoint(); st.cur = st.scene[sel_scene];
        toast("SCENE RECALLED", t);
    } else if (action == 2) {   /* insert it after the cursor */
        if (!((s->used >> sel_scene) & 1u)) { toast("EMPTY SCENE", "STORE IT FIRST"); return; }
        if (s->length >= PR_CHAIN) { toast("SONG FULL", "32 BLOCKS"); return; }
        unsigned at = s->length ? cursor_block + 1 : 0;
        for (unsigned i = s->length; i > at; --i) { s->scene[i] = s->scene[i - 1]; s->reps[i] = s->reps[i - 1]; }
        s->scene[at] = (uint8_t)sel_scene; s->reps[at] = 2; ++s->length; cursor_block = at;
    } else if (action == 3) {   /* delete the block */
        if (!s->length) return;
        for (unsigned i = cursor_block; i + 1 < s->length; ++i) { s->scene[i] = s->scene[i + 1]; s->reps[i] = s->reps[i + 1]; }
        --s->length;
    } else if (action == 4) {
        s->loop = !s->loop; toast("SONG LOOP", s->loop ? "ON" : "OFF");
    }
    changed();
}
static void song_mode_set(int on)
{
    song_on = on != 0;
    pr_song_mode((unsigned)song_on);
    toast(song_on ? "PLAY: THE SONG" : "PLAY: THE CIRCLES", song_on ? "SCENES IN ORDER" : "AS EDITED");
    settings_write();
}
static void play_toggle(void)
{
    if (stat.host && stat.playing) { toast("THE HOST LEADS", "START / STOP THERE"); return; }
    if (stat.playing) { pr_stop(); return; }
    publish();
    if (song_on) {
        if (!st.song.length) { toast("EMPTY SONG", "STORE A SCENE (SONG)"); return; }
        if (!has_presong) { presong = st.cur; has_presong = 1; }   /* the circles come back when the song stops */
        pr_play(1, shift && page == PG_SONG ? cursor_block : 0);
    } else pr_play(0, 0);
}

/* ---------- MIDI routing ---------- */
static const char *const MIDI_MODE[PR_MIDI_MODES] = {"OFF", "NOTE", "STEP NOTES"};
/* "C2 · CH 10 · KICK" – the note, its channel, and what it plays on a GM drum kit or a P-6 */
static void midi_text(char *t, const struct pr_ring *r, int long_form)
{
    if (!r->midi[0]) { strcpy(t, "OFF"); return; }
    char n[8]; pr_note_name(n, r->midi[2]);
    t[0] = 0;
    if (r->midi[0] == PR_MIDI_STEPS) cat(t, "STEPS ");
    cat(t, n); cat(t, " · CH "); catn(t, (r->midi[1] & 15) + 1);
    if (!long_form || r->midi[0] != PR_MIDI_NOTE) return;
    const char *gm = (r->midi[1] & 15) == 9 ? pr_gm_drum_name(r->midi[2]) : 0;
    if (gm) { cat(t, " · "); cat(t, gm); }
    else if (r->midi[2] >= 48 && r->midi[2] <= 95) {        /* a Roland P-6: bank A pad 1 .. bank H pad 6 */
        char pad[8] = "P-6 A1"; pad[4] = (char)('A' + (r->midi[2] - 48) / 6); pad[5] = (char)('1' + (r->midi[2] - 48) % 6);
        cat(t, " · "); cat(t, pad);
    }
}
static void midi_auto(struct pr_ring *r)
{
    unsigned mode, ch, note = pr_midi_suggest(r, &mode, &ch);
    r->midi[0] = (uint8_t)mode; r->midi[1] = (uint8_t)ch; r->midi[2] = (uint8_t)note;
}
/* a circle keeps its MIDI routing in every scene of the song */
static void routing_everywhere(void)
{
    for (unsigned k = 0; k < PR_SCENES; ++k)
        for (unsigned q = 0; q < PR_RINGS; ++q) memcpy(st.scene[k].ring[q].midi, st.cur.ring[q].midi, 4);
}

/* ---------- pages: the four values, the soft keys ---------- */
struct value { const char *label; char text[24]; float frac; };
static void value_of(unsigned i, struct value *v)
{
    struct pr_ring *r = ring_sel();
    const struct pr_pattern *p = &st.cur;
    v->label = 0; v->text[0] = 0; v->frac = -1;
    switch (page) {
    case PG_SOUND:
        if (i == 0) { v->label = "BANK"; strcpy(v->text, pr_bank_names[r->bank % PR_BANKS]); v->frac = (float)r->bank / (PR_BANKS - 1); }
        if (i == 1) { v->label = "SOUND"; strcpy(v->text, pr_sound_short(r->bank, r->sound)); v->frac = (float)r->sound / (PR_SOUNDS - 1); }
        if (i == 2) { v->label = "TUNE"; pr_tune_text(v->text, r, 0); v->frac = (float)r->tune / 127; }
        if (i == 3) { v->label = "LEVEL"; catn(v->text, (int)r->level * 100 / 127); cat(v->text, "%"); v->frac = (float)r->level / 127; }
        break;
    case PG_ENV:
        if (i == 0) { v->label = "DECAY"; signed_text(v->text, (int)r->decay - 64); v->frac = (float)r->decay / 127; }
        if (i == 1) { v->label = "TONE"; signed_text(v->text, (int)r->tone - 64); v->frac = (float)r->tone / 127; }
        if (i == 2) { v->label = "DRIVE"; catn(v->text, r->drive); v->frac = (float)r->drive / 127; }
        if (i == 3) { v->label = "PAN"; int d = (int)r->pan - 64; if (!d) strcpy(v->text, "C"); else { strcpy(v->text, d < 0 ? "L" : "R"); catn(v->text, d < 0 ? -d : d); } v->frac = (float)r->pan / 127; }
        break;
    case PG_MIX:
        if (i == 0) { v->label = "CIRCLE"; catn(v->text, (int)sel + 1); v->frac = (float)sel / (PR_RINGS - 1); }
        if (i == 1) { v->label = "LEVEL"; catn(v->text, (int)r->level * 100 / 127); cat(v->text, "%"); v->frac = (float)r->level / 127; }
        if (i == 2) { v->label = "PAN"; int d = (int)r->pan - 64; if (!d) strcpy(v->text, "C"); else { strcpy(v->text, d < 0 ? "L" : "R"); catn(v->text, d < 0 ? -d : d); } v->frac = (float)r->pan / 127; }
        if (i == 3) { v->label = "VOLUME"; catn(v->text, (int)(volume * 100.0f + .5f)); cat(v->text, "%"); v->frac = volume / 1.5f; }
        break;
    case PG_RHYTHM:
        if (i == 0) { v->label = "STEPS"; catn(v->text, r->n); v->frac = (float)r->n / PR_STEPS; }
        if (i == 1) { v->label = "HITS"; catn(v->text, (int)pr_onsets(r)); v->frac = (float)pr_onsets(r) / (float)pr_nz(r->n); }
        if (i == 2) { v->label = "ROTATE"; signed_text(v->text, rot[sel]); v->frac = -1; }
        if (i == 3) { v->label = "LIBRARY"; catn(v->text, (int)preset_idx[sel] + 1); cat(v->text, "/"); catn(v->text, (int)pr_preset_count); v->frac = (float)preset_idx[sel] / (float)(pr_preset_count - 1); }
        break;
    case PG_NOTES: {
        unsigned k = cursor_step < r->n ? cursor_step : 0;
        if (i == 0) { v->label = "STEP"; catn(v->text, (int)k + 1); cat(v->text, "/"); catn(v->text, r->n); v->frac = (float)k / (float)pr_nz(r->n - 1u); }
        if (i == 1) { v->label = "PITCH"; int pt = pr_note_pitch(r->note[k]); if (ps_melodic(r->bank)) pr_tune_text(v->text, r, pt); else signed_text(v->text, pt); v->frac = (float)(pt + 16) / 31; }
        if (i == 2) { v->label = "LENGTH"; strcpy(v->text, pr_length_names[pr_note_len(r->note[k]) % PR_LENGTHS]); v->frac = (float)pr_note_len(r->note[k]) / (PR_LENGTHS - 1); }
        if (i == 3) { v->label = "HIT"; static const char *const h[3] = {"OFF", "ON", "ACCENT"}; strcpy(v->text, h[r->steps[k] % 3]); v->frac = (float)r->steps[k] / 2; }
        break; }
    case PG_SEQ:
        if (i == 0) { v->label = "TEMPO"; bpm_text(v->text, p->bpm10); v->frac = (float)(p->bpm10 - 300) / 2700; }
        if (i == 1) { v->label = "BEATS / TURN"; catn(v->text, p->beats); v->frac = (float)p->beats / 16; }
        if (i == 2) { v->label = "SWING"; catn(v->text, p->swing); cat(v->text, "%"); v->frac = (float)(p->swing - 50) / 25; }
        if (i == 3) { v->label = "CIRCLES"; catn(v->text, p->rings); v->frac = (float)p->rings / PR_RINGS; }
        break;
    case PG_SONG: {
        const struct pr_song *s = &st.song;
        if (i == 0) { v->label = "BLOCK"; if (s->length) { catn(v->text, (int)cursor_block + 1); cat(v->text, "/"); catn(v->text, s->length); } else strcpy(v->text, "-"); }
        if (i == 1) { v->label = "ITS SCENE"; if (s->length) { char t[2] = {(char)('A' + s->scene[cursor_block] % PR_SCENES), 0}; strcpy(v->text, t); } else strcpy(v->text, "-"); }
        if (i == 2) { v->label = "TURNS"; if (s->length) { cat(v->text, "x"); catn(v->text, s->reps[cursor_block]); } else strcpy(v->text, "-"); }
        if (i == 3) { v->label = "SCENE"; char t[2] = {(char)('A' + sel_scene), 0}; strcpy(v->text, t); v->frac = (float)sel_scene / 7; }
        break; }
    case PG_MIDI:
        if (i == 0) { v->label = "CIRCLE"; catn(v->text, (int)sel + 1); v->frac = (float)sel / (PR_RINGS - 1); }
        if (i == 1) { v->label = "MIDI OUT"; strcpy(v->text, MIDI_MODE[r->midi[0] % PR_MIDI_MODES]); }
        if (i == 2) { v->label = "CHANNEL"; catn(v->text, (r->midi[1] & 15) + 1); v->frac = (float)(r->midi[1] & 15) / 15; }
        if (i == 3) { v->label = r->midi[0] == PR_MIDI_STEPS ? "ROOT NOTE" : "NOTE"; char n[8]; pr_note_name(n, r->midi[2]); strcpy(v->text, n); cat(v->text, " · "); catn(v->text, r->midi[2]); v->frac = (float)r->midi[2] / 127; }
        break;
    case PG_SHAPE:
        if (i == 0) { v->label = "STEPS"; catn(v->text, r->n); v->frac = (float)r->n / PR_STEPS; }
        if (i == 1) { v->label = "HITS"; catn(v->text, shape_k ? (int)shape_k : (int)pr_onsets(r)); v->frac = (float)(shape_k ? shape_k : pr_onsets(r)) / (float)pr_nz(r->n); }
        if (i == 2) { v->label = "BEST RHYTHMS"; if (cand_valid && cand_count && cand_n == r->n) { catn(v->text, (int)cand_idx + 1); cat(v->text, "/"); catn(v->text, (int)cand_count); v->frac = (float)cand_idx / (float)pr_nz(cand_count - 1u); } else strcpy(v->text, "TURN"); }
        if (i == 3) { static const char *const sym[3] = {"ANY", "WITH AXIS", "NO AXIS"}; v->label = "SYMMETRY"; strcpy(v->text, sym[shape_sym % 3]); }
        break;
    case PG_SLOTS:
        if (i == 0) { v->label = "SLOT"; catn(v->text, (int)sel_slot + 1); v->frac = (float)sel_slot / 7; }
        if (i == 1) { v->label = "IN IT"; strcpy(v->text, slot_used[sel_slot] ? "A SONG" : "EMPTY"); }
        if (i == 2) { v->label = "LAST USED"; catn(v->text, (int)cur_slot + 1); }
        if (i == 3) { v->label = "CHANGES"; strcpy(v->text, unsaved ? "NOT SAVED" : "SAVED"); }
        break;
    }
}
static void turn(unsigned i, int d)
{
    struct pr_ring *r = ring_sel();
    struct pr_pattern *p = &st.cur;
    if (!d) return;
    switch (page) {
    case PG_SOUND:
        if (i == 0) set_bank(d);
        if (i == 1) set_sound(d);
        if (i == 2) { r->tune = (uint8_t)clampi(r->tune + d, 0, 127); changed(); }
        if (i == 3) { r->level = (uint8_t)clampi(r->level + d * (shift ? 1 : 2), 0, 127); changed(); }
        break;
    case PG_ENV:
        if (i == 0) { r->decay = (uint8_t)clampi(r->decay + d * (shift ? 1 : 2), 0, 127); changed(); }
        if (i == 1) { r->tone = (uint8_t)clampi(r->tone + d * (shift ? 1 : 2), 0, 127); changed(); }
        if (i == 2) { r->drive = (uint8_t)clampi(r->drive + d * (shift ? 1 : 2), 0, 127); changed(); }
        if (i == 3) { r->pan = (uint8_t)clampi(r->pan + d * (shift ? 1 : 2), 0, 127); changed(); }
        break;
    case PG_MIX:
        if (i == 0) select_ring(d);
        if (i == 1) { r->level = (uint8_t)clampi(r->level + d * (shift ? 1 : 2), 0, 127); changed(); }
        if (i == 2) { r->pan = (uint8_t)clampi(r->pan + d * (shift ? 1 : 2), 0, 127); changed(); }
        if (i == 3) { volume = (float)clampi((int)(volume * 100.0f + .5f) + d * (shift ? 1 : 2), 0, 150) / 100.0f; pr_volume = volume; params_sync(); autosave_due = 1; changed_ms = now_ms(); }
        break;
    case PG_RHYTHM:
        if (i == 0) { checkpoint(); pr_resize(r, (unsigned)clampi(r->n + d, 1, PR_STEPS)); lib_name[sel] = 0; changed(); }
        if (i == 1) set_euclid((int)pr_onsets(r) + d);
        if (i == 2) { checkpoint(); pr_rotate(r, d); rot[sel] += d; if (r->n) rot[sel] %= (int)r->n; changed(); }
        if (i == 3) apply_preset(d);
        break;
    case PG_NOTES: {
        if (i == 0) { cursor_step = (unsigned)clampi((int)cursor_step + d, 0, r->n - 1); pad_page = cursor_step / 16; break; }
        unsigned k = cursor_step;
        if (k >= r->n) break;
        checkpoint();
        if (i == 1) { if (!r->steps[k]) r->steps[k] = 1; r->note[k] = pr_note_make(clampi(pr_note_pitch(r->note[k]) + d, -16, 15), pr_note_len(r->note[k])); }
        if (i == 2) { if (!r->steps[k]) r->steps[k] = 1; r->note[k] = pr_note_make(pr_note_pitch(r->note[k]), (unsigned)clampi((int)pr_note_len(r->note[k]) + d, 0, PR_LENGTHS - 1)); }
        if (i == 3) r->steps[k] = (uint8_t)clampi(r->steps[k] + d, 0, 2);
        lib_name[sel] = 0;
        changed();
        if (i == 1 && !stat.playing) { publish(); pr_audition_step(sel, k, r->steps[k] == 2 ? 127 : 100); }
        break; }
    case PG_SEQ:
        if (i == 0) { p->bpm10 = (uint16_t)clampi(p->bpm10 + d * (shift ? 1 : 10), 300, 3000); changed(); }
        if (i == 1) { p->beats = (uint8_t)clampi(p->beats + d, 1, 16); changed(); }
        if (i == 2) { p->swing = (uint8_t)clampi(p->swing + d, 50, 75); changed(); }
        if (i == 3) { p->rings = (uint8_t)clampi(p->rings + d, 1, PR_RINGS); changed(); }
        break;
    case PG_SONG: {
        struct pr_song *s = &st.song;
        if (i == 0 && s->length) cursor_block = (unsigned)clampi((int)cursor_block + d, 0, s->length - 1);
        if (i == 1 && s->length) { s->scene[cursor_block] = (uint8_t)((s->scene[cursor_block] + d + PR_SCENES * 8) % PR_SCENES); changed(); }
        if (i == 2 && s->length) { s->reps[cursor_block] = (uint8_t)clampi(s->reps[cursor_block] + d, 1, 64); changed(); }
        if (i == 3) sel_scene = (unsigned)clampi((int)sel_scene + d, 0, PR_SCENES - 1);
        break; }
    case PG_MIDI:
        if (i == 0) select_ring(d);
        if (i == 1) {
            int was = r->midi[0];
            r->midi[0] = (uint8_t)clampi(r->midi[0] + d, 0, PR_MIDI_MODES - 1);
            if (!was && r->midi[0]) { unsigned mode = r->midi[0]; midi_auto(r); r->midi[0] = (uint8_t)mode; }   /* a sensible note to start from */
            routing_everywhere(); changed();
        }
        if (i == 2) { r->midi[1] = (uint8_t)clampi((r->midi[1] & 15) + d, 0, 15); routing_everywhere(); changed(); }
        if (i == 3) { r->midi[2] = (uint8_t)clampi(r->midi[2] + d * (shift ? 12 : 1), 0, 127); routing_everywhere(); changed(); if (!stat.playing) audition(100); }
        break;
    case PG_SLOTS:
        if (i == 0) sel_slot = (unsigned)clampi((int)sel_slot + d, 0, SLOTS - 1);
        break;
    case PG_SHAPE:
        if (i == 0) { checkpoint(); pr_resize(r, (unsigned)clampi(r->n + d, 1, PR_STEPS)); lib_name[sel] = 0; cand_valid = 0; changed(); }
        if (i == 1) { shape_k = (unsigned)clampi((int)(shape_k ? shape_k : pr_onsets(r)) + d, 2, r->n); shape_regenerate_apply(); }
        if (i == 2) {
            if (!cand_valid || cand_n != r->n) { if (shape_generate_now()) break; shape_apply(0); break; }
            shape_apply((unsigned)clampi((int)cand_idx + d, 0, (int)cand_count - 1));
        }
        if (i == 3) { shape_sym = (unsigned)clampi((int)shape_sym + d, 0, 2); shape_regenerate_apply(); }
        break;
    }
}
static const char *softkey(unsigned i)
{
    static const char *const sound_keys[6] = {"PREV", "NEXT", "PLAY", "MUTE", "SOLO", "RESET"};
    static const char *const mix_keys[6] = {"PREV", "NEXT", "MUTE", "SOLO", "ALL ON", "PLAY"};
    static const char *const rhythm_keys[6] = {"CLEAR", "INVERT", "REVERSE", "SHUFFLE", "COPY", "PASTE"};
    static const char *const notes_keys[6] = {"PLAY", "RESET", "ALL 0", "PREV", "NEXT", "LEN ALL"};
    static const char *const seq_keys[6] = {"TAP", "SONG", "MIDI", "SLOTS", "UNDO", "SAVE"};
    static const char *const song_keys[6] = {"STORE", "RECALL", "INSERT", "DELETE", "LOOP", "MODE"};
    static const char *const midi_keys[6] = {"ONLY", "CLOCK", "PANIC", "TEST", "AUTO", "BACK"};
    static const char *const slot_keys[6] = {"LOAD", "SAVE", "DEMO", "", "", "BACK"};
    static const char *const shape_keys[6] = {"EVEN", "BALANCE", "AREA", "ODD", "OPENING", "CLOSURE"};
    switch (page) {
    case PG_SOUND: case PG_ENV: return sound_keys[i];
    case PG_MIX: return mix_keys[i];
    case PG_RHYTHM: return rhythm_keys[i];
    case PG_NOTES: return notes_keys[i];
    case PG_SEQ: return seq_keys[i];
    case PG_SONG: return song_keys[i];
    case PG_MIDI: return midi_keys[i];
    case PG_SHAPE: return shape_keys[i];
    default: return slot_keys[i];
    }
}
static int softkey_on(unsigned i)
{
    const struct pr_ring *r = ring_sel();
    switch (page) {
    case PG_SOUND: case PG_ENV: return (i == 3 && (r->flags & PR_MUTE)) || (i == 4 && (r->flags & PR_SOLO));
    case PG_MIX: return (i == 2 && (r->flags & PR_MUTE)) || (i == 3 && (r->flags & PR_SOLO));
    case PG_SONG: return (i == 4 && st.song.loop) || (i == 5 && song_on);
    case PG_MIDI: return (i == 0 && (r->midi[3] & PR_MIDI_ONLY)) || (i == 1 && clock_out);
    case PG_SHAPE: return (shape_want >> i) & 1u;
    }
    return 0;
}
static void press(unsigned i)
{
    struct pr_ring *r = ring_sel();
    switch (page) {
    case PG_SOUND: case PG_ENV:
        if (i == 0) select_ring(-1);
        if (i == 1) select_ring(1);
        if (i == 2) audition(110);
        if (i == 3) flag_toggle(sel, PR_MUTE);
        if (i == 4) flag_toggle(sel, PR_SOLO);
        if (i == 5) sound_reset();
        break;
    case PG_MIX:
        if (i == 0) select_ring(-1);
        if (i == 1) select_ring(1);
        if (i == 2) flag_toggle(sel, PR_MUTE);
        if (i == 3) flag_toggle(sel, PR_SOLO);
        if (i == 4) { for (unsigned q = 0; q < PR_RINGS; ++q) st.cur.ring[q].flags = 0; toast("ALL CIRCLES HEARD", 0); changed(); }
        if (i == 5) audition(110);
        break;
    case PG_RHYTHM: tool(i); break;
    case PG_NOTES: {
        unsigned k = cursor_step < r->n ? cursor_step : 0;
        if (i == 0) { publish(); pr_audition_step(sel, k, r->steps[k] == 2 ? 127 : 100); }
        if (i == 1) { checkpoint(); r->note[k] = 0; changed(); }
        if (i == 2) { checkpoint(); memset(r->note, 0, sizeof r->note); toast("NOTES RESET", "THE WHOLE CIRCLE"); changed(); }
        if (i == 3) select_ring(-1);
        if (i == 4) select_ring(1);
        if (i == 5) {   /* the cursor step's length on every step of the circle */
            checkpoint(); unsigned len = pr_note_len(r->note[k]);
            for (unsigned q = 0; q < r->n; ++q) r->note[q] = pr_note_make(pr_note_pitch(r->note[q]), len);
            toast("LENGTH ON ALL STEPS", pr_length_names[len % PR_LENGTHS]); changed();
        }
        break; }
    case PG_SEQ:
        if (i == 0) tap_tempo();
        if (i == 1) page = PG_SONG;
        if (i == 2) page = PG_MIDI;
        if (i == 3) { slots_scan(); sel_slot = cur_slot; page = PG_SLOTS; }
        if (i == 4) undo();
        if (i == 5) slot_save(cur_slot);
        break;
    case PG_SONG:
        if (i < 5) song_edit(i);
        else song_mode_set(!song_on);
        break;
    case PG_MIDI:
        if (i == 0) {
            r->midi[3] ^= PR_MIDI_ONLY; routing_everywhere(); changed();
            toast(r->midi[3] & PR_MIDI_ONLY ? "MIDI ONLY" : "MIDI + SOUND", r->midi[0] ? 0 : "TURN MIDI OUT ON");
        }
        if (i == 1) { clock_out = !clock_out; pr_clock((unsigned)clock_out); settings_write(); toast("MIDI CLOCK OUT", clock_out ? "ON" : "OFF"); }
        if (i == 2) { pr_panic(); toast("ALL NOTES OFF", 0); }
        if (i == 3) audition(110);
        if (i == 4) { midi_auto(r); routing_everywhere(); changed(); char t[40]; midi_text(t, r, 1); toast("MIDI", t); }
        if (i == 5) page = PG_SEQ;
        break;
    case PG_SLOTS:
        if (i == 0) { if (!unsaved || confirmed(10 + (int)sel_slot, "CHANGES NOT SAVED")) slot_load(sel_slot); }
        if (i == 1) { if (!slot_used[sel_slot] || sel_slot == cur_slot || confirmed(20 + (int)sel_slot, "REPLACE THIS SLOT?")) slot_save(sel_slot); }
        if (i == 2 && (!unsaved || confirmed(30, "CHANGES NOT SAVED"))) {
            if (stat.playing) pr_stop();
            has_presong = 0; checkpoint();
            pr_state_demo(&st);
            for (unsigned r = 0; r < PR_RINGS; ++r) { lib_name[r] = 0; rot[r] = 0; }
            changed(); unsaved = 1;
            toast("DEMO LOADED", "SAVE IT INTO A SLOT TO KEEP IT");
        }
        if (i == 5) page = PG_SEQ;
        break;
    case PG_SHAPE: {
        static const char *const names[6] = {"EVEN", "BALANCED", "LARGE AREA", "ODD", "OPENING", "CLOSURE"};
        shape_want ^= 1u << i;
        toast(names[i], (shape_want >> i) & 1u ? "WANTED" : "IGNORED");
        shape_regenerate_apply();
        break; }
    }
}

/* ---------- the song plays: the circles show its scene ---------- */
static void poll(void)
{
    pr_status_read(&stat);
    if (stat.playing && stat.song && stat.scene < PR_SCENES) {
        if (shown_scene != stat.scene) {
            shown_scene = stat.scene; st.cur = st.scene[shown_scene];
            sanitize(); publish_pending = 1; params_sync();
        }
    } else shown_scene = -1;
    if (stat.playing && stat.song) song_was_playing = 1;
    else if (song_was_playing) {
        song_was_playing = 0;
        if (has_presong) { st.cur = presong; has_presong = 0; changed(); }
    }
    for (unsigned r = 0; r < PR_RINGS; ++r)
        if (stat.hits[r] != seen_hits[r]) { seen_hits[r] = stat.hits[r]; flash_ms[r] = now_ms(); flash_step[r] = stat.last_step[r]; }
    publish();
    if (autosave_due && now_ms() - changed_ms > 1500u) save_now();
}

/* ---------- events ---------- */
static int detents(unsigned enc, float value)
{
    if (enc >= VE_COUNT) return 0;
    enc_acc[enc] += value;
    int d = (int)enc_acc[enc];
    enc_acc[enc] -= (float)d;
    return d;
}
static void param_in(unsigned i, float v)
{
    if (i >= PA_COUNT) return;
    struct pr_pattern *p = &st.cur;
    pval[i] = v;
    switch (i) {
    case PA_TEMPO: p->bpm10 = (uint16_t)clampi((int)(v * 10.0f + .5f), 300, 3000); break;
    case PA_SWING: p->swing = (uint8_t)clampi((int)(v + .5f), 50, 75); break;
    case PA_BEATS: p->beats = (uint8_t)clampi((int)(v + .5f), 1, 16); break;
    case PA_VOLUME: volume = vd_clamp(v * 0.01f, 0.0f, 1.5f); pr_volume = volume; return;
    case PA_SELECT: sel = (unsigned)clampi((int)(v + .5f) - 1, 0, (int)st.cur.rings - 1); return;
    default: {
        unsigned r = (i - PA_RING0) / PA_PER_RING, k = (i - PA_RING0) % PA_PER_RING;
        struct pr_ring *q = &p->ring[r];
        int iv = (int)(v < 0 ? v - .5f : v + .5f);
        if (k == RP_LEVEL) q->level = (uint8_t)clampi((int)(v * 127.0f + .5f), 0, 127);
        if (k == RP_PAN) q->pan = (uint8_t)clampi(64 + (int)(v * 63.0f + (v < 0 ? -.5f : .5f)), 0, 127);
        if (k == RP_MUTE) q->flags = (uint8_t)(v >= .5f ? q->flags | PR_MUTE : q->flags & ~PR_MUTE);
        if (k == RP_SOLO) q->flags = (uint8_t)(v >= .5f ? q->flags | PR_SOLO : q->flags & ~PR_SOLO);
        if (k == RP_TUNE) q->tune = (uint8_t)clampi(64 + iv, 0, 127);
        if (k == RP_DECAY) q->decay = (uint8_t)clampi(64 + iv, 0, 127);
        if (k == RP_TONE) q->tone = (uint8_t)clampi(64 + iv, 0, 127);
        if (k == RP_DRIVE) q->drive = (uint8_t)clampi(iv, 0, 127);
    } }
    changed();
}
static void note_in(const vibe_event *e)
{
    if (!e->down) return;
    unsigned vel = (unsigned)(e->value * 127.0f + .5f); if (vel < 1) vel = 1; if (vel > 127) vel = 127;
    int semis = (int)e->id - 60;
    if (page == PG_NOTES) {                       /* step entry: the note into the cursor step, then on to the next */
        struct pr_ring *r = ring_sel();
        unsigned k = cursor_step < r->n ? cursor_step : 0;
        checkpoint();
        if (!r->steps[k]) r->steps[k] = 1;
        r->note[k] = pr_note_make(clampi(semis, -16, 15), pr_note_len(r->note[k]));
        lib_name[sel] = 0;
        changed();
        publish(); pr_audition_step(sel, k, r->steps[k] == 2 ? 127 : vel);
        cursor_step = (k + 1) % pr_nz(r->n); pad_page = cursor_step / 16;
        return;
    }
    publish();
    pr_audition_note(sel, semis, vel);
    if (rec && stat.playing) {
        uint64_t heard;
        if (!vibe_event_heard(&heard)) heard = vibe_frames();
        record_hit(sel, semis, vel >= 110, phase_at(heard));
    }
}
static void pad_in(const vibe_event *e)
{
    if (!e->down || e->id >= 16) return;
    if (page == PG_SONG) {
        if (e->id < PR_SCENES) { sel_scene = e->id; if (!(stat.playing && stat.song) && ((st.song.used >> sel_scene) & 1u)) song_edit(1); }
        return;
    }
    if (page == PG_SLOTS) { if (e->id < SLOTS) sel_slot = e->id; return; }
    if (page == PG_MIX) {                          /* the pads as a mixer: top row(s) mute 1-8, bottom solo 1-8 */
        unsigned q = e->id % 8u;
        if (q < st.cur.rings) { sel = q; flag_toggle(q, e->id < 8 ? PR_MUTE : PR_SOLO); }
        return;
    }
    unsigned k = pad_page * 16u + e->id;
    if (page == PG_NOTES) { cursor_step = k < ring_sel()->n ? k : cursor_step; }
    toggle_step(k, shift ? 1 : (e->value >= .85f ? -1 : 0));
}
static void button_in(const vibe_event *e)
{
    if (e->id == VB_SHIFT) { shift = e->down; return; }
    if (!e->down) return;
    if (help && (e->id == VB_RETURN || e->id == VB_HOME)) { help = 0; return; }
    if (e->id >= VB_F1 && e->id <= VB_F6) { press((unsigned)(e->id - VB_F1)); return; }
    if (e->id >= VB_K1 && e->id <= VB_K6) {
        unsigned p = (unsigned)(e->id - VB_K1);
        if (p == PG_SEQ && (page == PG_SONG || page == PG_MIDI || page == PG_SLOTS)) page = PG_SEQ;
        else if (p == PG_RHYTHM && page == PG_RHYTHM) { page = PG_SHAPE; shape_k = 0; cand_valid = 0; }   /* K4 again: SHAPE */
        else if (p == PG_RHYTHM && page == PG_SHAPE) page = PG_RHYTHM;
        else page = p;
        help = 0;
        return;
    }
    struct pr_ring *r = ring_sel();
    switch (e->id) {
    case VB_HOME: help = 1; break;
    case VB_RETURN:
        if (page == PG_SONG || page == PG_MIDI || page == PG_SLOTS) page = PG_SEQ;
        if (page == PG_SHAPE) page = PG_RHYTHM;
        break;
    case VB_OCT_DOWN: tool(4); break;            /* copy the circle */
    case VB_OCT_UP: tool(5); break;              /* paste it into the selected one (its MIDI route stays) */
    case VB_PLAY: play_toggle(); break;
    case VB_STOP: if (stat.host && stat.playing) toast("THE HOST LEADS", 0); else pr_stop(); break;
    case VB_REC: rec = !rec; toast("RECORD", rec ? "KEYS INTO THE CIRCLE" : "OFF"); break;
    case VB_LEFT:
        if (page == PG_NOTES) { cursor_step = cursor_step ? cursor_step - 1 : r->n - 1u; pad_page = cursor_step / 16; }
        else if (page == PG_SONG) { if (cursor_block) --cursor_block; }
        else if (pad_page) --pad_page;
        break;
    case VB_RIGHT:
        if (page == PG_NOTES) { cursor_step = (cursor_step + 1) % pr_nz(r->n); pad_page = cursor_step / 16; }
        else if (page == PG_SONG) { if (cursor_block + 1 < st.song.length) ++cursor_block; }
        else if ((pad_page + 1) * 16u < r->n) ++pad_page;
        break;
    default: break;
    }
}
void vibe_on_event(const vibe_event *e)
{
    int id, f = vf_event(e, &id);
    if (f == VF_PRESS && id >= 100 && id < 100 + PR_RINGS) { if ((unsigned)(id - 100) < st.cur.rings) { sel = (unsigned)(id - 100); pad_page = 0; cursor_step = 0; params_sync(); } return; }
    if (f != VF_NO) { poll(); return; }
    pr_status_read(&stat);
    switch (e->type) {
    case VIBE_EV_NOTE: note_in(e); break;
    case VIBE_EV_PAD: pad_in(e); break;
    case VIBE_EV_BUTTON: button_in(e); break;
    case VIBE_EV_ENCODER: {
        int d = detents(e->id, e->value);
        if (!d) break;
        if (e->id <= VE_4) turn(e->id - VE_1, d);
        else if (e->id == VE_A) select_ring(d);
        else if (e->id == VE_B) set_sound(d);
        break; }
    case VIBE_EV_PARAM: param_in(e->id, e->value); break;
    case VIBE_EV_SAVE: save_now(); break;
    default: break;
    }
    poll();
}

/* ---------- drawing ---------- */
static int ring_heard(const struct pr_pattern *p, unsigned r)
{
    unsigned solo = 0;
    for (unsigned i = 0; i < p->rings; ++i) solo |= p->ring[i].flags & PR_SOLO;
    if (p->ring[r].flags & PR_MUTE) return 0;
    return !solo || (p->ring[r].flags & PR_SOLO);
}
static uint32_t phase_now(void)
{
    if (!stat.playing) return 0;
    return phase_at(vibe_frames());
}
/* the circles: one orbit per circle, a dot per step, the hand; scale 1 on the display, more on the stage */
static void draw_circles(float cx, float cy, float outer, float scale)
{
    const struct pr_pattern *p = &st.cur;
    unsigned rings = p->rings;
    float gap = rings > 1 ? (outer - 20.0f * scale) / (float)(rings - 1) : 0.0f;
    if (gap > 17.0f * scale) gap = 17.0f * scale;
    uint64_t now = now_ms();
    float hand = (float)phase_now() * (1.0f / 4294967296.0f);
    for (unsigned r = 0; r < rings; ++r) {
        const struct pr_ring *q = &p->ring[r];
        float rad = outer - gap * (float)r;
        int heard = ring_heard(p, r);
        uint32_t col = RING_RGB[r];
        uint32_t orbit = vg_mix(BG, col, r == sel ? 0.75f : heard ? 0.3f : 0.12f);
        float px = cx, py = cy - rad;
        for (unsigned s = 1; s <= 72; ++s) {
            float t = (float)s / 72.0f, x = cx + vd_sin(t) * rad, y = cy - vd_sin(t + 0.25f) * rad;
            vg_line(px, py, x, y, r == sel ? 1.6f * scale : 1.0f * scale, orbit);
            px = x; py = y;
        }
        unsigned n = q->n ? q->n : 1;
        float spacing = 6.2831853f * rad / (float)n;
        float d = spacing * 0.3f;
        if (d > 5.0f * scale) d = 5.0f * scale;
        if (d < 1.6f * scale) d = 1.6f * scale;
        int flashing = stat.playing && now - flash_ms[r] < 140u;
        for (unsigned k = 0; k < n; ++k) {
            float t = (float)k / (float)n;
            if (p->swing > 50 && !(n & 1u) && (k & 1u)) t += (float)(p->swing - 50) / 50.0f / (float)n;
            float x = cx + vd_sin(t) * rad, y = cy - vd_sin(t + 0.25f) * rad;
            uint8_t v = q->steps[k];
            if (!v) { vg_circle(x, y, d * 0.42f, vg_mix(BG, col, heard ? 0.35f : 0.15f)); continue; }
            uint32_t c = heard ? (v == 2 ? vg_mix(col, 0xFFFFFF, 0.35f) : col) : vg_mix(BG, col, 0.3f);
            float size = v == 2 ? d * 1.3f : d;
            if (flashing && flash_step[r] == k && heard) { c = 0xFFFFFF; size *= 1.35f; }
            vg_circle(x, y, size, c);
        }
    }
    if (stat.playing) {
        float hx = cx + vd_sin(hand) * (outer + 6.0f * scale), hy = cy - vd_sin(hand + 0.25f) * (outer + 6.0f * scale);
        vg_line(cx, cy, hx, hy, 2.0f * scale, 0xF2F2F2);
    } else vg_line(cx, cy, cx, cy - outer - 6.0f * scale, 1.5f * scale, MUTED);
    vg_circle(cx, cy, 4.0f * scale, stat.playing ? 0xF2F2F2 : MUTED);
}
static void draw_header(void)
{
    for (unsigned i = 0; i < 6; ++i) {
        int x = 4 + (int)i * 59, on = page_key(page) == i;
        vg_round(x, 5, 56, 24, 5, on ? KEY_RGB[i] : CELL);
        vg_text(PAGE_NAME[i], x + 28, 9, VG_BOLD, on ? DARK : vg_mix(BG, KEY_RGB[i], 0.85f), VG_CENTER);
    }
    if (page >= PG_SONG) {   /* the tab carries its sub-page */
        unsigned k = page_key(page);
        vg_round(4 + (int)k * 59, 5, 56, 24, 5, KEY_RGB[k]);
        vg_text(PAGE_NAME[page], 4 + (int)k * 59 + 28, 9, VG_BOLD, DARK, VG_CENTER);
    }
    char b[16];
    bpm_text(b, stat.bpm10 ? stat.bpm10 : st.cur.bpm10);
    vg_text(b, 436, 9, VG_MED, stat.host ? GREEN : FG, VG_RIGHT);
    if (stat.playing) vg_tri(442, 10, 454, 17, 442, 24, GREEN); else vg_rect(443, 11, 11, 11, MUTED);
    if (rec) vg_circle(466, 17, 6, stat.playing ? RED : vg_mix(BG, RED, 0.5f));
    else {
        float lv[2] = { stat.peak_l, stat.peak_r };
        for (int c = 0; c < 2; ++c) {
            float v = lv[c] > 0.00001f ? 1.0f + vd_log2(lv[c]) / 8.0f : 0.0f;
            int m = (int)(v * 24.0f); m = clampi(m, 0, 24);
            vg_rect(462 + c * 7, 5, 5, 24, CELL);
            vg_rect(462 + c * 7, 29 - m, 5, m, lv[c] > 0.89f ? RED : lv[c] > 0.5f ? 0xFFC828 : GREEN);
        }
    }
    vg_rect(4, 34, 472, 1, SEP);
}
static void draw_chips(void)
{
    for (unsigned i = 0; i < PR_RINGS; ++i) {
        float x = 226.0f + (float)i * 25.0f, y = 52.0f;
        if (i < st.cur.rings) {
            int heard = ring_heard(&st.cur, i);
            if (i == sel) vg_circle(x, y, 12.5f, 0xFFFFFF);
            vg_circle(x, y, 10.5f, heard ? RING_RGB[i] : vg_mix(BG, RING_RGB[i], 0.35f));
            char t[2] = {(char)('1' + i), 0};
            vg_text(t, (int)x, (int)y - 8, VG_BOLD, DARK, VG_CENTER);
            if (st.cur.ring[i].flags & PR_MUTE) vg_line(x - 8, y + 8, x + 8, y - 8, 2.0f, RED);
            if (st.cur.ring[i].flags & PR_SOLO) vg_circle(x + 8, y - 8, 3.5f, 0xFFE14D);
        } else vg_circle(x, y, 4.0f, CELL2);
    }
    char t[24] = "";
    uint32_t c = DIM;
    if (stat.playing && stat.song && stat.scene < PR_SCENES) {
        strcpy(t, "SONG "); char s[2] = {(char)('A' + stat.scene), 0}; cat(t, s); c = SCENE_RGB[stat.scene];
        cat(t, " "); catn(t, stat.block + 1); cat(t, "/"); catn(t, st.song.length);
    } else if (song_on) { strcpy(t, "SONG"); c = KEY_RGB[5]; }
    else strcpy(t, "CIRCLES");
    vg_text(t, 474, 44, VG_SMALL, c, VG_RIGHT);
}
/* the selected circle's steps in rows of 16: the pads show the framed row */
static void draw_strip(int y, int cell_h)
{
    const struct pr_ring *r = ring_sel();
    uint32_t col = RING_RGB[sel];
    unsigned playing_step = 255;
    if (stat.playing && r->n) playing_step = (unsigned)(((uint64_t)phase_now() * r->n) >> 32);
    for (unsigned k = 0; k < r->n; ++k) {
        int x = 216 + (int)(k % 16) * 16, yy = y + (int)(k / 16) * (cell_h + 3);
        uint8_t v = r->steps[k];
        uint32_t c = v == 2 ? vg_mix(col, 0xFFFFFF, 0.3f) : v ? vg_mix(BG, col, 0.8f) : CELL2;
        vg_round(x, yy, 14, cell_h, 3, c);
        int pitch = pr_note_pitch(r->note[k]);
        if (v && pitch) vg_rect(x + 3, yy + (pitch > 0 ? 2 : cell_h - 4), 8, 2, DARK);
        if (v && pr_note_len(r->note[k])) vg_rect(x + 2, yy + cell_h / 2 - 1, 10 * (int)pr_note_len(r->note[k]) / 5, 2, DARK);
        if (k == playing_step) vg_frame(x - 1, yy - 1, 16, cell_h + 2, 2, 0xFFFFFF);
        if (page == PG_NOTES && k == cursor_step) vg_rect(x, yy + cell_h + 1, 14, 2, 0xFFFFFF);
    }
    unsigned rows = (r->n + 15u) / 16u;
    if (rows > 1 && page != PG_NOTES) {
        int yy = y + (int)pad_page * (cell_h + 3);
        vg_rect(212, yy, 2, cell_h, FG);
    }
}
static void draw_info(void)
{
    const struct pr_ring *r = ring_sel();
    uint32_t col = RING_RGB[sel];
    char t[48];
    draw_chips();
    switch (page) {
    case PG_SOUND: case PG_ENV:
        vg_text(pr_sound_name(r->bank, r->sound), 216, 72, VG_BIG, col, VG_LEFT);
        strcpy(t, pr_bank_names[r->bank % PR_BANKS]); cat(t, " · SOUND "); catn(t, r->sound + 1);
        if (r->midi[0]) { char m[40]; midi_text(m, r, 0); cat(t, " · MIDI "); cat(t, m); }
        vg_text(t, 216, 100, VG_SMALL, DIM, VG_LEFT);
        draw_strip(124, 16);
        break;
    case PG_RHYTHM:
        if (lib_name[sel]) strcpy(t, lib_name[sel]);
        else if (pr_is_euclid(r)) { strcpy(t, "EUCLID "); catn(t, (int)pr_onsets(r)); cat(t, "/"); catn(t, r->n); }
        else { catn(strcpy(t, ""), (int)pr_onsets(r)); cat(t, " IN "); catn(t, r->n); }
        vg_text(t, 216, 72, VG_BIG, col, VG_LEFT);
        strcpy(t, pr_sound_name(r->bank, r->sound)); cat(t, " · "); catn(t, r->n); cat(t, r->n == 1 ? " STEP" : " STEPS");
        vg_text(t, 216, 100, VG_SMALL, DIM, VG_LEFT);
        draw_strip(124, 16);
        break;
    case PG_NOTES: {
        unsigned k = cursor_step < r->n ? cursor_step : 0;
        strcpy(t, "STEP "); catn(t, (int)k + 1);
        vg_text(t, 216, 72, VG_BIG, col, VG_LEFT);
        int pt = pr_note_pitch(r->note[k]);
        char n[12]; if (ps_melodic(r->bank)) pr_tune_text(n, r, pt); else signed_text(n, pt);
        strcpy(t, n); cat(t, " · "); cat(t, pr_length_names[pr_note_len(r->note[k]) % PR_LENGTHS]);
        cat(t, " · KEYS: NOTE INTO THE STEP");
        vg_text(t, 216, 100, VG_SMALL, DIM, VG_LEFT);
        draw_strip(124, 16);
        break; }
    case PG_MIX:
        for (unsigned i = 0; i < st.cur.rings; ++i) {
            const struct pr_ring *q = &st.cur.ring[i];
            int x = 220 + (int)i * 32, h = 96 * q->level / 127;
            int heard = ring_heard(&st.cur, i);
            vg_rect(x, 84, 22, 96, CELL);
            vg_rect(x, 180 - h, 22, h, heard ? RING_RGB[i] : vg_mix(BG, RING_RGB[i], 0.3f));
            if (i == sel) vg_frame(x - 3, 81, 28, 102, 2, 0xFFFFFF);
            if (q->flags & PR_SOLO) vg_circle((float)x + 11, 76, 3.5f, 0xFFE14D);
            int px = x + 11 + ((int)q->pan - 64) * 11 / 64;
            vg_rect(px - 1, 185, 3, 7, FG);
            char c[2] = {(char)('1' + i), 0};
            vg_text(c, x + 11, 194, VG_BOLD, i == sel ? FG : heard ? DIM : MUTED, VG_CENTER);
        }
        strcpy(t, pr_sound_name(r->bank, r->sound));
        vg_text(t, 474, 72, VG_SMALL, col, VG_RIGHT);
        break;
    case PG_SEQ: {
        bpm_text(t, stat.host ? stat.bpm10 : st.cur.bpm10);
        int w = vg_text(t, 216, 70, VG_HUGE, stat.host ? GREEN : FG, VG_LEFT);
        vg_text("BPM", 222 + w, 108, VG_MED, DIM, VG_LEFT);
        strcpy(t, ""); catn(t, st.cur.beats); cat(t, st.cur.beats == 1 ? " BEAT" : " BEATS"); cat(t, " A TURN · SWING "); catn(t, st.cur.swing); cat(t, "%");
        vg_text(t, 216, 140, VG_SMALL, DIM, VG_LEFT);
        vg_text(stat.host ? "THE HOST LEADS: ITS TEMPO AND SONG POSITION" : "OWN TEMPO · MIDI CLOCK OUT", 216, 160, VG_SMALL, stat.host ? GREEN : MUTED, VG_LEFT);
        strcpy(t, "SLOT "); catn(t, (int)cur_slot + 1); cat(t, unsaved ? " · NOT SAVED" : " · SAVED");
        vg_text(t, 216, 180, VG_SMALL, unsaved ? 0xFFC828 : MUTED, VG_LEFT);
        break; }
    case PG_SONG: {
        const struct pr_song *s = &st.song;
        for (unsigned i = 0; i < PR_SCENES; ++i) {
            int x = 216 + (int)i * 32, used = (s->used >> i) & 1u;
            vg_round(x, 72, 28, 26, 4, used ? SCENE_RGB[i] : CELL2);
            char c[2] = {(char)('A' + i), 0};
            vg_text(c, x + 14, 76, VG_BOLD, used ? DARK : MUTED, VG_CENTER);
            if (i == sel_scene) vg_frame(x - 2, 70, 32, 30, 2, 0xFFFFFF);
        }
        for (unsigned b = 0; b < PR_CHAIN; ++b) {
            int x = 216 + (int)(b % 16) * 16, y = 112 + (int)(b / 16) * 34;
            if (b >= s->length) { vg_rect(x + 5, y + 12, 4, 4, CELL2); continue; }
            unsigned sc = s->scene[b] % PR_SCENES;
            vg_round(x, y, 14, 28, 3, SCENE_RGB[sc]);
            char c[2] = {(char)('A' + sc), 0};
            vg_text(c, x + 7, y + 2, VG_SMALL, DARK, VG_CENTER);
            char n[6]; vg_itoa(s->reps[b], n);
            vg_text(n, x + 7, y + 14, VG_SMALL, DARK, VG_CENTER);
            if (b == cursor_block) vg_rect(x, y + 30, 14, 2, 0xFFFFFF);
            if (stat.playing && stat.song && b == stat.block) vg_frame(x - 1, y - 1, 16, 30, 2, 0xFFFFFF);
        }
        strcpy(t, s->loop ? "LOOPS · " : "ONCE · "); catn(t, (int)pr_song_cycles(s)); cat(t, " TURNS");
        vg_text(t, 216, 186, VG_SMALL, DIM, VG_LEFT);
        break; }
    case PG_MIDI:
        for (unsigned i = 0; i < st.cur.rings; ++i) {
            const struct pr_ring *q = &st.cur.ring[i];
            int y = 70 + (int)i * 17;
            char m[48]; midi_text(m, q, 1);
            char c[2] = {(char)('1' + i), 0};
            vg_text(c, 218, y, VG_SMALL, i == sel ? 0xFFFFFF : RING_RGB[i], VG_LEFT);
            vg_text(pr_sound_short(q->bank, q->sound), 230, y, VG_SMALL, i == sel ? RING_RGB[i] : DIM, VG_LEFT);
            vg_text(m, 300, y, VG_SMALL, q->midi[0] ? FG : MUTED, VG_LEFT);
            if (q->midi[0] && (q->midi[3] & PR_MIDI_ONLY)) vg_text("ONLY", 474, y, VG_SMALL, 0xFFC828, VG_RIGHT);
            if (i == sel) vg_rect(216, y + 15, 258, 1, RING_RGB[i]);
        }
        break;
    case PG_SLOTS:
        for (unsigned i = 0; i < SLOTS; ++i) {
            int x = 216 + (int)(i % 4) * 64, y = 76 + (int)(i / 4) * 56;
            vg_round(x, y, 58, 48, 6, slot_used[i] ? CELL2 : CELL);
            char n[4]; vg_itoa((int)i + 1, n);
            vg_text(n, x + 29, y + 6, VG_BIG, slot_used[i] ? FG : MUTED, VG_CENTER);
            if (i == cur_slot) vg_rect(x + 18, y + 38, 22, 3, KEY_RGB[5]);
            if (i == sel_slot) vg_frame(x - 2, y - 2, 62, 52, 2, 0xFFFFFF);
        }
        break;
    }
}
/* SHAPE: the selected circle alone, as a polygon: its steps, beats, the hits joined, the hits facing each
 * other (red), the axis of symmetry (dashes) and the centre of gravity (white) */
static void draw_polygon(float cx, float cy, float R, const struct shape *g)
{
    const struct pr_ring *r = ring_sel();
    unsigned n = r->n ? r->n : 1, beats = st.cur.beats ? st.cur.beats : 4;
    uint32_t col = RING_RGB[sel];
    float px = cx, py = cy - R;
    for (unsigned s = 1; s <= 96; ++s) {
        float t = (float)s / 96.0f, x = cx + vd_sin(t) * R, y = cy - vd_sin(t + 0.25f) * R;
        vg_line(px, py, x, y, 1.0f, CELL2); px = x; py = y;
    }
    for (unsigned k = 0; k < n; ++k) {
        float t = (float)k / (float)n, sx = vd_sin(t), cy_ = vd_sin(t + 0.25f);
        int beat = (k * beats) % n == 0;
        float in = beat ? R - 9.0f : R - 4.0f;
        vg_line(cx + sx * in, cy - cy_ * in, cx + sx * (R + 3.0f), cy - cy_ * (R + 3.0f), beat ? 2.0f : 1.0f, beat ? DIM : MUTED);
    }
    if (g->axis >= 0) {                          /* the axis: t -> axis - t, through axis / 2 */
        float t = (float)g->axis * 0.5f / (float)n, sx = vd_sin(t), sy = vd_sin(t + 0.25f);
        for (int d = -10; d < 10; d += 2) {
            float a = (float)d / 10.0f * (R + 6.0f), b = (float)(d + 1) / 10.0f * (R + 6.0f);
            vg_line(cx + sx * a, cy - sy * a, cx + sx * b, cy - sy * b, 1.5f, 0xFFE14D);
        }
    }
    unsigned first = n, prev = n;
    for (unsigned k = 0; k <= n; ++k) {
        unsigned q = k % n;
        if (k < n && !r->steps[q]) continue;
        if (k == n && first == n) break;
        if (k == n) q = first;
        if (prev < n) {
            float a = (float)prev / (float)n, b = (float)q / (float)n;
            vg_line(cx + vd_sin(a) * R, cy - vd_sin(a + 0.25f) * R, cx + vd_sin(b) * R, cy - vd_sin(b + 0.25f) * R, 2.5f, col);
        }
        if (k == n) break;
        if (first == n) first = q;
        prev = q;
    }
    if (!(n & 1u)) for (unsigned k = 0; k < n / 2; ++k) if (r->steps[k] && r->steps[k + n / 2]) {
        float a = (float)k / (float)n;
        vg_line(cx + vd_sin(a) * R, cy - vd_sin(a + 0.25f) * R, cx - vd_sin(a) * R, cy + vd_sin(a + 0.25f) * R, 1.5f, RED);
    }
    for (unsigned k = 0; k < n; ++k) if (r->steps[k]) {
        float t = (float)k / (float)n;
        vg_circle(cx + vd_sin(t) * R, cy - vd_sin(t + 0.25f) * R, r->steps[k] == 2 ? 6.0f : 4.5f, r->steps[k] == 2 ? vg_mix(col, 0xFFFFFF, 0.35f) : col);
    }
    if (g->k >= 2) {
        vg_circle(cx, cy, 2.0f, MUTED);
        vg_line(cx, cy, cx + g->cx * R, cy - g->cy * R, 1.0f, FG);
        vg_circle(cx + g->cx * R, cy - g->cy * R, 3.5f, 0xFFFFFF);
    }
    if (stat.playing) {
        float hand = (float)phase_now() * (1.0f / 4294967296.0f);
        vg_line(cx, cy, cx + vd_sin(hand) * (R + 6.0f), cy - vd_sin(hand + 0.25f) * (R + 6.0f), 1.5f, 0xF2F2F2);
    }
}
static void draw_shape_info(const struct shape *g)
{
    char t[40];
    const struct pr_ring *r = ring_sel();
    if (g->k < 2) { vg_text("TWO HITS OR MORE", 216, 80, VG_MED, DIM, VG_LEFT); return; }
    struct row { const char *name; int ok; };
    const struct row rows[7] = {
        {"EUCLIDEAN", g->euclid}, {"BALANCED", g->balanced}, {"LARGE AREA", g->large}, {"ODD", g->odd},
        {"OPENING", g->opening}, {"CLOSURE", g->closure}, {"SYMMETRY", g->symmetric}};
    for (unsigned i = 0; i < 7; ++i) {
        int y = 68 + (int)i * 17;
        vg_circle(222, (float)y + 7.0f, 5.0f, rows[i].ok ? GREEN : CELL2);
        vg_text(rows[i].name, 234, y, VG_SMALL, rows[i].ok ? FG : DIM, VG_LEFT);
        t[0] = 0;
        if (i == 0) { catn(t, (int)(g->even * 1000.0f + .5f) / 10); cat(t, "% EVEN"); }
        if (i == 1) { catn(t, (int)(g->balance * 100.0f + .5f)); cat(t, "%"); }
        if (i == 2) { catn(t, (int)(g->area * 100.0f + .5f)); cat(t, "% OF THE MOST"); }
        if (i == 3) { if (g->n & 1u) strcpy(t, "ODD STEPS"); else { catn(t, (int)g->opposite); cat(t, g->opposite == 1 ? " FACING PAIR" : " FACING PAIRS"); } }
        if (i == 4) strcpy(t, g->opening ? "2ND HIT OFF THE BEAT" : "2ND HIT ON A BEAT");
        if (i == 5) strcpy(t, g->closure ? "LAST HIT ON A BEAT" : "LAST HIT OFF THE BEAT");
        if (i == 6) strcpy(t, g->symmetric ? "AN AXIS" : "NO AXIS");
        vg_text(t, 474, y, VG_SMALL, rows[i].ok ? DIM : MUTED, VG_RIGHT);
    }
    strcpy(t, ""); catn(t, (int)g->score); cat(t, " / 7 · "); catn(t, (int)g->k); cat(t, " IN "); catn(t, r->n);
    vg_text(t, 216, 190, VG_BOLD, RING_RGB[sel], VG_LEFT);
}
static void draw_values(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        struct value v; value_of(i, &v);
        int x = 4 + (int)i * 118;
        vg_round(x, 220, 114, 58, 6, CELL);
        if (!v.label) continue;
        vg_text(v.label, x + 57, 224, VG_SMALL, DIM, VG_CENTER);
        int font = vg_text_width(v.text, VG_BIG) > 106 ? VG_MED : VG_BIG;
        vg_text(v.text, x + 57, font == VG_BIG ? 240 : 245, font, FG, VG_CENTER);
        if (v.frac >= 0.0f) {
            float f = vd_clamp(v.frac, 0.0f, 1.0f);
            vg_rect(x + 10, 269, 94, 3, CELL2);
            vg_rect(x + 10, 269, (int)(94.0f * f), 3, page_rgb(page));
        }
    }
}
static void draw_softkeys(void)
{
    vg_rect(0, 282, VIBE_SCREEN_W, 38, 0x0E1220);
    for (unsigned i = 0; i < 6; ++i) {
        int x = 40 + (int)i * 80, on = softkey_on(i);
        const char *s = softkey(i);
        vg_round(x - 37, 286, 74, 30, 5, on ? page_rgb(page) : s[0] ? CELL2 : CELL);
        vg_text(s, x, 293, VG_MED, on ? DARK : FG, VG_CENTER);
        vibe_label(VB_F1 + (int)i, s);
        vibe_led_button(VB_F1 + (int)i, page_rgb(page), on ? 1.0f : 0.0f);
    }
    for (unsigned i = 0; i < 6; ++i) {
        vibe_label(VB_K1 + (int)i, PAGE_NAME[i]);
        vibe_led_button(VB_K1 + (int)i, KEY_RGB[i], page_key(page) == i ? 1.0f : 0.2f);
    }
}
static void draw_toast(void)
{
    if (!toast_a || now_ms() - toast_ms > 1600u) return;
    vg_round(212, 186, 264, 30, 6, page_rgb(page));
    char t[64]; strcpy(t, toast_a); if (toast_b[0]) { cat(t, " · "); cat(t, toast_b); }
    int font = vg_text_width(t, VG_MED) > 252 ? VG_SMALL : VG_MED;
    vg_text(t, 344, 192, font, DARK, VG_CENTER);
}
static void leds(void)
{
    const struct pr_ring *r = ring_sel();
    unsigned playing_step = 255;
    if (stat.playing && r->n) playing_step = (unsigned)(((uint64_t)phase_now() * r->n) >> 32);
    for (unsigned i = 0; i < 16; ++i) {
        if (page == PG_SONG) {
            unsigned used = i < PR_SCENES && ((st.song.used >> i) & 1u);
            vibe_led_pad((int)i, i < PR_SCENES ? SCENE_RGB[i] : 0, i == sel_scene ? 1.0f : used ? 0.35f : 0.0f);
            continue;
        }
        if (page == PG_SLOTS) {
            vibe_led_pad((int)i, KEY_RGB[5], i < SLOTS ? (i == sel_slot ? 1.0f : slot_used[i] ? 0.3f : 0.0f) : 0.0f);
            continue;
        }
        if (page == PG_MIX) {
            unsigned q = i % 8u;
            if (q >= st.cur.rings) { vibe_led_pad((int)i, 0, 0.0f); continue; }
            if (i < 8) vibe_led_pad((int)i, RING_RGB[q], (st.cur.ring[q].flags & PR_MUTE) ? 0.06f : 0.8f);
            else vibe_led_pad((int)i, 0xFFE14D, (st.cur.ring[q].flags & PR_SOLO) ? 1.0f : 0.05f);
            continue;
        }
        unsigned k = pad_page * 16u + i;
        if (k >= r->n) { vibe_led_pad((int)i, 0, 0.0f); continue; }
        uint8_t v = r->steps[k];
        if (k == playing_step) vibe_led_pad((int)i, 0xFFFFFF, 0.9f);
        else vibe_led_pad((int)i, RING_RGB[sel], v == 2 ? 1.0f : v ? 0.4f : 0.04f);
    }
    vibe_led_button(VB_PLAY, GREEN, stat.playing ? 1.0f : 0.0f);
    vibe_led_button(VB_REC, RED, rec ? 1.0f : 0.0f);
    vibe_label(VB_OCT_DOWN, "COPY");
    vibe_label(VB_OCT_UP, "PASTE");
    vibe_led_button(VB_OCT_UP, RING_RGB[sel], has_clip ? 0.6f : 0.0f);
    vibe_label(VB_LEFT, page == PG_NOTES ? "STEP-" : "PAGE-");
    vibe_label(VB_RIGHT, page == PG_NOTES ? "STEP+" : "PAGE+");
}
static const char *const HELP_COMMON[] = {
    "K1-K6", "the pages", "ENCODER A", "the circle", "ENCODER B", "its sound",
    "PADS", "steps · hard: accent", "- / +", "step page",
    "KEYS", "play · REC: record", "PLAY / STOP", "circles or song",
    "OCT - / +", "copy / paste circle",
};
static const char *const HELP_KEYS[PG_COUNT][6] = {
    {"previous circle", "next circle", "hear it", "mute", "solo", "sound as designed"},
    {"previous circle", "next circle", "hear it", "mute", "solo", "sound as designed"},
    {"previous circle", "next circle", "mute", "solo", "all heard", "hear it"},
    {"clear the circle", "invert", "reverse", "shuffle", "copy", "paste"},
    {"hear the step", "reset its note", "reset all notes", "previous circle", "next circle", "its length on all"},
    {"tap tempo", "song page", "MIDI page", "slots page", "undo", "save (slot)"},
    {"store scene", "recall scene", "insert block", "delete block", "song loops", "PLAY: song / circles"},
    {"MIDI only", "MIDI clock out", "all notes off", "hear it", "GM drum / root note", "back to SEQ"},
    {"load the slot", "save into it", "the demo groove", "", "", "back to SEQ"},
    {"want: even", "want: balanced", "want: large area", "want: odd", "want: opening", "want: closure"},
};
static void draw_help(void)
{
    static const char *rows[64];
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof HELP_COMMON / sizeof HELP_COMMON[0]; ++i) rows[n++] = HELP_COMMON[i];
    static char names[4][32], what[4][32];
    for (unsigned i = 0; i < 4; ++i) {
        struct value v; value_of(i, &v);
        strcpy(names[i], "ENCODER "); catn(names[i], (int)i + 1);
        strcpy(what[i], v.label ? v.label : "-");
        for (char *c = what[i] + 1; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        rows[n++] = names[i]; rows[n++] = what[i];
    }
    static const char *const FK[6] = {"F1", "F2", "F3", "F4", "F5", "F6"};
    for (unsigned i = 0; i < 6; ++i) { if (!HELP_KEYS[page][i][0]) continue; rows[n++] = FK[i]; rows[n++] = HELP_KEYS[page][i]; }
    if (page == PG_MIX) { rows[n++] = "PADS 1-8 / 9-16"; rows[n++] = "mute / solo 1-8"; }
    rows[n++] = "SHIFT"; rows[n++] = "fine steps";
    char title[24] = "POLYCERCLE · "; cat(title, PAGE_NAME[page]);
    vg_controls(title, rows, (int)(n / 2));
}
static void focus(void)
{
    vf_begin();
    for (unsigned i = 0; i < 6; ++i) vf_key(VB_K1 + (int)i, 4 + (int)i * 59, 5, 56, 24);
    for (unsigned i = 0; i < st.cur.rings; ++i) vf_button(100 + (int)i, 226 + (int)i * 25 - 12, 39, 25, 26);
    for (unsigned i = 0; i < 4; ++i) vf_encoder(VE_1 + (int)i, 4 + (int)i * 118, 220, 114, 58);
    for (unsigned i = 0; i < 6; ++i) if (softkey(i)[0]) vf_key(VB_F1 + (int)i, 3 + (int)i * 80, 286, 74, 30);
    vf_end(page_rgb(page));
}
void vibe_on_draw(uint16_t *fb)
{
    (void)fb;
    poll();
    if (help) { draw_help(); return; }
    vg_clear(BG);
    draw_header();
    if (page == PG_SHAPE) {
        struct shape g; const struct pr_ring *r = ring_sel();
        shape_analyse(r->steps, r->n, st.cur.beats, &g);
        draw_polygon(108.0f, 128.0f, 84.0f, &g);
        draw_chips();
        draw_shape_info(&g);
    } else {
        draw_circles(108.0f, 128.0f, 86.0f, 1.0f);
        draw_info();
    }
    draw_values();
    draw_softkeys();
    draw_toast();
    focus();
    leds();
    {   /* Link (an FM-1 as controller): the page and the four values */
        vibe_link_page(PAGE_NAME[page]);
        for (unsigned i = 0; i < 4; ++i) { struct value v; value_of(i, &v); vibe_link_encoder(VE_1 + (int)i, v.label ? v.label : "", v.text); }
    }
}

/* ---------- the stage (SDK 23): the circles, big, in the plugin's and the browser's window ---------- */
static uint16_t stage_fb[960 * 540];
void vibe_on_stage(uint16_t *fb, int w, int h)
{
    (void)fb;
    vg_clear(BG);
    float cy = (float)h * 0.5f + 8.0f, outer = (float)h * 0.5f - 44.0f;
    draw_circles((float)w * 0.5f, cy, outer, 2.2f);
    vg_text("POLYCERCLE", 24, 18, VG_BOLD | VG_X2, KEY_RGB[3], VG_LEFT);
    char t[32];
    bpm_text(t, stat.bpm10 ? stat.bpm10 : st.cur.bpm10); cat(t, " BPM");
    vg_text(t, w - 24, 18, VG_BOLD | VG_X2, stat.host ? GREEN : FG, VG_RIGHT);
    for (unsigned i = 0; i < st.cur.rings; ++i) {
        const struct pr_ring *q = &st.cur.ring[i];
        int y = 84 + (int)i * 52;
        strcpy(t, ""); catn(t, (int)i + 1); cat(t, "  "); cat(t, pr_sound_short(q->bank, q->sound));
        vg_text(t, 24, y, VG_MED | VG_X2, ring_heard(&st.cur, i) ? RING_RGB[i] : MUTED, VG_LEFT);
        strcpy(t, ""); catn(t, (int)pr_onsets(q)); cat(t, " / "); catn(t, q->n);
        vg_text(t, w - 24, y, VG_MED | VG_X2, ring_heard(&st.cur, i) ? RING_RGB[i] : MUTED, VG_RIGHT);
    }
}

/* ---------- start and sound ---------- */
void vibe_on_init(float sample_rate)
{
    SR = sample_rate > 1.0f ? sample_rate : 44100.0f;
    pr_rate(SR);
    pr_engine_init();
    vibe_notes(VIBE_NOTES | VIBE_NOTES_OWN_OCTAVE);   /* OCT - / + copy and paste a circle */
    vibe_params(PARAMS, PA_COUNT);
    vibe_param_groups(GROUPS);
    vibe_editor_cards(CARDS, (int)(sizeof CARDS / sizeof CARDS[0]));
    vibe_editor_accent(KEY_RGB[3]);
    for (unsigned r = 0; r < PR_RINGS; ++r) {
        char tab[4] = {'C', (char)('1' + r), 0};
        (void)tab;
        vibe_editor_control(PA_RING0 + (int)r * PA_PER_RING + RP_PAN, VIBE_CTL_BIPOLAR);
        vibe_editor_control(PA_RING0 + (int)r * PA_PER_RING + RP_MUTE, VIBE_CTL_TOGGLE);
        vibe_editor_control(PA_RING0 + (int)r * PA_PER_RING + RP_SOLO, VIBE_CTL_TOGGLE);
        vibe_editor_control(PA_RING0 + (int)r * PA_PER_RING + RP_TUNE, VIBE_CTL_BIPOLAR);
        vibe_editor_control(PA_RING0 + (int)r * PA_PER_RING + RP_DECAY, VIBE_CTL_BIPOLAR);
        vibe_editor_control(PA_RING0 + (int)r * PA_PER_RING + RP_TONE, VIBE_CTL_BIPOLAR);
    }
    static const char *const tabs[PR_RINGS] = {"C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8"};
    for (unsigned r = 0; r < PR_RINGS; ++r) vibe_editor_tab_color(tabs[r], RING_RGB[r]);
    vibe_editor_control(PA_SELECT, VIBE_CTL_MENU);
    vibe_param_help(PA_BEATS, "Quarter notes in one turn of the hand: every circle spreads its steps over this");
    vibe_param_help(PA_SWING, "Delays every second step of circles with an even number of steps");
    vf_clicks(1);
    vibe_stage(stage_fb, 960, 540, BG, 0);

    settings_read();
    if (state_read("state.bin", &st)) pr_state_demo(&st);
    sanitize();
    slots_scan();
    pr_volume = volume;
    pr_clock((unsigned)clock_out);
    pr_song_mode((unsigned)song_on);
    publish_pending = 1; publish();
    for (unsigned i = 0; i < PA_COUNT; ++i) pval[i] = -1000.0f;
    params_sync();
}
void vibe_on_audio(const float *in, float *out_l, float *out_r, int frames)
{
    (void)in;
    pr_render(out_l, out_r, frames);
}
