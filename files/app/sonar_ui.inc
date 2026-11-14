/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * ec-1 Sonar: the screen and the panel. Included by screen.c so it shares
 * the band painter and key bookkeeping.
 *
 * The main screen is the FM-1 way of seeing a sequence: one lane per ring
 * (haze, glint, drone, pulse), sixteen step cells, each ring's playhead
 * moving at its own length and speed, and a fifth lane for KEYS: a little
 * keyboard lighting the keys you hold.
 *
 *   all 27 keys  play KEYS, chromatic from F3 like the stock FM-1, snapped to
 *                the mood's scale while SCALE LOCK is on. With REC on and a
 *                ring selected they play that ring and write into it
 *   SELECT       the ring (or KEYS) the knobs and pages work on
 *   PRESETS      that ring's sound        ALGORITHM  the keys' pad
 *   K1-K4        the page's knobs, named on the screen
 *   PLAY play/stop  REC write  SAVE save  OCT-/+ keys octave
 *   SCL   a new world (shuffle)    ARP   drift on/off
 *   HOME  WORLD: songs, undo, wipe, mood   SEQ   STEPS: the ring's 16 steps
 *   EDIT  sound    ENV  shape    LFO  tape    FX  space    GLO  mix
 * On WORLD and STEPS the keys become that page's buttons, listed on screen. */
static const uint16_t sn_col[SN_LANES] = {0xb47f /* haze */, 0x7eff /* glint */, 0xfd4a /* drone */, 0x7ff7 /* pulse */, 0xfef2 /* keys */};
static unsigned sn_sel = SN_GLINT;
static uint16_t dim565(uint16_t c) { return (uint16_t)((c >> 1) & 0x7bef); }

enum { SN_MAIN, SN_SEQ, SN_SPACE, SN_SOUND, SN_SHAPE, SN_TAPE, SN_MIX, SN_SONG };
static unsigned sn_page = SN_MAIN, sn_cursor, sn_browse;
static uint32_t sn_keys_down, sn_keys_latched;   /* by key, 0..26 */
static uint32_t sn_notice_at, sn_notice_rev;
static const char *sn_notice_seen;

/* ------------------------------------------------------------ the lanes */
enum { SN_LANE_Y = 34, SN_LANE_H = 31, SN_CELL_X = 62, SN_CELL_W = 11 };
static void sn_lane_label(unsigned l, unsigned y)
{
    const struct sn_ring *R = &sn_song.r[l];
    uint32_t row = 1U | ((uint32_t)(l == sn_sel) << 1) | ((uint32_t)R->mute << 2) | ((uint32_t)R->ch << 3) | ((uint32_t)R->len << 8) |
                   ((uint32_t)R->p.spd << 13) | ((uint32_t)(l == SN_KEYS ? (uint32_t)(R->p.oct + 2) : 0) << 16) | ((uint32_t)sn_catching << 19);
    if (cache->row_cache[l] == row) return;
    cache->row_cache[l] = row;
    paint_fill(0, y, SN_CELL_X - 1, SN_LANE_H - 2, BACK);
    uint16_t c = R->mute && l < SN_RINGS ? DIM : sn_col[l];
    if (l == sn_sel) paint_fill(0, y + 1, 3, SN_LANE_H - 4, sn_catching && l < SN_RINGS ? 0xf800 : TEXT);
    text(6, y + 3, sonar_ring_name(l), 1, c, BACK);
    char sub[10]; memset(sub, 0, sizeof sub); strncpy(sub, sonar_sound_name(l), 9);
    text(6, y + 15, l < SN_RINGS && R->mute ? "MUTE" : sub, 1, l < SN_RINGS && R->mute ? AMBER : DIM, BACK);
}
static void sn_draw_ring_lane(unsigned l)
{
    const struct sn_ring *R = &sn_song.r[l];
    unsigned y = SN_LANE_Y + l * SN_LANE_H, len = R->len, md = sonar_max_degree(l);
    int pos = sn_rt.playing ? sn_rt.pos[l] : -1;
    for (unsigned s = 0; s < SN_STEPS; ++s) {
        unsigned on = s < len && R->st[s].on, d = on ? R->st[s].d : 0, wild = on && R->st[s].y > 153, here = (int)s == pos;
        uint32_t st = 1U | (on << 1) | (d << 2) | (wild << 7) | (here << 8) | ((uint32_t)(s < len) << 9) | ((uint32_t)(l == sn_sel) << 10) |
                      ((uint32_t)R->mute << 11);
        if (cache->cell_cache[l][s] == st) continue;
        cache->cell_cache[l][s] = st;
        unsigned x = SN_CELL_X + s * SN_CELL_W, top = y + 2, h = SN_LANE_H - 6;
        paint_fill(x, top, SN_CELL_W - 1, h, s >= len ? BACK : here ? 0x31a6 : (s & 3U) == 0 ? 0x18c3 : 0x1082);
        if (s >= len) { paint_fill(x + 4, top + h / 2, 2, 2, 0x2104); continue; }
        if (!on) continue;
        unsigned hh = 4U + d * (h - 6U) / (md ? md : 1U);
        uint16_t c = R->mute ? 0x4208 : l == sn_sel ? sn_col[l] : dim565(sn_col[l]);
        if (here) c = TEXT;
        paint_fill(x, top + h - hh, SN_CELL_W - 1, hh, c);
        if (wild) paint_fill(x + 3, top + h - hh - 3, SN_CELL_W - 7, 2, c);     /* a wild note wears a cap */
    }
}
/* the KEYS lane: the FM-1's 27 keys, lit while held (or latched) */
static const uint8_t sn_black_keys[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
static void sn_draw_keys_lane(void)
{
    unsigned top = SN_LANE_Y + SN_KEYS * SN_LANE_H + 2, h = SN_LANE_H - 6;
    uint32_t lit = sn_keys_down | sn_keys_latched;
    for (unsigned w = 0; w < 16; ++w) {
        unsigned on = (lit >> white_keys[w]) & 1U;
        uint32_t st = 1U | (on << 1);
        if (cache->cell_cache[SN_KEYS][w] == st) continue;
        cache->cell_cache[SN_KEYS][w] = st;
        paint_fill(SN_CELL_X + w * SN_CELL_W, top, SN_CELL_W - 1, h, on ? sn_col[SN_KEYS] : 0x39c7);
    }
    for (unsigned i = 0; i < 11; ++i) {      /* each black key sits on the line after the white just below it */
        unsigned b = sn_black_keys[i], on = (lit >> b) & 1U, wi = 0;
        for (unsigned w = 0; w < 16; ++w) if (white_keys[w] == b - 1) wi = w;
        uint32_t st = 1U | (on << 1);
        if (cache->cell_cache[SN_KEYS + 1][i] == st) continue;
        cache->cell_cache[SN_KEYS + 1][i] = st;
        paint_fill(SN_CELL_X + (wi + 1) * SN_CELL_W - 4, top, 7, h * 3 / 5, on ? 0xfd4a : 0x0841);
    }
}
static void sn_draw_tracks(void)
{
    for (unsigned l = 0; l < SN_LANES; ++l) sn_lane_label(l, SN_LANE_Y + l * SN_LANE_H);
    for (unsigned l = 0; l < SN_RINGS; ++l) sn_draw_ring_lane(l);
    sn_draw_keys_lane();
}

/* ------------------------------------------------------------ text helpers */
static void sn_num(char out[8], int v)
{
    char t[8]; int n = 0, neg = v < 0; if (neg) v = -v;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 6);
    int k = 0; if (neg) out[k++] = '-';
    while (n) out[k++] = t[--n];
    out[k] = 0;
}
static void sn_knob_num(unsigned col, const char *label, int v, uint16_t c) { char b[8]; sn_num(b, v); knob(col, label, b, c); }
static void sn_header(void)
{
    uint32_t sig = signature(sn_revision * 7U + sn_song.bpm * 3U + sn_song.mood + ((uint32_t)sn_song.r[SN_KEYS].ch << 20) + (sn_drifting << 9) + (sn_catching << 10) +
                             ((uint32_t)sn_rt.playing << 11) + ((uint32_t)sn_song.fx_mode << 16), sn_song.name, (unsigned)strlen(sn_song.name));
    if (cache->header_cache == sig) return;
    cache->header_cache = sig;
    paint_fill(0, 0, 240, 30, BACK);
    text(6, 5, sn_song.name, 1, 0xb47f, BACK);
    char line[40]; memset(line, 0, sizeof line);
    strcpy(line, sonar_mood_name()); strcat(line, "  ");
    char b[8]; sn_num(b, sn_song.bpm); strcat(line, b); strcat(line, " BPM  SPACE "); strcat(line, sonar_fx_name());
    text(6, 18, line, 1, TEXT, BACK);
    const char *k = sonar_sound_name(SN_KEYS); unsigned n = (unsigned)strlen(k);
    text(234 - 6 * n, 5, k, 1, sn_col[SN_KEYS], BACK);
    if (sn_catching) text(216, 18, "REC", 1, 0xf800, BACK);
    else if (sn_drifting) text(204, 18, "DRIFT", 1, AMBER, BACK);
    paint_fill(0, 30, 240, 1, PANEL);
}
static void sn_page_header(const char *title, const char *sub, uint16_t color)
{
    uint32_t sig = signature(signature(color, title, (unsigned)strlen(title)), sub, (unsigned)strlen(sub)) ^ ((uint32_t)sn_rt.playing << 3);
    if (cache->header_cache == sig) return;
    cache->header_cache = sig;
    paint_fill(0, 0, 240, 30, BACK);
    text(6, 2, title, 2, color, BACK);
    text(6, 19, sub, 1, TEXT, BACK);
    if (sn_rt.playing) text(210, 19, "PLAY", 1, GREEN, BACK);
    paint_fill(0, 30, 240, 1, PANEL);
}
static void sn_body_clear(uint32_t sig)
{
    if (cache->middle_cache == sig) return;
    cache->middle_cache = sig;
    paint_fill(0, 31, 240, 163, BACK);
    memset(cache->cell_cache, 0xff, sizeof cache->cell_cache);
    memset(cache->row_cache, 0xff, sizeof cache->row_cache);
    memset(cache->tile_cache, 0xff, sizeof cache->tile_cache);
    cache->title_cache = ~0U;
}
static void sn_lines(uint32_t sig, const char *const *lines, unsigned n)
{
    if (cache->title_cache == sig) return;
    cache->title_cache = sig;
    paint_fill(0, 34, 240, 158, BACK);
    for (unsigned i = 0; i < n; ++i) text(8, 40 + i * 14, lines[i], 1, i == 0 ? TEXT : DIM, BACK);
}
/* a page's key menu: what each key does on this page, two columns */
static void sn_key_menu(unsigned y, const char *const *items, unsigned n, uint16_t c)
{
    for (unsigned i = 0; i < n; ++i) text(8 + (i & 1U) * 118, y + (i / 2) * 11, items[i], 1, c, BACK);
}
static void sn_hint(void)
{
    static char line[44];
    const char *hint = sn_notice;
    if (!hint) {
        memset(line, 0, sizeof line);
        strcpy(line, sonar_ring_name(sn_sel)); strcat(line, ": "); strcat(line, sonar_sound_name(sn_sel));
        if (sn_sel < SN_RINGS && sn_song.r[sn_sel].mute) strcat(line, "  MUTED");
        hint = line;
    }
    uint32_t h = signature((uint32_t)(uintptr_t)hint ^ (sn_sel << 24) ^ ((uint32_t)sn_song.r[sn_sel].ch << 26), hint, (unsigned)strlen(hint));
    if (cache->hint_cache != h) {
        cache->hint_cache = h;
        paint_fill(0, 228, 240, 12, BACK);
        text(6, 231, hint, 1, sn_notice ? AMBER : DIM, BACK);
    }
}

/* ------------------------------------------------------------ pages */
static const char *const sn_step_keys[8] = {"C# MUTE", "D# NEW LOOP", "F# CLEAR", "G# REMIX", "A# UNDO", "C# SHIFT <", "D# SHIFT >", ""};
static void sn_draw_seq(void)
{
    unsigned r = sn_sel;
    const struct sn_ring *R = &sn_song.r[r];
    char sub[32]; memset(sub, 0, sizeof sub);
    strcpy(sub, sonar_ring_name(r)); strcat(sub, "  "); strcat(sub, sonar_sound_name(r));
    sn_page_header("STEPS", sub, sn_col[r]);
    sn_body_clear(0x5e000000U | r);
    if (r >= SN_RINGS) {
        static const char *const l[3] = {"THE KEYS PLAY LIVE, WITH NO STEPS.", "TURN SELECT TO A RING TO EDIT", "ITS SIXTEEN STEPS HERE."};
        sn_lines(0x5e0000ffU, l, 3);
        for (unsigned k = 0; k < 4; ++k) knob(k, "-", "", DIM);
        return;
    }
    if (cache->title_cache != 0x5e00aa00U) {
        cache->title_cache = 0x5e00aa00U;
        text(8, 146, "WHITE KEYS: STEPS 1-16   BLACK:", 1, DIM, BACK);
        sn_key_menu(158, sn_step_keys, 7, sn_col[r]);
    }
    unsigned top = 38, h = 98, step = sn_rt.playing ? (unsigned)(sn_rt.pos[r] < 0 ? 99 : sn_rt.pos[r]) : 99U, md = sonar_max_degree(r);
    for (unsigned s = 0; s < SN_STEPS; ++s) {
        const struct sn_dot *d = &R->st[s];
        uint32_t st = 1U | ((uint32_t)d->on << 1) | ((uint32_t)(s == step) << 2) | ((uint32_t)(s == sn_cursor) << 3) | ((uint32_t)d->d << 4) |
                      ((uint32_t)(d->y >> 4) << 9) | ((uint32_t)(s < R->len) << 13);
        if (cache->cell_cache[0][s] == st) continue;
        cache->cell_cache[0][s] = st;
        unsigned x = 6 + s * 14;
        paint_fill(x, top, 12, h + 4, BACK);
        paint_fill(x, top, 12, h, s >= R->len ? 0x0841 : s == step ? 0x31a6 : (s & 3U) == 0 ? 0x18c3 : 0x1082);
        if (s == sn_cursor) paint_fill(x, top + h + 2, 12, 2, TEXT);
        if (d->on && s < R->len) {
            unsigned yy = top + h - 10 - (unsigned)d->d * (h - 14) / (md ? md : 1);
            paint_fill(x, yy, 12, 8, R->mute ? DIM : sn_col[r]);
            unsigned wb = (unsigned)d->y * 10U / 255U;          /* wildness: a bar under the note */
            paint_fill(x + 1, yy + 9 < top + h ? yy + 9 : top + h - 1, wb ? wb : 1, 1, AMBER);
        }
    }
    const struct sn_dot *c = &R->st[sn_cursor];
    sn_knob_num(0, "K1 STEP", (int)sn_cursor + 1, TEXT);
    if (c->on) knob(1, "K2 NOTE", sonar_note_name(r, c->d), sn_col[r]); else knob(1, "K2 NOTE", "--", DIM);
    if (c->on) sn_knob_num(2, "K3 WILD", c->y * 100 / 255, sn_col[r]); else knob(2, "K3 WILD", "--", DIM);
    sn_knob_num(3, "K4 STEPS", R->len, sn_col[r]);
}
/* seconds with one decimal, "1.4S" */
static void sn_secs(char out[8], float s)
{
    int t = (int)(s * 10.f + .5f); if (t > 999) t = 999;
    char b[8]; sn_num(b, t / 10); strcpy(out, b); unsigned n = (unsigned)strlen(out);
    out[n] = '.'; out[n + 1] = (char)('0' + t % 10); out[n + 2] = 'S'; out[n + 3] = 0;
}
/* KEYS: the wave you hear, two cycles of it drawn across the screen */
static void sn_draw_keys_wave(uint32_t sig)
{
    static int8_t shape[220]; static uint32_t shape_sig = ~0U;
    uint32_t s2 = sig ^ 0x77000000U;
    if (shape_sig != s2) { shape_sig = s2; sonar_dsp_keys_shape(shape, 220); }
    if (cache->title_cache == s2) return;
    cache->title_cache = s2;
    unsigned top = 40, h = 104, mid = top + h / 2;
    paint_fill(0, 34, 240, 160, BACK);
    paint_fill(10, top, 220, h, 0x0841);
    for (unsigned x = 10; x < 230; x += 55) paint_fill(x, top, 1, h, 0x18c3);         /* half-cycle marks */
    paint_fill(10, mid, 220, 1, 0x2945);
    int py = (int)mid - shape[0] * (int)(h / 2 - 4) / 120;
    for (unsigned i = 1; i < 220; ++i) {
        int y = (int)mid - shape[i] * (int)(h / 2 - 4) / 120;
        line(9 + (int)i, py, 10 + (int)i, y, sn_col[SN_KEYS]);
        line(9 + (int)i, py + 1, 10 + (int)i, y + 1, sn_col[SN_KEYS]);
        py = y;
    }
    text(10, 152, "WAVE", 1, DIM, BACK); text(40, 152, sonar_keys_wave_name(), 1, sn_col[SN_KEYS], BACK);
    text(130, 152, "DETUNE", 1, DIM, BACK); { char b[8]; sn_num(b, sn_song.keys_detune); text(172, 152, b, 1, TEXT, BACK); }
    text(10, 166, "PAD = THE SOUND'S OWN WAVES", 1, DIM, BACK);
    text(10, 178, "OCT-/+ MOVE THE KEYS", 1, DIM, BACK);
}
/* KEYS: the envelope, drawn: swell in, sustain while held, fade after release */
static void sn_draw_keys_env(uint32_t sig)
{
    if (cache->title_cache == (sig ^ 0x55000000U)) return;
    cache->title_cache = sig ^ 0x55000000U;
    float a, s, r; sonar_dsp_keys_env(&a, &s, &r);
    float hold = 1.5f, total = a + hold + r, px = 210.f / total;
    unsigned top = 44, bot = 140;
    paint_fill(0, 34, 240, 160, BACK);
    paint_fill(10, top - 4, 220, bot - top + 8, 0x0841);
    int x0 = 15, xa = x0 + (int)(a * px), xh = xa + (int)(hold * px), xr = xh + (int)(r * px);
    int ys = (int)bot - (int)((float)(bot - top) * s);
    uint16_t c = sn_col[SN_KEYS];
    for (int t = 0; t < 2; ++t) {
        line(x0, (int)bot + t, xa, (int)top + t, c);
        line(xa, (int)top + t, xa + 6, ys + t, c);
        line(xa + 6, ys + t, xh, ys + t, c);
        line(xh, ys + t, xr, (int)bot + t, c);
    }
    paint_fill((unsigned)xh, top - 4, 1, bot - top + 8, 0x4208);                      /* the key lets go here */
    char b[8];
    text(10, 152, "ATTACK", 1, DIM, BACK); sn_secs(b, a); text(52, 152, b, 1, TEXT, BACK);
    text(124, 152, "RELEASE", 1, DIM, BACK); sn_secs(b, r); text(172, 152, b, 1, TEXT, BACK);
    text(10, 166, "SWELL IN, HOLD, THEN FADE", 1, DIM, BACK);
    text(10, 178, sn_song.keys_latch ? "HOLD ON: TAP A KEY ON, TAP IT OFF" : "HOLD OFF: NOTES END WHEN YOU LET GO", 1, sn_song.keys_latch ? AMBER : DIM, BACK);
}
static const char *const sn_world_keys[6] = {"C# SHUFFLE", "D# UNDO", "F# WIPE ALL", "G# NEXT MOOD", "A# SPACE MODE", "C# DRIFT"};
static void sn_draw_knob_page(void)
{
    unsigned r = sn_sel;
    const struct sn_ring *R = &sn_song.r[r];
    char sub[32]; memset(sub, 0, sizeof sub);
    strcpy(sub, sonar_ring_name(r)); strcat(sub, "  "); strcat(sub, sonar_sound_name(r));
    uint32_t sig = sn_revision * 13U + sn_page * 7U + r;
    static const char *const speeds[4] = {"1/4X", "1/2X", "1X", "2X"};
    switch (sn_page) {
    case SN_SPACE: {
        sn_page_header("SPACE", "TAPE ECHO + 11 SECOND HALL", 0x7eff);
        sn_body_clear(0xec000000U);
        static const char *const l[5] = {"EVERY RING FEEDS THE SPACE:", "A THREE-HEAD TAPE ECHO WITH", "WOW AND FLUTTER, AND A LONG", "DARK HALL. ECHO/VERB/DRY FOR", "OUTBOARD GEAR."};
        sn_lines(sig, l, 5);
        knob(0, "K1 MODE", sonar_fx_name(), 0x7eff);
        sn_knob_num(1, "K2 AMOUNT", sn_song.fx_amt, 0x7eff);
        sn_knob_num(2, "K3 VOLUME", sn_song.vol, TEXT);
        sn_knob_num(3, "K4 BPM", sn_song.bpm, TEXT);
        break; }
    case SN_SOUND: {
        if (r == SN_KEYS) {
            sn_page_header("WAVE", sub, sn_col[r]);
            sn_body_clear(0x50000000U | r);
            sn_draw_keys_wave(((uint32_t)R->ch << 8) | ((uint32_t)sn_song.keys_wave << 4) | ((uint32_t)sn_song.keys_detune << 12));
            knob(0, "K1 PAD", sonar_sound_name(r), sn_col[r]);
            knob(1, "K2 WAVE", sonar_keys_wave_name(), sn_col[r]);
            sn_knob_num(2, "K3 DETUNE", sn_song.keys_detune, sn_col[r]);
            knob(3, "K4 CHORD", sonar_keys_chord_name(), sn_col[r]);
            break;
        }
        sn_page_header("SOUND", sub, sn_col[r]);
        sn_body_clear(0x50000000U | r);
        static const char *const l[4] = {"K1 OR PRESETS (TRACK SOUND): ITS SOUND", "K2 STEPS IN THE LOOP", "K3 SPEED AGAINST THE CLOCK", "K4 OCTAVE"};
        static const char *const lk[5] = {"THE KEYS: PADS OVER THE LOOPS", "K1 OR ALGORITHM (KEYS SOUND): THE PAD", "K2 CHORD: SINGLE OPEN TRIAD WIDE", "K3 HOLD: KEYS STAY ON TILL PRESSED", "K4 SCALE: SNAP KEYS TO THE MOOD"};
        if (r == SN_KEYS) sn_lines(sig, lk, 5); else sn_lines(sig, l, 4);
        knob(0, "K1 SOUND", sonar_sound_name(r), sn_col[r]);
        if (r < SN_RINGS) {
            sn_knob_num(1, "K2 STEPS", R->len, sn_col[r]); knob(2, "K3 SPEED", speeds[R->p.spd & 3], sn_col[r]);
            sn_knob_num(3, "K4 OCT", R->p.oct, sn_col[r]);
        } else {
            knob(1, "K2 CHORD", sonar_keys_chord_name(), sn_col[r]);
            knob(2, "K3 HOLD", sn_song.keys_latch ? "ON" : "OFF", sn_song.keys_latch ? AMBER : DIM);
            knob(3, "K4 SCALE", sn_song.keys_lock ? "LOCK" : "FREE", sn_col[r]);
        }
        break; }
    case SN_SHAPE: {
        if (r == SN_KEYS) {
            sn_page_header("ENVELOPE", sub, sn_col[r]);
            sn_body_clear(0x5a000000U | r);
            sn_draw_keys_env(((uint32_t)R->ch << 8) | ((uint32_t)sn_song.keys_atk << 12) | ((uint32_t)sn_song.keys_rel << 20) | ((uint32_t)sn_song.keys_latch << 28));
            float a, s, rl; char b1[8], b2[8]; sonar_dsp_keys_env(&a, &s, &rl); sn_secs(b1, a); sn_secs(b2, rl); (void)s;
            knob(0, "K1 ATTACK", b1, sn_col[r]);
            knob(1, "RELEASE", b2, sn_col[r]);
            knob(2, "K3 HOLD", sn_song.keys_latch ? "ON" : "OFF", sn_song.keys_latch ? AMBER : DIM);
            sn_knob_num(3, "K4 VOLUME", R->p.lvl, sn_col[r]);
            break;
        }
        sn_page_header("SHAPE", sub, sn_col[r]);
        sn_body_clear(0x5a000000U | r);
        static const char *const l[4] = {"K1 DECAY: HOW LONG NOTES RING", "K2 WILD: CALM TO WILD TIMBRE", "K3 MOD: VIBRATO", "K4 LEVEL"};
        sn_lines(sig, l, 4);
        sn_knob_num(0, "K1 DECAY", R->p.dec, sn_col[r]);
        sn_knob_num(1, "K2 WILD", R->p.wild, sn_col[r]);
        sn_knob_num(2, "K3 MOD", R->p.mod, sn_col[r]);
        sn_knob_num(3, "K4 LEVEL", R->p.lvl, sn_col[r]);
        break; }
    case SN_TAPE: {
        if (r == SN_KEYS) {
            sn_page_header("TONE", sub, sn_col[r]);
            sn_body_clear(0x7a000000U | r);
            if (cache->title_cache != sig) {
                cache->title_cache = sig;
                paint_fill(0, 34, 240, 160, BACK);
                static const char *const l[4] = {"K1 TONE: DARK TO BRIGHT", "K2 VIBRATO", "K3 SPACE: ECHO AND HALL", "K4 SCALE: SNAP KEYS TO THE MOOD"};
                for (unsigned i = 0; i < 4; ++i) text(8, 44 + i * 14, l[i], 1, i ? DIM : TEXT, BACK);
                /* the tone as a filter curve: flat, then rolling off past the cutoff */
                unsigned cx = 20 + (unsigned)R->p.tone * 190U / 100U;
                paint_fill(10, 110, 220, 70, 0x0841);
                for (int t = 0; t < 2; ++t) { line(14, 125 + t, (int)cx, 125 + t, sn_col[r]); line((int)cx, 125 + t, (int)cx + 30 > 226 ? 226 : (int)cx + 30, 172 + t, sn_col[r]); }
                text(14, 113, "LOW", 1, DIM, BACK); text(200, 113, "HIGH", 1, DIM, BACK);
            }
            sn_knob_num(0, "K1 TONE", R->p.tone, sn_col[r]);
            sn_knob_num(1, "VIBRATO", R->p.mod, sn_col[r]);
            if (R->p.sp_on) sn_knob_num(2, "K3 SPACE", R->p.sp, sn_col[r]); else knob(2, "K3 SPACE", "AUTO", sn_col[r]);
            knob(3, "K4 SCALE", sn_song.keys_lock ? "LOCK" : "FREE", sn_col[r]);
            break;
        }
        sn_page_header("TAPE", sub, sn_col[r]);
        sn_body_clear(0x7a000000U | r);
        static const char *const l[5] = {"EACH RING HAS ITS OWN TAPE.", "K1 TONE: DARK TO BRIGHT", "K2 ERA: WARM TAPE ... 8-BIT", "K3 TAPE: WOW, FLUTTER, HISS", "K4 SPACE: HOW MUCH ECHO"};
        sn_lines(sig, l, 5);
        sn_knob_num(0, "K1 TONE", R->p.tone, sn_col[r]);
        sn_knob_num(1, "K2 ERA", R->p.era, sn_col[r]);
        sn_knob_num(2, "K3 TAPE", R->p.tape, sn_col[r]);
        if (R->p.sp_on) sn_knob_num(3, "K4 SPACE", R->p.sp, sn_col[r]); else knob(3, "K4 SPACE", "AUTO", sn_col[r]);
        break; }
    case SN_MIX: {
        sn_page_header("MIX", "WHITE KEYS 1-4 MUTE A TRACK", TEXT);
        sn_body_clear(0x3a000000U);
        if (cache->title_cache != sig) {
            cache->title_cache = sig;
            paint_fill(0, 34, 240, 158, BACK);
            for (unsigned l = 0; l < SN_LANES; ++l) {
                unsigned x = 10 + l * 46, hh = (unsigned)sn_song.r[l].p.lvl * 110U / 100U;
                paint_fill(x, 40, 34, 120, 0x1082);
                paint_fill(x, 40 + 120 - hh, 34, hh, sn_song.r[l].mute ? DIM : sn_col[l]);
                text(x, 166, sonar_ring_name(l), 1, sn_song.r[l].mute ? DIM : sn_col[l], BACK);
                text(x, 178, sn_song.r[l].mute ? "MUTE" : "", 1, AMBER, BACK);
            }
        }
        for (unsigned k = 0; k < 4; ++k) {
            static const char *const lb[4] = {"K1 HAZE", "K2 GLINT", "K3 DRONE", "K4 PULSE"};
            sn_knob_num(k, lb[k], sn_song.r[k].p.lvl, sn_col[k]);
        }
        break; }
    default: {   /* WORLD: songs and the whole-song moves */
        sn_page_header("WORLD", sn_song.name, 0xb47f);
        sn_body_clear(0x50a00000U);
        if (cache->title_cache != sig) {
            cache->title_cache = sig;
            paint_fill(0, 34, 240, 158, BACK);
            text(6, 38, "WHITE 1-8: LOAD   SAVE+WHITE: STORE", 1, DIM, BACK);
            for (unsigned k = 0; k < SN_SLOTS; ++k) {
                char slot[30]; memset(slot, 0, sizeof slot); slot[0] = (char)('1' + k); slot[1] = ' ';
                strncat(slot, sonar_store_name(k), 24);
                text(6 + (k / 4) * 118, 52 + (k % 4) * 11, slot, 1, k == sn_slot ? GREEN : k == sn_browse ? TEXT : DIM, BACK);
            }
            text(6, 106, "BLACK KEYS:", 1, DIM, BACK);
            sn_key_menu(118, sn_world_keys, 6, 0x7eff);
            text(6, 156, "SCL SHUFFLES  ARP DRIFTS", 1, DIM, BACK);
            text(6, 168, "HOLD SAVE: +SCL UNDO +PLAY RESTART", 1, DIM, BACK);
            text(6, 180, "+REC CLEAR TRACK  +K1-K4 LEVELS", 1, DIM, BACK);
        }
        {   /* audio health, for tracking down glitches: late blocks since start, the voice budget */
            uint32_t sig = (sn_diag_late << 8) ^ sn_diag_budget;
            if (cache->tile_cache[2] != sig) {
                cache->tile_cache[2] = sig;
                char line[40], b[8]; memset(line, 0, sizeof line);
                strcpy(line, "AUDIO LATE "); sn_num(b, (int)sn_diag_late); strcat(line, b);
                strcat(line, "  LOAD "); sn_num(b, (int)sn_diag_budget); strcat(line, b);
                paint_fill(130, 106, 110, 9, BACK);
                text(130, 106, line + 6, 1, sn_diag_late ? AMBER : GREEN, BACK);
            }
        }
        knob(0, "K1 DRIFT", sn_drifting ? "ON" : "OFF", sn_drifting ? AMBER : DIM);
        knob(1, "K2 MOOD", sonar_mood_name(), TEXT);
        sn_knob_num(2, "K3 MUTATE", sn_song.mut, TEXT);
        sn_knob_num(3, "K4 BPM", sn_song.bpm, TEXT);
        break; }
    }
}
static void sonar_draw(void)
{
    uint32_t now = fm1_millis();
    if (sn_notice != sn_notice_seen || (sn_notice && sn_revision != sn_notice_rev && sn_notice_seen == sn_notice && now - sn_notice_at > 600U)) {
        if (sn_notice != sn_notice_seen) sn_notice_at = now;
        sn_notice_seen = sn_notice; sn_notice_rev = sn_revision;
    }
    if (sn_notice && now - sn_notice_at > 5000U) { sn_notice = 0; sn_notice_seen = 0; }
    if (sn_first_boot) {           /* the first start: the essentials on one page */
        sn_page_header("EC-1 SONAR", "FOUR LOOPS AND A KEYBOARD OF PADS", 0xb47f);
        sn_body_clear(0x0e1c0001U);
        if (cache->title_cache != 0x0e1c0001U) {
            cache->title_cache = 0x0e1c0001U;
            static const char *const k[8] = {"PLAY", "KEYS", "ALGORITHM", "SCL", "SEQ", "HOME", "K1  K2", "HOLD SAVE"};
            static const char *const v[8] = {"START, STOP (HOLD: FADE)", "PLAY PADS OVER THE LOOPS", "CHOOSE THE PAD", "A NEW WORLD", "EDIT A TRACK'S STEPS", "SONGS, UNDO, MOOD", "KEYS AND LOOPS VOLUME", "SHORTCUTS: SCL UNDO"};
            for (unsigned i = 0; i < 8; ++i) { text(8, 40 + i * 16, k[i], 1, sn_col[i & 3], BACK); text(74, 40 + i * 16, v[i], 1, TEXT, BACK); }
            text(8, 176, "PRESS PLAY TO BEGIN", 1, AMBER, BACK);
        }
        for (unsigned i = 0; i < 4; ++i) knob(i, "", "", BACK);
        sn_hint();
        return;
    }
    if (sn_page == SN_MAIN) {
        sn_header();
        sn_body_clear(0xd15c0001U);
        sn_draw_tracks();
        /* the main knobs: the two volumes, the space, the tempo */
        sn_knob_num(0, "K1 KEYS", sn_song.r[SN_KEYS].p.lvl, sn_col[SN_KEYS]);
        sn_knob_num(1, "K2 LOOPS", sn_song.loops_vol, 0xb47f);
        sn_knob_num(2, "K3 SPACE", sn_song.fx_amt, 0x7eff);
        sn_knob_num(3, "K4 BPM", sn_song.bpm, TEXT);
    } else if (sn_page == SN_SEQ) sn_draw_seq();
    else sn_draw_knob_page();
    sn_hint();
}

/* Panel lights: PLAY, REC and ARP (drift); the keys you hold or have latched; on STEPS, the ring's steps. */
static void sonar_leds(void)
{
    static uint64_t shown = ~0ULL;
    uint64_t bits = 0;
    if (sn_rt.playing) bits |= 1ULL << PLAY;
    if (sn_catching) bits |= 1ULL << REC;
    if (sn_drifting) bits |= 1ULL << ARP;
    if (sn_page == SN_SEQ && sn_sel < SN_RINGS) {
        const struct sn_ring *R = &sn_song.r[sn_sel];
        for (unsigned s = 0; s < SN_STEPS && s < R->len; ++s) if (R->st[s].on) bits |= 1ULL << (14 + white_keys[s]);
        if (sn_rt.playing && sn_rt.pos[sn_sel] >= 0) bits ^= 1ULL << (14 + white_keys[sn_rt.pos[sn_sel] & 15]);
    } else bits |= (uint64_t)(sn_keys_down | sn_keys_latched) << 14;
    uint64_t changed = bits ^ shown;
    for (unsigned led = 0; led <= 40; ++led)
        if (changed & (1ULL << led)) fm1_led_set(led, (int)((bits >> led) & 1U));
    shown = bits;
}

/* ------------------------------------------------------------ the panel */
/* every knob says what it did, in words, on the bottom line */
static char sn_fb[44];
static void sn_feedback_text(const char *who, const char *what, int value, const char *word)
{
    memset(sn_fb, 0, sizeof sn_fb);
    if (who && *who) { strcpy(sn_fb, who); strcat(sn_fb, " "); }
    strcat(sn_fb, what); strcat(sn_fb, " ");
    if (word) strncat(sn_fb, word, 14);
    else { char b[8]; sn_num(b, value); strcat(sn_fb, b); }
    sn_notice = sn_fb; ++sn_revision;
}
static void sn_knob_feedback(unsigned k)
{
    if (!k) return;
    static const char *const speeds[4] = {"1/4X", "1/2X", "1X", "2X"};
    const struct sn_ring *R = &sn_song.r[sn_sel];
    const char *who = sonar_ring_name(sn_sel);
    switch (sn_page) {
    case SN_MAIN:
        if (k == 1) sn_feedback_text("", "KEYS VOLUME", sn_song.r[SN_KEYS].p.lvl, 0);
        else if (k == 2) sn_feedback_text("", "LOOPS VOLUME", sn_song.loops_vol, 0);
        else if (k == 3) sn_feedback_text("", "SPACE AMOUNT", sn_song.fx_amt, 0);
        else sn_feedback_text("", "TEMPO", sn_song.bpm, 0);
        break;
    case SN_SEQ: {
        if (sn_sel >= SN_RINGS) return;
        const struct sn_dot *c = &R->st[sn_cursor];
        char st[12]; memset(st, 0, sizeof st); strcpy(st, "STEP "); char b[4]; sn_num(b, (int)sn_cursor + 1); strcat(st, b);
        if (k == 1) sn_feedback_text(who, st, 0, c->on ? "ON" : "EMPTY");
        else if (k == 2) sn_feedback_text(st, "NOTE", 0, c->on ? sonar_note_name(sn_sel, c->d) : "(EMPTY)");
        else if (k == 3) sn_feedback_text(st, "WILD", c->y * 100 / 255, c->on ? 0 : "(EMPTY)");
        else sn_feedback_text(who, "LOOP STEPS", R->len, 0);
        break; }
    case SN_SPACE:
        if (k == 1) sn_feedback_text("", "SPACE MODE", 0, sonar_fx_name());
        else if (k == 2) sn_feedback_text("", "SPACE AMOUNT", sn_song.fx_amt, 0);
        else if (k == 3) sn_feedback_text("", "OUTPUT VOLUME", sn_song.vol, 0);
        else sn_feedback_text("", "TEMPO", sn_song.bpm, 0);
        break;
    case SN_SOUND:
        if (k == 1) sn_feedback_text(who, "SOUND", 0, sonar_sound_name(sn_sel));
        else if (sn_sel == SN_KEYS) {
            if (k == 2) sn_feedback_text("KEYS", "WAVE", 0, sonar_keys_wave_name());
            else if (k == 3) sn_feedback_text("KEYS", "DETUNE", sn_song.keys_detune, 0);
            else sn_feedback_text("KEYS", "CHORD", 0, sonar_keys_chord_name());
        }
        else if (k == 2) sn_feedback_text(who, "LOOP STEPS", R->len, 0);
        else if (k == 3) sn_feedback_text(who, "SPEED", 0, speeds[R->p.spd & 3]);
        else sn_feedback_text(who, "OCTAVE", R->p.oct, 0);
        break;
    case SN_SHAPE:
        if (sn_sel == SN_KEYS) {
            float a, s, r; char b[8]; sonar_dsp_keys_env(&a, &s, &r);
            if (k == 1) { sn_secs(b, a); sn_feedback_text("KEYS", "ATTACK", 0, b); }
            else if (k == 2) { sn_secs(b, r); sn_feedback_text("KEYS", "RELEASE", 0, b); }
            else if (k == 3) sn_feedback_text("KEYS", "HOLD", 0, sn_song.keys_latch ? "ON: TAP ON, TAP OFF" : "OFF");
            else sn_feedback_text("KEYS", "VOLUME", R->p.lvl, 0);
            (void)s;
            break;
        }
        if (k == 1) sn_feedback_text(who, "DECAY", R->p.dec, 0);
        else if (k == 2) sn_feedback_text(who, "WILD", R->p.wild, 0);
        else if (k == 3) sn_feedback_text(who, "VIBRATO", R->p.mod, 0);
        else sn_feedback_text(who, "VOLUME", R->p.lvl, 0);
        break;
    case SN_TAPE:
        if (sn_sel == SN_KEYS) {
            if (k == 1) sn_feedback_text("KEYS", "TONE", R->p.tone, 0);
            else if (k == 2) sn_feedback_text("KEYS", "VIBRATO", R->p.mod, 0);
            else if (k == 3) sn_feedback_text("KEYS", "SPACE", R->p.sp, 0);
            else sn_feedback_text("KEYS", "SCALE", 0, sn_song.keys_lock ? "LOCKED TO THE MOOD" : "FREE");
            break;
        }
        if (k == 1) sn_feedback_text(who, "TONE", R->p.tone, 0);
        else if (k == 2) sn_feedback_text(who, "ERA", R->p.era, 0);
        else if (k == 3) sn_feedback_text(who, "TAPE", R->p.tape, 0);
        else sn_feedback_text(who, "SPACE SEND", R->p.sp, 0);
        break;
    case SN_MIX:
        sn_feedback_text(sonar_ring_name(k - 1), "LEVEL", sn_song.r[k - 1].p.lvl, 0);
        break;
    default:
        if (k == 1) sn_feedback_text("", "DRIFT", 0, sn_drifting ? "ON" : "OFF");
        else if (k == 2) sn_feedback_text("", "MOOD", 0, sonar_mood_name());
        else if (k == 3) sn_feedback_text("", "MUTATE", sn_song.mut, 0);
        else sn_feedback_text("", "TEMPO", sn_song.bpm, 0);
        break;
    }
}
/* PRESETS only ever changes a sequencer track; the keys' pad is ALGORITHM's */
static void sn_track_sound(int d)
{
    if (sn_sel >= SN_RINGS) { sn_notice = "SELECT A TRACK. KEYS SOUND IS ALGORITHM"; ++sn_revision; return; }
    sonar_sound(sn_sel, d);
    static char tl[32]; memset(tl, 0, sizeof tl); strcpy(tl, sonar_ring_name(sn_sel)); strcat(tl, ": "); strcat(tl, sonar_sound_name(sn_sel));
    sn_notice = tl; ++sn_revision;
}
static void sn_open(unsigned pg)
{
    sn_page = sn_page == pg ? SN_MAIN : pg;
    cache->shown_page = ~0U;
    if (sn_page == SN_SONG) sn_browse = sn_slot;
}
static void sn_release_latched(void)
{
    for (unsigned k = 0; k < 27; ++k) if (sn_keys_latched & (1U << k)) sonar_dsp_live(SN_KEYS, k, sn_brush_y, k, 0);
    sn_keys_latched = 0;
}
static void sn_play_key(unsigned k, unsigned down)
{
    static char line[32];
    uint32_t bit = 1U << k;
    if (sn_song.keys_latch) {                      /* HOLD: a press starts the note, the next press ends it */
        if (!down) { sn_keys_down &= ~bit; return; }
        sn_keys_down |= bit;
        if (sn_keys_latched & bit) { sn_keys_latched &= ~bit; sonar_dsp_live(SN_KEYS, k, sn_brush_y, k, 0); ++sn_revision; return; }
        sn_keys_latched |= bit;
    } else if (down) sn_keys_down |= bit; else { sn_keys_down &= ~bit; sonar_dsp_live(SN_KEYS, k, sn_brush_y, k, 0); ++sn_revision; return; }
    sonar_dsp_live(SN_KEYS, k, sn_brush_y, k, 1);
    memset(line, 0, sizeof line); strcpy(line, "KEYS "); strcat(line, sonar_midi_name(sonar_keys_note(k)));
    strcat(line, "  "); strcat(line, sonar_sound_name(SN_KEYS));
    sn_notice = line; ++sn_revision;
}
static void sonar_page_input(const struct fm1_controls *controls, uint32_t pressed, uint32_t old_keys, uint32_t now)
{
    const int16_t *e = controls->encoders;
    uint32_t held = controls->buttons;
    static uint32_t prev_held, play_at, save_used;
    uint32_t released = prev_held & ~held;
    prev_held = held;
    unsigned save = (held & BUTTON(SAVE)) != 0;
    /* the first start: a page of the essentials, gone at the first press */
    if (sn_first_boot && (pressed || controls->keys || e[0] || e[1] || e[2] || e[3] || e[4] || e[5] || e[6])) {
        sn_first_boot = 0; cache->shown_page = ~0U;     /* the first press also does its job */
    }
    /* SAVE is the shift: held, it turns SCL, PLAY, REC and the four knobs into shortcuts; tapped alone it saves */
    if (pressed & BUTTON(SAVE)) save_used = 0;
    if (save && (pressed & ~BUTTON(SAVE))) {
        if (pressed & BUTTON(SCL)) { (void)sonar_undo(); }
        if (pressed & BUTTON(PLAY)) { if (sn_rt.playing) { sonar_play(1); sn_notice = "ALL LOOPS BACK TO STEP 1"; ++sn_revision; } else sonar_play(1); }
        if (pressed & BUTTON(REC)) { if (sn_sel < SN_RINGS) sonar_clear_ring(sn_sel); else { sn_notice = "SELECT A TRACK TO CLEAR"; ++sn_revision; } }
        save_used = 1;
        pressed &= ~(BUTTON(SCL) | BUTTON(PLAY) | BUTTON(REC));
    }
    if ((released & BUTTON(SAVE)) && !save_used && sn_page != SN_SONG) { sn_notice = sonar_store_save(sn_slot) ? "SAVE FAILED" : "SONG SAVED"; ++sn_revision; }
    if (pressed & BUTTON(HOME)) sn_open(sn_page == SN_MAIN ? SN_SONG : sn_page);
    if (pressed & BUTTON(SEQ)) sn_open(SN_SEQ);
    if (pressed & BUTTON(FX)) sn_open(SN_SPACE);
    if (pressed & BUTTON(EDIT)) sn_open(SN_SOUND);
    if (pressed & BUTTON(ENV)) sn_open(SN_SHAPE);
    if (pressed & BUTTON(LFO)) sn_open(SN_TAPE);
    if (pressed & BUTTON(GLO)) sn_open(SN_MIX);
    if (pressed & BUTTON(ARP)) sonar_drift_toggle();
    if (pressed & BUTTON(SCL)) sonar_shuffle();
    /* PLAY: a tap starts or stops, a long press (a second) fades the loops in or out */
    if (pressed & BUTTON(PLAY)) play_at = now | 1U;
    if ((released & BUTTON(PLAY)) && play_at) {
        if (now - play_at >= 700U) sonar_fade(!sn_rt.playing);
        else sonar_play(!sn_rt.playing);
        play_at = 0;
    }
    if (pressed & BUTTON(REC)) {
        sn_catching = !sn_catching;
        sn_notice = !sn_catching ? "KEYS PLAY THEIR OWN SOUND" : sn_sel < SN_RINGS ? "REC: KEYS WRITE INTO THE TRACK" : "REC: SELECT A TRACK TO WRITE INTO";
        ++sn_revision;
    }
    /* OCT moves the keys, as on the stock FM-1; a ring's octave is on its EDIT page */
    if (pressed & (BUTTON(OCT_UP) | BUTTON(OCT_DOWN))) {
        sonar_param(SN_KEYS, SP_OCT, (pressed & BUTTON(OCT_UP)) ? 1 : -1);
        static char ol[24]; memset(ol, 0, sizeof ol); strcpy(ol, "KEYS OCTAVE ");
        int o = sn_song.r[SN_KEYS].p.oct; char b[4] = {o < 0 ? '-' : '+', (char)('0' + (o < 0 ? -o : o)), 0, 0}; strcat(ol, b);
        sn_notice = ol; ++sn_revision;
    }
    if (e[0]) { sn_sel = (unsigned)((int)sn_sel + (e[0] > 0 ? 1 : -1) + SN_LANES) % SN_LANES; ++sn_revision; if (sn_page == SN_SEQ) cache->shown_page = ~0U; }
    int d1 = e[2] > 0 ? 1 : e[2] < 0 ? -1 : 0, d2 = e[3] > 0 ? 1 : e[3] < 0 ? -1 : 0, d3 = e[4] > 0 ? 1 : e[4] < 0 ? -1 : 0, d4 = e[5] > 0 ? 1 : e[5] < 0 ? -1 : 0;
    int dp = e[6] > 0 ? 1 : e[6] < 0 ? -1 : 0, da = e[1] > 0 ? 1 : e[1] < 0 ? -1 : 0;
    int f1 = d1 ? fm_encoder_delta(2, e[2]) : 0, f2 = d2 ? fm_encoder_delta(3, e[3]) : 0, f3 = d3 ? fm_encoder_delta(4, e[4]) : 0, f4 = d4 ? fm_encoder_delta(5, e[5]) : 0;
    if (da) {   /* ALGORITHM always picks the keys' pad, whatever page is open */
        sonar_sound(SN_KEYS, da);
        static char kl[32]; memset(kl, 0, sizeof kl); strcpy(kl, "KEYS: "); strcat(kl, sonar_sound_name(SN_KEYS));
        sn_notice = kl; ++sn_revision;
    }
    if (save && sn_page == SN_MAIN && (f1 || f2 || f3 || f4)) {     /* SAVE + K1-K4: each track's level */
        if (f1) sonar_param(0, SP_LVL, f1);
        if (f2) sonar_param(1, SP_LVL, f2);
        if (f3) sonar_param(2, SP_LVL, f3);
        if (f4) sonar_param(3, SP_LVL, f4);
        unsigned t = f1 ? 0U : f2 ? 1U : f3 ? 2U : 3U;
        sn_feedback_text(sonar_ring_name(t), "LEVEL", sn_song.r[t].p.lvl, 0);
        save_used = 1;
        f1 = f2 = f3 = f4 = 0; d1 = d2 = d3 = d4 = 0;
    }
    switch (sn_page) {
    case SN_MAIN:
        if (dp) sn_track_sound(dp);
        if (f1) sonar_param(SN_KEYS, SP_LVL, f1);
        if (f2) sonar_loops_volume(f2);
        if (f3) sonar_fx_amt(f3);
        if (f4) sonar_tempo(f4);
        break;
    case SN_SEQ:
        if (dp) { sn_cursor = (sn_cursor + 16U + (unsigned)dp) & 15U; ++sn_revision; }
        if (d1) { sn_cursor = (sn_cursor + 16U + (unsigned)d1) & 15U; ++sn_revision; }
        if (d2) sonar_step_edit(sn_sel, sn_cursor, d2, 0);
        if (f3) sonar_step_edit(sn_sel, sn_cursor, 0, f3);
        if (d4) sonar_param(sn_sel, SP_LEN, d4);
        break;
    case SN_SPACE:
        if (d1) sonar_fx_mode(d1);
        if (f2) sonar_fx_amt(f2);
        if (f3) sonar_volume(f3);
        if (f4) sonar_tempo(f4);
        break;
    case SN_SOUND:
        if (d1) sonar_sound(sn_sel, d1);
        if (dp) sn_track_sound(dp);
        if (sn_sel == SN_KEYS) {             /* WAVE: pad, wave, detune, chord */
            if (d2) sonar_keys_edit(0, d2);
            if (d3) sonar_keys_edit(1, d3);
            if (d4) sonar_keys_chord(d4);
        } else {
            if (d2) sonar_param(sn_sel, SP_LEN, d2);
            if (d3) sonar_param(sn_sel, SP_SPD, d3);
            if (d4) sonar_param(sn_sel, SP_OCT, d4);
        }
        break;
    case SN_SHAPE:
        if (sn_sel == SN_KEYS) {             /* ENVELOPE: attack, release, hold, volume */
            if (f1) sonar_keys_edit(2, f1);
            if (f2) sonar_keys_edit(3, f2);
            if (d3) { sn_song.keys_latch = d3 > 0; if (!sn_song.keys_latch) sn_release_latched(); ++sn_revision; }
            if (f4) sonar_param(SN_KEYS, SP_LVL, f4);
            break;
        }
        if (f1) sonar_param(sn_sel, SP_DEC, f1);
        if (f2) sonar_param(sn_sel, SP_WILD, f2);
        if (f3) sonar_param(sn_sel, SP_MOD, f3);
        if (f4) sonar_param(sn_sel, SP_LVL, f4);
        if (dp) sn_track_sound(dp);
        break;
    case SN_TAPE:
        if (sn_sel == SN_KEYS) {             /* TONE: tone, vibrato, space, scale */
            if (f1) sonar_param(SN_KEYS, SP_TONE, f1);
            if (f2) sonar_param(SN_KEYS, SP_MOD, f2);
            if (f3) sonar_param(SN_KEYS, SP_SP, f3);
            if (d4) { sn_song.keys_lock = d4 > 0; ++sn_revision; }
            break;
        }
        if (f1) sonar_param(sn_sel, SP_TONE, f1);
        if (f2) sonar_param(sn_sel, SP_ERA, f2);
        if (f3) sonar_param(sn_sel, SP_TAPE, f3);
        if (f4) sonar_param(sn_sel, SP_SP, f4);
        if (dp) sn_track_sound(dp);
        break;
    case SN_MIX:
        if (f1) sonar_param(0, SP_LVL, f1);
        if (f2) sonar_param(1, SP_LVL, f2);
        if (f3) sonar_param(2, SP_LVL, f3);
        if (f4) sonar_param(3, SP_LVL, f4);
        break;
    default:   /* WORLD */
        if (dp) { sn_browse = (sn_browse + SN_SLOTS + (unsigned)dp) % SN_SLOTS; ++sn_revision; }
        if (d1) sonar_drift_toggle();
        if (d2) { sonar_mood(d2); sn_notice = sonar_mood_name(); }
        if (f3) sonar_mutate_amt(f3);
        if (f4) sonar_tempo(f4);
        break;
    }
    sn_knob_feedback(d1 ? 1U : d2 ? 2U : d3 ? 3U : d4 ? 4U : 0U);
    /* the keys */
    for (unsigned k = 0; k < 27; ++k) {
        uint32_t bit = 1U << k;
        unsigned down = (controls->keys & bit) && !(old_keys & bit), up = !(controls->keys & bit) && (old_keys & bit);
        if (!down && !up) continue;
        int white = -1, black = -1;
        for (unsigned i = 0; i < 16; ++i) if (white_keys[i] == k) white = (int)i;
        for (unsigned i = 0; i < 10; ++i) if (edit_keys[i] == k) black = (int)i;
        if (sn_page == SN_SEQ && sn_sel < SN_RINGS) {             /* STEPS: whites are the steps, blacks the ring's moves */
            if (!down) continue;
            if (white >= 0) { sn_cursor = (unsigned)white; sonar_step_toggle(sn_sel, (unsigned)white); continue; }
            switch (black) {
            case 0: sonar_mute(sn_sel); break;
            case 1: sonar_seed_ring(sn_sel); break;
            case 2: sonar_clear_ring(sn_sel); break;
            case 3: sonar_remix(sn_sel); break;
            case 4: (void)sonar_undo(); break;
            case 5: sonar_rotate(sn_sel, -1); break;
            case 6: sonar_rotate(sn_sel, 1); break;
            default: break;
            }
            ++sn_revision;
            continue;
        }
        if (sn_page == SN_SONG) {                                 /* WORLD: whites load songs, blacks the song moves */
            if (!down) continue;
            if (white >= 0 && white < SN_SLOTS) {
                if (save) { sn_notice = sonar_store_save((unsigned)white) ? "SAVE FAILED" : "SONG SAVED IN THAT SLOT"; save_used = 1; }
                else if ((unsigned)white == sn_slot) sn_notice = "THAT SONG IS PLAYING NOW";
                else { (void)sonar_store_save(sn_slot); sn_notice = sonar_store_load((unsigned)white) ? "THAT SLOT IS EMPTY" : "SONG LOADED"; }
                sn_browse = (unsigned)white; ++sn_revision; cache->title_cache = ~0U;
                continue;
            }
            switch (black) {
            case 0: sonar_shuffle(); break;
            case 1: (void)sonar_undo(); break;
            case 2: sonar_wipe(); break;
            case 3: sonar_mood(1); sn_notice = sonar_mood_name(); break;
            case 4: sonar_fx_mode(1); break;
            case 5: sonar_drift_toggle(); break;
            default: break;
            }
            ++sn_revision; cache->title_cache = ~0U;
            continue;
        }
        if (sn_page == SN_MIX && white >= 0 && white < SN_RINGS) { if (down) sonar_mute((unsigned)white); continue; }
        /* everywhere else, every key is a note */
        if (sn_catching && sn_sel < SN_RINGS) { sonar_key(sn_sel, sonar_key_degree(sn_sel, k), k, down); continue; }
        sn_play_key(k, down);
    }
    (void)now;
    blocked_keys |= controls->keys;
}
