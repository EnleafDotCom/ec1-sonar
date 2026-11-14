/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * ec-1 Sonar: the composer and the controls (control task).
 * Worlds, patterns, seeds, drift, mutate, undo and the saved songs follow
 * the ec-1 Sonar Synth web app. */
#include <string.h>
#include <stddef.h>
#include "sonar.h"

struct sn_song sn_song;
struct sn_runtime sn_rt;
const char *sn_notice;
uint32_t sn_revision;
uint32_t sn_diag_late, sn_diag_budget = 80;   /* shown on WORLD: late audio blocks since start, the voice work budget */
uint8_t sn_first_boot, sn_drifting, sn_catching = 0, sn_slot, sn_brush_y = 77;

/* ------------------------------------------------------------ randomness */
static uint32_t rng = 0x2545f491u;
static uint32_t rnd(void) { uint32_t x = rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rng = x ? x : 1u; return rng; }
static int rr(int n) { return n > 0 ? (int)(rnd() % (unsigned)n) : 0; }
static float frand(void) { return (float)(rnd() >> 8) / 16777216.f; }
static float frr(float a, float b) { return a + frand() * (b - a); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
#define PICK(a) ((a)[rr((int)(sizeof(a) / sizeof((a)[0])))])

/* ------------------------------------------------------------ the world */
static const char *const mood_names[SN_MOODS] = {"DAWN", "TIDE", "DUSK", "DEEP", "ZEN", "FLOAT"};
static const uint8_t mood_len[SN_MOODS] = {7, 5, 7, 7, 5, 6};
static const char *const ring_names[SN_LANES] = {"HAZE", "GLINT", "DRONE", "PULSE", "KEYS"};
static const char *const sound_names[SN_SOUNDS] = {"ANALOG", "BELL", "COMPUTER", "LO-RES", "CHOIR", "HARP", "PAD", "COMET",
                                                   "SHIMMER", "THEREMIN", "BRAAM", "SIGNAL", "ANGEL", "HALO"};
static const char *const key_names[SN_KEY_SOUNDS] = {"SERAPH", "HALO PAD", "CHOIR", "CATHEDRAL", "AURORA", "GLASS PAD", "STRINGS", "ETHER"};
static const char *const chord_names[4] = {"SINGLE", "OPEN", "TRIAD", "WIDE"};
static const char *const kit_names[SN_KITS] = {"STATION", "DAMAGED", "GLITCH"};
static const char *const piece_names[SN_PIECES] = {"BOOM", "SONAR", "TELEMETRY", "AIRLOCK", "HULL", "BEAM"};
static const char *const fx_names[4] = {"ON", "ECHO", "VERB", "DRY"};
static const char *const note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const uint8_t scales[SN_MOODS][7] = {{0, 2, 4, 6, 7, 9, 11}, {0, 2, 4, 7, 9}, {0, 2, 3, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 10}, {0, 1, 5, 7, 10}, {0, 2, 4, 6, 8, 10}};
static const uint8_t ring_base[SN_LANES] = {48, 60, 36, 0, 60};

static void say(const char *t) { sn_notice = t; ++sn_revision; }
static char say_buf[48];
static void say2(const char *a, const char *b)
{
    memset(say_buf, 0, sizeof say_buf);
    strncpy(say_buf, a, 40); strncat(say_buf, b, sizeof say_buf - 1 - strlen(say_buf));
    say(say_buf);
}
unsigned sonar_kind(unsigned r) { return r == SN_HAZE ? SN_KIND_CHORD : r == SN_PULSE ? SN_KIND_DRUM : SN_KIND_MEL; }
unsigned sonar_max_degree(unsigned r) { return r == SN_PULSE ? SN_PIECES - 1 : r == SN_HAZE ? 9 : r == SN_KEYS ? 15 : 14; }
const char *sonar_ring_name(unsigned r) { return r < SN_LANES ? ring_names[r] : ""; }
const char *sonar_sound_name(unsigned r)
{
    if (r >= SN_LANES) return "";
    if (r == SN_KEYS) return key_names[sn_song.r[r].ch % SN_KEY_SOUNDS];
    return r == SN_PULSE ? kit_names[sn_song.r[r].ch % SN_KITS] : sound_names[sn_song.r[r].ch % SN_SOUNDS];
}
const char *sonar_mood_name(void) { return mood_names[sn_song.mood % SN_MOODS]; }
const char *sonar_fx_name(void) { return fx_names[sn_song.fx_mode & 3]; }
const char *sonar_note_name(unsigned r, unsigned d)
{
    if (r == SN_PULSE) return piece_names[d % SN_PIECES];
    unsigned L = mood_len[sn_song.mood % SN_MOODS], o = d / L, i = d % L;
    int m = ring_base[r] + 12 * sn_song.r[r].p.oct + 12 * (int)o + scales[sn_song.mood % SN_MOODS][i];
    return note_names[((m % 12) + 12) % 12];
}
static unsigned scale_len(void) { return mood_len[sn_song.mood % SN_MOODS]; }

static void apply_all(void)
{
    sonar_dsp_keys();
    sonar_dsp_tempo();
    for (unsigned r = 0; r < SN_LANES; ++r) sonar_dsp_ring(r);
    sonar_dsp_fx();
}

/* ------------------------------------------------------------ patterns */
static struct sn_dot pat[SN_STEPS];
static unsigned pat_n;
static void put(int i, int d, float y)
{
    if (i < 0 || i >= (int)pat_n) return;
    pat[i].on = 1; pat[i].d = (uint8_t)clampi(d, 0, 30); pat[i].y = (uint8_t)clampi((int)(y * 255.f), 0, 255); pat[i].skip = 0;
}
static void put_r(int i, int d) { put(i, d, frr(.15f, .45f)); }
static int euclid(int k, int n, int rot, int i) { int j = (i + rot) % n; return (j + 1) * k / n > j * k / n; }

static const int8_t *prog_now;
static int at_prog(int i) { return prog_now[(i * 4 / (int)pat_n) % 4]; }

static void compose(unsigned r, const char *p)
{
    unsigned n = sn_song.r[r].len;
    memset(pat, 0, sizeof pat); pat_n = n;
    int L = (int)scale_len();
    if (!strcmp(p, "scan")) {
        put(0, 1, .2f); put((int)n / 2, 1, .35f); { static const int a[3] = {3, 5, 11}; put(PICK(a), 2, frr(.2f, .6f)); }
        if (frand() < .6f) put((int)n - 2, 3, .4f);
    } else if (!strcmp(p, "chatter")) {
        put(0, 0, .2f); int k = rr(2) ? 5 : 4;
        for (int i = 0; i < (int)n; ++i) if (euclid(k, (int)n, 1, i) && !pat[i].on) put(i, 2, frr(.2f, .9f));
        put(rr(2) ? 10 : 6, 4, .4f);
    } else if (!strcmp(p, "hull")) {
        put(0, 0, .3f); put(rr(2) ? 6 : 5, 4, frr(.2f, .5f)); put(rr(2) ? 11 : 10, 4, frr(.4f, .8f)); if (frand() < .7f) put(14, 3, .3f);
    } else if (!strcmp(p, "beam")) {
        put(0, 0, .2f); for (int i = 0; i < (int)n; ++i) if (euclid(3, (int)n, 2, i) && !pat[i].on) put(i, 5, frr(.2f, .9f));
        put(rr(2) ? 13 : 7, 2, .5f);
    } else if (!strcmp(p, "wind")) {
        put(0, 3, .6f); put((int)n / 2, 3, .3f); put(4, 0, .15f); if (frand() < .5f) put(12, 1, .3f);
    } else if (!strcmp(p, "drift")) {
        put(0, 5, .4f); put(5 + rr(3), 3, .3f); put(11, 5, .6f);
    } else if (!strcmp(p, "drone")) {
        put(0, 0, .15f); if (n > 4) put((int)n / 2, 4, .2f);
    } else if (!strcmp(p, "balladbass")) {
        put(0, at_prog(0), .2f); put((int)n / 2, at_prog((int)n / 2), .2f); if (frand() < .5f) put((int)n - 2, at_prog((int)n - 2) + 4, .2f);
    } else if (!strcmp(p, "melody")) {
        int d = 4 + rr(4); int k = (int)((float)n * frr(.35f, .6f) + .5f), rot = rr(2) ? 2 : 0;
        static const int mv[7] = {-2, -1, -1, 0, 1, 1, 2};
        for (int i = 0; i < (int)n; ++i) if (euclid(k, (int)n, rot, i)) { d = clampi(d + PICK(mv), 0, 11); put(i, d, frand() < .15f ? frr(.6f, .85f) : frr(.05f, .4f)); }
    } else if (!strcmp(p, "ostinato")) {
        static const int ms[3] = {4, 6, 8}; int m = PICK(ms); int fig[8];
        int pool[6] = {0, 2, 4, L, L + 2, 1};
        for (int k = 0; k < m; ++k) fig[k] = k == 0 ? 0 : pool[rr(6)];
        for (int i = 0; i < (int)n; ++i) put(i, at_prog(i) + fig[i % m], i % 4 == 0 ? frr(.5f, .65f) : frr(.15f, .35f));
    } else if (!strcmp(p, "hook")) {
        int half = (int)n / 2 < 4 ? 4 : (int)n / 2;
        static const int8_t rhs[5][4] = {{0, 3, 6, 7}, {0, 2, 3, 6}, {0, 3, 4, 7}, {2, 3, 5, 6}, {0, 1, 4, 6}};
        const int8_t *rh = rhs[rr(5)];
        int d = 4 + rr(4), ph[4];
        static const int mv[4] = {-2, -1, 1, 2};
        for (int k = 0; k < 4; ++k) ph[k] = d = clampi(d + PICK(mv), 2, 10);
        for (int k = 0; k < 4; ++k) {
            int p1 = rh[k] * half / 8;
            put_r(p1, ph[k]);
            int last[3] = {L, 4, 0};
            put_r(half + p1, k == 3 ? last[rr(3)] : ph[k]);
        }
    } else if (!strcmp(p, "hymn")) {
        int d = 7 + rr(3);
        int pos[4] = {0, (int)n / 4, (int)n / 2, 3 * (int)n / 4}, np = n >= 8 ? 4 : 2;
        if (np == 2) pos[1] = (int)n / 2;
        static const int mv[6] = {-1, -1, 1, 1, -2, 2};
        for (int k = 0; k < np; ++k) { if (k && frand() < .25f) continue; put(pos[k], d, frr(.1f, .35f)); d = clampi(d + PICK(mv), 4, 12); }
    } else if (!strcmp(p, "arp")) {
        int shapes[4][4] = {{0, 2, 4, L}, {0, 2, 4, 2}, {0, 4, L, 4}, {L, 4, 2, 0}};
        int *sh = shapes[rr(4)];
        for (int i = 0; i < (int)n; ++i) put(i, at_prog(i) + sh[i % 4], frr(.25f, .45f));
    } else if (!strcmp(p, "bells")) {
        int pool[6] = {L, L + 2, L + 4, 4, 6, 2 * L}, c = 4 + rr(3);
        for (int k = 0; k < c; ++k) put(rr((int)n), pool[rr(6)], frr(.15f, .5f));
    } else if (!strcmp(p, "sparse")) {
        int pool[4] = {0, 2, 4, L}, c = 2 + rr(3);
        for (int k = 0; k < c; ++k) put(rr((int)n), pool[rr(4)], frr(.2f, .8f));
    } else if (!strcmp(p, "ballad")) {
        int bp[4]; if (L >= 7) { bp[0] = 0; bp[1] = 4; bp[2] = 5; bp[3] = 3; } else { bp[0] = 0; bp[1] = 3; bp[2] = 4; bp[3] = 2; }
        int m = n < 4 ? (int)n : 4;
        for (int k = 0; k < m; ++k) put(k * (int)n / m, bp[k], frr(.15f, .35f));
    } else {   /* prog: the chords of the progression */
        int hits = n >= 6 ? 2 : 1;
        for (int k = 0; k < hits; ++k) put(k * ((int)n / 2), prog_now[(k * 2) % 4], frr(.15f, .45f));
    }
    for (unsigned i = 0; i < SN_STEPS; ++i) {
        struct sn_dot *d = &pat[i];
        if (d->on) d->d = (uint8_t)clampi(d->d, 0, (int)sonar_max_degree(r));
    }
    memcpy(sn_song.r[r].st, pat, sizeof pat);
}

/* a fresh loop for one ring, in the style the web app seeds a ring with */
static void seed(unsigned r)
{
    struct sn_ring *R = &sn_song.r[r];
    int n = R->len, L = (int)scale_len();
    memset(pat, 0, sizeof pat); pat_n = (unsigned)n;
    if (r == SN_PULSE) {
        put(0, 0, frr(.1f, .3f));
        static const int ks[3] = {2, 3, 5}, rots[3] = {1, 2, 3}, ps[3] = {1, 1, 2};
        int k = PICK(ks), rot = PICK(rots);
        for (int i = 0; i < n; ++i) if (euclid(k, n, rot, i) && !pat[i].on) put(i, PICK(ps), frr(0, .5f));
        static const int extra[4] = {3, 4, 4, 5};
        for (int k2 = 1 + rr(3); k2 > 0; --k2) { int i = rr(n); if (!pat[i].on) put(i, PICK(extra), frr(.2f, .7f)); }
    } else if (r == SN_DRONE) {
        put(0, 0, frr(.1f, .3f));
        static const int c[3] = {3, 4, 2};
        if (n >= 8 && frand() < .7f) put(n / 2, PICK(c), frr(.1f, .3f));
    } else if (r == SN_HAZE) {
        int prog[4]; if (L >= 7) { prog[0] = 0; prog[1] = 5; prog[2] = 3; prog[3] = 4; } else { prog[0] = 0; prog[1] = 3; prog[2] = 1; prog[3] = 2; }
        int hits[3] = {0, n / 2, n - 1}, nh = n >= 6 ? 2 : 1;
        if (frand() < .5f && n > 4) { if (nh == 1) hits[1] = n - 1; ++nh; }
        for (int k = 0; k < nh; ++k) put(hits[k], prog[k % 4], frr(.15f, .5f));
    } else {
        int d = 4 + rr(4), k = (int)((float)n * frr(.2f, .4f) + .5f), rot = rr(2) ? 2 : 0;
        static const int mv[8] = {-2, -1, -1, 0, 1, 1, 2, 3};
        for (int i = 0; i < n; ++i) if (euclid(k, n, rot, i)) { d = clampi(d + PICK(mv), 0, 11); put(i, d, frand() < .2f ? frr(.6f, .9f) : frr(.05f, .45f)); }
    }
    for (unsigned i = 0; i < SN_STEPS; ++i) if (pat[i].on) pat[i].d = (uint8_t)clampi(pat[i].d, 0, (int)sonar_max_degree(r));
    memcpy(R->st, pat, sizeof pat);
}

/* ------------------------------------------------------------ worlds: every shuffle lands somewhere new */
/* amounts are percent; NA (255) keeps the default */
struct world_ring { uint8_t ch, spd, len[3], era0, era1; const char *pat; uint8_t sp, tone, wild; int8_t oct; uint8_t dec, mod, lvl; };
struct world { const char *name; uint8_t keep; uint8_t bpm0, bpm1, moods[2], tape, mut, space0, space1; struct world_ring r[4]; };
/* rings: haze (bloom), glint (spark), drone (bass), pulse (beat). -1 leaves a setting at its default */
#define NA -1.f
#define V(x) ((x) < 0 ? 255 : (uint8_t)((x) * 100.f + .5f))
#define K(h, g, d, p) ((h) | (g) << 1 | (d) << 2 | (p) << 3)
static const struct world worlds[] = {
    {"DEEP SPACE STATION", K(0, 0, 0, 1), 20, 26, {3, 5}, V(0), V(.2f), V(1.1f), V(1.4f), {
        {6, 0, {0}, V(.05f), V(.2f), "ballad", V(1), V(.45f), V(NA), 0, V(1), V(.35f), V(NA)},
        {8, 1, {16, 10, 7}, V(.1f), V(.25f), "bells", V(1), V(NA), V(.45f), 1, V(.8f), V(NA), V(NA)},
        {10, 0, {8}, V(.05f), V(.2f), "drone", V(.8f), V(.35f), V(NA), 0, V(1), V(NA), V(NA)},
        {0, 2, {0}, V(.05f), V(.2f), "scan", V(1), V(.7f), V(NA), 0, V(NA), V(NA), V(NA)}}},
    {"ALIEN SIGNAL", K(0, 1, 0, 0), 24, 30, {5, 4}, V(0), V(.35f), V(.9f), V(1.3f), {
        {9, 0, {0}, V(.05f), V(.2f), "ballad", V(1), V(NA), V(NA), 0, V(1), V(.2f), V(NA)},
        {11, 1, {16, 12, 7}, V(.1f), V(.4f), "sparse", V(1), V(NA), V(.7f), 0, V(.8f), V(NA), V(NA)},
        {11, 0, {8}, V(.1f), V(.3f), "drone", V(.8f), V(.4f), V(.3f), 0, V(1), V(NA), V(NA)},
        {2, 2, {0}, V(.3f), V(.6f), "chatter", V(1), V(NA), V(NA), 0, V(NA), V(NA), V(.7f)}}},
    {"NEON RAIN", K(1, 0, 0, 0), 26, 32, {2, 3}, V(.45f), V(.15f), V(.8f), V(1.1f), {
        {6, 1, {0}, V(.05f), V(.2f), "prog", V(1), V(.55f), V(NA), 0, V(.95f), V(.4f), V(NA)},
        {1, 2, {16}, V(.1f), V(.25f), "hook", V(1), V(NA), V(.35f), 0, V(.8f), V(NA), V(NA)},
        {0, 1, {0}, V(.05f), V(.2f), "balladbass", V(.7f), V(.38f), V(NA), 0, V(.95f), V(NA), V(NA)},
        {1, 2, {0}, V(.05f), V(.2f), "wind", V(.9f), V(.55f), V(NA), 0, V(NA), V(NA), V(NA)}}},
    {"DERELICT SHIP", K(0, 0, 1, 1), 18, 24, {3, 4}, V(.4f), V(.25f), V(1.1f), V(1.5f), {
        {4, 0, {0}, V(.05f), V(.2f), "ballad", V(1), V(.3f), V(NA), 0, V(1), V(.4f), V(NA)},
        {7, 1, {16, 12}, V(.1f), V(.3f), "sparse", V(1), V(NA), V(.4f), 0, V(NA), V(.3f), V(NA)},
        {10, 0, {8}, V(.05f), V(.2f), "drone", V(.85f), V(.3f), V(.4f), 0, V(1), V(NA), V(NA)},
        {1, 2, {0}, V(.1f), V(.3f), "hull", V(1), V(.5f), V(NA), 0, V(NA), V(NA), V(NA)}}},
    {"FIRST CONTACT", K(0, 1, 0, 0), 22, 28, {0, 5}, V(.2f), V(.15f), V(1), V(1.3f), {
        {4, 0, {0}, V(.05f), V(.15f), "ballad", V(1), V(NA), V(NA), 0, V(1), V(.25f), V(NA)},
        {9, 1, {16}, V(.05f), V(.2f), "hook", V(1), V(NA), V(.4f), 0, V(.9f), V(NA), V(NA)},
        {0, 0, {8}, V(.05f), V(.2f), "drone", V(.7f), V(.4f), V(NA), 0, V(1), V(NA), V(NA)},
        {0, 2, {0}, V(.05f), V(.2f), "scan", V(1), V(NA), V(NA), 0, V(NA), V(NA), V(.65f)}}},
    {"CRYOSLEEP", K(1, 0, 0, 0), 18, 22, {0, 1}, V(.3f), V(.1f), V(1.2f), V(1.5f), {
        {6, 0, {0}, V(.05f), V(.2f), "ballad", V(1), V(NA), V(NA), 0, V(1), V(.3f), V(NA)},
        {2, 1, {16}, V(.85f), V(1), "arp", V(1), V(.6f), V(.2f), 0, V(NA), V(NA), V(.6f)},
        {8, 0, {8}, V(.05f), V(.2f), "drone", V(.8f), V(NA), V(NA), 0, V(1), V(NA), V(NA)},
        {0, 2, {0}, V(.05f), V(.2f), "drift", V(1), V(NA), V(NA), 0, V(NA), V(NA), V(.5f)}}},
    {"HYPERDRIVE", K(0, 1, 0, 0), 32, 38, {5, 2}, V(0), V(.2f), V(.6f), V(.9f), {
        {11, 0, {0}, V(.1f), V(.3f), "ballad", V(1), V(NA), V(NA), 0, V(1), V(.2f), V(NA)},
        {8, 2, {16}, V(.15f), V(.35f), "ostinato", V(1), V(NA), V(.4f), 0, V(.45f), V(NA), V(NA)},
        {10, 1, {0}, V(.1f), V(.25f), "drone", V(.7f), V(NA), V(.5f), 0, V(1), V(NA), V(NA)},
        {2, 2, {0}, V(.2f), V(.4f), "beam", V(.9f), V(NA), V(NA), 0, V(NA), V(NA), V(NA)}}},
    {"PLANET SURFACE", K(0, 0, 0, 1), 22, 28, {4, 3}, V(0), V(.2f), V(.9f), V(1.2f), {
        {4, 0, {0}, V(.05f), V(.2f), "ballad", V(1), V(NA), V(NA), 0, V(1), V(.3f), V(NA)},
        {9, 1, {16, 12}, V(.05f), V(.2f), "melody", V(1), V(NA), V(.35f), 0, V(.8f), V(NA), V(NA)},
        {0, 0, {8}, V(.05f), V(.2f), "drone", V(.7f), V(.35f), V(NA), 0, V(1), V(.2f), V(NA)},
        {0, 2, {0}, V(.05f), V(.2f), "wind", V(1), V(.6f), V(NA), 0, V(NA), V(NA), V(NA)}}},
    {"MAINFRAME", K(0, 1, 0, 1), 28, 36, {1, 5}, V(0), V(.25f), V(.7f), V(1), {
        {11, 0, {0}, V(.6f), V(.8f), "ballad", V(1), V(NA), V(NA), 0, V(1), V(NA), V(.5f)},
        {2, 2, {16}, V(.9f), V(1), "arp", V(1), V(.8f), V(.3f), 0, V(NA), V(NA), V(NA)},
        {3, 1, {0}, V(.85f), V(1), "drone", V(.7f), V(NA), V(.1f), 0, V(.9f), V(NA), V(NA)},
        {2, 2, {0}, V(.8f), V(1), "chatter", V(.9f), V(NA), V(NA), 0, V(NA), V(NA), V(NA)}}},
    {"CELESTIAL", K(1, 1, 0, 0), 18, 24, {0, 1}, V(.2f), V(.1f), V(1.2f), V(1.5f), {
        {12, 0, {0}, V(0), V(.15f), "ballad", V(1), V(.6f), V(NA), 0, V(1), V(.2f), V(NA)},
        {12, 1, {16}, V(0), V(.15f), "hymn", V(1), V(NA), V(.3f), 1, V(1), V(NA), V(NA)},
        {13, 0, {8}, V(0), V(.15f), "drone", V(.9f), V(.4f), V(NA), 0, V(1), V(NA), V(NA)},
        {0, 2, {0}, V(0), V(.15f), "drift", V(1), V(NA), V(NA), 0, V(NA), V(NA), V(.35f)}}},
    {"HALO", K(1, 0, 0, 0), 20, 26, {0, 3}, V(0), V(.12f), V(1.1f), V(1.4f), {
        {13, 0, {0}, V(0), V(.15f), "ballad", V(1), V(.55f), V(NA), 0, V(1), V(.15f), V(NA)},
        {8, 1, {16, 10}, V(.05f), V(.2f), "bells", V(1), V(NA), V(.35f), 1, V(.9f), V(NA), V(NA)},
        {0, 0, {8}, V(0), V(.15f), "drone", V(.8f), V(.35f), V(NA), 0, V(1), V(NA), V(NA)},
        {0, 2, {0}, V(0), V(.2f), "scan", V(1), V(NA), V(NA), 0, V(NA), V(NA), V(.4f)}}},
    {"SERAPHIM", K(1, 0, 0, 0), 22, 28, {2, 0}, V(.25f), V(.15f), V(1.1f), V(1.4f), {
        {12, 1, {0}, V(0), V(.15f), "prog", V(1), V(NA), V(NA), 0, V(1), V(.25f), V(NA)},
        {9, 1, {16}, V(0), V(.15f), "hymn", V(1), V(NA), V(.35f), 0, V(1), V(NA), V(NA)},
        {13, 0, {8}, V(0), V(.15f), "drone", V(.85f), V(.4f), V(NA), 0, V(1), V(NA), V(NA)},
        {1, 2, {0}, V(0), V(.15f), "wind", V(1), V(NA), V(NA), 0, V(NA), V(NA), V(.4f)}}},
};
enum { WORLDS = sizeof worlds / sizeof worlds[0] };
static uint8_t deck[WORLDS], deck_n;
static int last_world = -1;
static const struct world *next_world(void)
{
    if (!deck_n) {
        for (unsigned i = 0; i < WORLDS; ++i) deck[i] = (uint8_t)i;
        for (unsigned i = WORLDS - 1; i > 0; --i) { unsigned j = (unsigned)rr((int)i + 1); uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
        if (deck[WORLDS - 1] == last_world) { uint8_t t = deck[WORLDS - 1]; deck[WORLDS - 1] = deck[0]; deck[0] = t; }
        deck_n = WORLDS;
    }
    last_world = deck[--deck_n];
    return &worlds[last_world];
}
static uint8_t pc(float x) { return (uint8_t)clampi((int)(x * 100.f + .5f), 0, 100); }
static void defaults(struct sn_ring *R, unsigned r)
{
    static const uint8_t ch[SN_LANES] = {S_ANGEL, S_SHIMMER, S_BRAAM, 0, 0}, len[SN_LANES] = {8, 16, 16, 16, 16}, spd[SN_LANES] = {1, 2, 1, 2, 2};
    static const uint8_t era[SN_LANES] = {15, 20, 15, 15, 10}, tone[SN_LANES] = {55, 70, 40, 75, 70}, dec[SN_LANES] = {50, 50, 80, 50, 50};
    memset(R, 0, sizeof *R);
    R->ch = ch[r]; R->len = len[r];
    R->p.oct = 0; R->p.mod = r == SN_HAZE ? 30 : 0; R->p.dec = dec[r]; R->p.wild = 50; R->p.lvl = 80;
    R->p.tone = tone[r]; R->p.era = era[r]; R->p.tape = 0; R->p.sp = 100; R->p.sp_on = 0; R->p.spd = spd[r];
    if (r == SN_KEYS) { R->pad = 1; R->p.era = 5; R->p.tone = 72; R->p.wild = 40; }
}
/* the web app's moves: each shuffle also flips one thing, and says so */
static void world_move(const struct world *W)
{
    int pick = rr(10);
    if (pick < 3) {
        static const uint8_t ok[4] = {SN_DRONE, SN_DRONE, SN_HAZE, SN_GLINT};
        unsigned id = ok[rr(4)];
        if (W->keep & (1U << id)) return;
        sn_song.r[id].mute = 1;
        say2(ring_names[id], " MUTED: MUTE KEY BRINGS IT BACK");
    } else if (pick < 5) {
        struct sn_ring *R = &sn_song.r[SN_GLINT];
        if (W->keep & (1U << SN_GLINT)) return;
        memset(R->st, 0, sizeof R->st); R->mute = 0;
        say("YOUR TURN: PLAY GLINT ON THE KEYS");
    } else if (pick < 7) {
        static const uint8_t lens[3] = {7, 5, 12};
        sn_song.r[SN_GLINT].len = lens[rr(3)];
        say("GLINT LOOPS ODD AGAINST THE PULSE");
    } else if (pick < 8) {
        sn_song.mut = 40;
        say("MUTATE IS ON: LOOPS REWRITE THEMSELVES");
    }
}
static char world_line[48];
void sonar_shuffle(void)
{
    sonar_snap(-1);
    const struct world *W = next_world();
    sn_song.mood = W->moods[rr(2)];
    sn_song.bpm = (uint8_t)(W->bpm0 + rr(W->bpm1 - W->bpm0 + 1));
    sn_song.fx_amt = (uint8_t)clampi(W->space0 + rr(W->space1 - W->space0 + 1), 30, 150);
    sn_song.mut = W->mut;
    int L = (int)scale_len();
    static const int8_t p7[6][4] = {{0, 5, 3, 4}, {0, 4, 5, 3}, {5, 3, 0, 4}, {0, 3, 4, 4}, {0, 2, 3, 4}, {5, 4, 3, 4}};
    static const int8_t p5[3][4] = {{0, 3, 1, 2}, {0, 2, 3, 1}, {0, 4, 3, 1}};
    prog_now = L >= 7 ? p7[rr(6)] : p5[rr(3)];
    /* the world's rings: haze, glint, drone, pulse */
    for (unsigned r = 0; r < SN_RINGS; ++r) {
        const struct world_ring *w = &W->r[r];
        struct sn_ring *R = &sn_song.r[r];
        defaults(R, r);
        R->ch = w->ch;
        if (r == SN_PULSE && R->ch > 1) R->ch = 0;   /* the glitch kit plays as the station kit: its stutters need glitch, left out here */
        unsigned nl = w->len[2] ? 3 : w->len[1] ? 2 : w->len[0] ? 1 : 0;
        R->len = nl ? w->len[rr((int)nl)] : (r == SN_HAZE ? 8 : 16);
        R->p.spd = w->spd;
        R->p.tone = 75; R->p.dec = 50;
        if (w->tone != 255) R->p.tone = w->tone;
        if (w->wild != 255) R->p.wild = w->wild;
        if (w->dec != 255) R->p.dec = w->dec;
        R->p.mod = w->mod != 255 ? w->mod : 0;
        if (w->lvl != 255) R->p.lvl = w->lvl;
        R->p.oct = w->oct;
        R->p.era = (uint8_t)(w->era0 + rr(w->era1 - w->era0 + 1));
        R->p.tape = W->tape ? (uint8_t)clampi(W->tape + rr(21) - 10, 0, 100) : 0;
        R->p.sp = 30; R->p.sp_on = 0;
        compose(r, w->pat);
    }
    memset(world_line, 0, sizeof world_line);
    strcpy(sn_song.name, W->name);
    sn_notice = 0;
    world_move(W);
    if (!sn_notice) {
        char b[8]; int v = sn_song.bpm, k = 0; char t[4]; int m = 0;
        do { t[m++] = (char)('0' + v % 10); v /= 10; } while (v && m < 3);
        while (m) b[k++] = t[--m];
        b[k] = 0;
        strcpy(world_line, W->name); strcat(world_line, "  "); strcat(world_line, b); strcat(world_line, " BPM");
        say(world_line);
    }
    for (unsigned r = 0; r < SN_RINGS; ++r) { sn_rt.pos[r] = -1; }
    apply_all();
    if (!sn_rt.playing) sonar_play(1);
    ++sn_revision;
}

/* ------------------------------------------------------------ undo */
struct snap { uint8_t mask, mood, bpm, mut; char name[24]; struct sn_ring r[SN_LANES]; };
enum { HIST = 16 };
static struct snap hist[HIST];
static unsigned hist_n, hist_w;
void sonar_snap(int ring)
{
    struct snap *h = &hist[hist_w];
    h->mask = ring < 0 ? 0xff : (uint8_t)(1U << ring);
    h->mood = sn_song.mood; h->bpm = sn_song.bpm; h->mut = sn_song.mut; memcpy(h->name, sn_song.name, sizeof h->name);
    memcpy(h->r, sn_song.r, sizeof h->r);
    hist_w = (hist_w + 1) % HIST;
    if (hist_n < HIST) ++hist_n;
}
int sonar_undo(void)
{
    if (!hist_n) { say("NOTHING TO UNDO"); return -1; }
    hist_w = (hist_w + HIST - 1) % HIST; --hist_n;
    struct snap *h = &hist[hist_w];
    for (unsigned r = 0; r < SN_LANES; ++r) if (h->mask & (1U << r)) {
        sn_song.r[r] = h->r[r];
        if (sn_rt.pos[r] >= sn_song.r[r].len) sn_rt.pos[r] = -1;
    }
    if (h->mask & 0x80) { sn_song.mood = h->mood; sn_song.bpm = h->bpm; sn_song.mut = h->mut; memcpy(sn_song.name, h->name, sizeof h->name); }
    apply_all();
    say("UNDONE");
    return 0;
}

/* ------------------------------------------------------------ drift and mutate */
static unsigned count_dots(unsigned r) { unsigned c = 0; for (unsigned i = 0; i < sn_song.r[r].len; ++i) c += sn_song.r[r].st[i].on; return c; }
static void drift_step(void)
{
    struct op { uint8_t kind, ring; uint8_t w; } ops[24];
    unsigned n = 0, live = 0, muted_list[4], nm = 0, live_list[4];
    for (unsigned r = 0; r < SN_RINGS; ++r) { if (sn_song.r[r].mute) muted_list[nm++] = r; else live_list[live++] = r; }
    for (unsigned k = 0; k < live; ++k) {
        unsigned r = live_list[k], c = count_dots(r), len = sn_song.r[r].len;
        unsigned most = r == SN_HAZE || r == SN_DRONE ? 3 : (len * 45 + 99) / 100;
        if (c < most) ops[n++] = (struct op){0, (uint8_t)r, 30};
        if (c > 1) ops[n++] = (struct op){1, (uint8_t)r, 20};
        if (c) ops[n++] = (struct op){2, (uint8_t)r, 20};
        ops[n++] = (struct op){3, (uint8_t)r, 10};
    }
    if (nm) ops[n++] = (struct op){4, (uint8_t)muted_list[rr((int)nm)], 30};
    if (live > 2) ops[n++] = (struct op){5, (uint8_t)live_list[rr((int)live)], 12};
    if (!n) return;
    int tot = 0; for (unsigned i = 0; i < n; ++i) tot += ops[i].w;
    int x = rr(tot); unsigned pick = 0;
    for (unsigned i = 0; i < n; ++i) { x -= ops[i].w; if (x < 0) { pick = i; break; } }
    unsigned r = ops[pick].ring, kind = ops[pick].kind;
    struct sn_ring *R = &sn_song.r[r];
    unsigned dots[SN_STEPS], holes[SN_STEPS], nd = 0, nh = 0;
    for (unsigned i = 0; i < R->len; ++i) { if (R->st[i].on) dots[nd++] = i; else holes[nh++] = i; }
    const char *msg = 0;
    int maxd = (int)sonar_max_degree(r);
    static char line[48];
    if (kind == 0 && nh) {
        int near = nd ? R->st[dots[rr((int)nd)]].d : (r == SN_PULSE ? 0 : 4), d;
        static const int mv[5] = {-2, -1, 1, 2, 3};
        if (r == SN_PULSE) d = frand() < .6f && nd ? near : rr(SN_PIECES);
        else d = clampi(near + PICK(mv), 0, maxd);
        struct sn_dot *dot = &R->st[holes[rr((int)nh)]];
        dot->on = 1; dot->d = (uint8_t)d; dot->y = (uint8_t)(25 + rr(115)); dot->skip = 0;
        msg = " GAINS A NOTE";
    } else if (kind == 1 && nd) { memset(&R->st[dots[rr((int)nd)]], 0, sizeof(struct sn_dot)); msg = " LOSES A NOTE"; }
    else if (kind == 2 && nd) { struct sn_dot *dot = &R->st[dots[rr((int)nd)]]; dot->d = (uint8_t)clampi(dot->d + (rr(2) ? 1 : -1), 0, maxd); msg = " SHIFTS A NOTE"; }
    else if (kind == 3) {
        uint8_t *k = rr(3) == 0 ? &R->p.tone : rr(2) ? &R->p.era : &R->p.mod;
        *k = (uint8_t)clampi(*k + (rr(2) ? 12 : -12), 0, 100);
        sonar_dsp_fade(r, 2000); sonar_dsp_ring(r);
        msg = " CHANGES COLOR";
    } else if (kind == 4) { R->mute = 0; if (!nd) seed(r); sonar_dsp_fade(r, 1700); sonar_dsp_ring(r); msg = " DRIFTS IN"; }
    else if (kind == 5) { R->mute = 1; sonar_dsp_fade(r, 1700); sonar_dsp_ring(r); msg = " FADES OUT"; }
    if (!msg) return;
    memset(line, 0, sizeof line); strcpy(line, "DRIFT: "); strcat(line, ring_names[r]); strcat(line, msg);
    say(line);
}
static void mutate(unsigned r)
{
    float mut = sn_song.mut / 100.f;
    if (mut <= 0 || frand() > mut) return;
    struct sn_ring *R = &sn_song.r[r];
    int ops = 1 + (int)(frand() * mut * 3.f), maxd = (int)sonar_max_degree(r);
    static const int mv[4] = {-2, -1, 1, 2};
    for (int k = 0; k < ops; ++k) {
        unsigned idx[SN_STEPS], n = 0;
        for (unsigned i = 0; i < R->len; ++i) if (R->st[i].on) idx[n++] = i;
        if (!n) return;
        unsigned i = idx[rr((int)n)];
        float q = frand();
        if (q < .6f) R->st[i].d = (uint8_t)clampi(R->st[i].d + PICK(mv), 0, maxd);
        else if (q < .8f) {
            unsigned j = (i + R->len + (rr(2) ? 1 : R->len - 1)) % R->len;
            if (!R->st[j].on) { R->st[j] = R->st[i]; memset(&R->st[i], 0, sizeof R->st[i]); }
        } else R->st[i].y = (uint8_t)clampi(R->st[i].y + (int)((frand() - .5f) * 153.f), 0, 255);
    }
    ++sn_revision;
}

/* ------------------------------------------------------------ the controls */
static uint8_t fading_out;
void sonar_play(int on)
{
    if (on && !fading_out) sonar_dsp_loops_fade(32767, 0);
    if (on) {
        for (unsigned r = 0; r < SN_RINGS; ++r) { sn_rt.pos[r] = -1; sn_rt.wrap[r] = 0; }
        sn_rt.gtick = 0; sn_rt.restart = 1; sn_rt.playing = 1;
    } else { sn_rt.playing = 0; sn_rt.stop_req = 1; }
    ++sn_revision;
}
/* fades: eight seconds, then a fade out stops the loops */
void sonar_fade(int in)
{
    if (in) {
        if (!sn_rt.playing) sonar_play(1);
        sonar_dsp_loops_fade(0, 0);
        sonar_dsp_loops_fade(32767, 2756);
        fading_out = 0;
        say("FADING IN");
    } else {
        if (!sn_rt.playing) return;
        sonar_dsp_loops_fade(0, 2756);
        fading_out = 1;
        say("FADING OUT, THEN STOPPING");
    }
}
void sonar_drift_toggle(void)
{
    sn_drifting = !sn_drifting;
    say(sn_drifting ? "DRIFT ON: A SMALL CHANGE EVERY 2 BARS" : "DRIFT OFF");
    if (sn_drifting && !sn_rt.playing) sonar_play(1);
}
void sonar_wipe(void)
{
    sonar_snap(-1);
    for (unsigned r = 0; r < SN_RINGS; ++r) { memset(sn_song.r[r].st, 0, sizeof sn_song.r[r].st); sn_song.r[r].mute = 0; sonar_dsp_ring(r); }
    say("DISC WIPED (UNDO TO RESTORE)");
}
void sonar_clear_ring(unsigned r) { if (r >= SN_RINGS) return; sonar_snap((int)r); memset(sn_song.r[r].st, 0, sizeof sn_song.r[r].st); say2(ring_names[r], " CLEARED"); }
void sonar_seed_ring(unsigned r) { if (r >= SN_RINGS) return; sonar_snap((int)r); seed(r); say2("NEW LOOP ON ", ring_names[r]); }
void sonar_mute(unsigned r)
{
    if (r >= SN_RINGS) return;
    sn_song.r[r].mute = !sn_song.r[r].mute; sonar_dsp_ring(r);
    say2(ring_names[r], sn_song.r[r].mute ? " MUTED" : " BACK IN");
}
void sonar_remix(unsigned r)
{
    if (r >= SN_RINGS) return;
    struct sn_ring *R = &sn_song.r[r];
    sonar_snap((int)r);
    unsigned n = r == SN_PULSE ? 2 : SN_SOUNDS, c;
    do c = (unsigned)rr((int)n); while (c == R->ch && n > 1);
    R->ch = (uint8_t)c; R->p.era = (uint8_t)rr(101);
    static const int mv[4] = {-2, -1, 1, 2};
    for (unsigned s = 0; s < SN_STEPS; ++s) if (R->st[s].on) {
        R->st[s].y = (uint8_t)rr(256);
        if (frand() < .5f) R->st[s].d = (uint8_t)clampi(R->st[s].d + PICK(mv), 0, (int)sonar_max_degree(r));
    }
    sonar_dsp_ring(r);
    sonar_dsp_audition(r, r == SN_PULSE ? 1 : 4, 204);
    say2(ring_names[r], r == SN_PULSE ? " REMIXED" : " REMIXED, SAME RHYTHM");
}
void sonar_keys_chord(int delta)
{
    struct sn_ring *K = &sn_song.r[SN_KEYS];
    K->pad = (uint8_t)((K->pad + 4 + (delta > 0 ? 1 : -1)) & 3);
    say2("KEYS PLAY ", chord_names[K->pad]);
}
/* the keyboard is chromatic, F3 to G5 like the stock FM-1; with the scale lock each key snaps to the mood's nearest note */
static int deg_of_midi(int m)
{
    unsigned mood = sn_song.mood % SN_MOODS, L = mood_len[mood];
    int o = 0; while (m >= 12) { m -= 12; ++o; } while (m < 0) { m += 12; --o; }
    unsigned i = 0;
    while (i + 1 < L && scales[mood][i + 1] <= m) ++i;
    int next = i + 1 < L ? scales[mood][i + 1] : 12;
    if (next - m < m - scales[mood][i]) ++i;
    return o * (int)L + (int)i;
}
static int midi_of_deg(int d)
{
    unsigned mood = sn_song.mood % SN_MOODS; int L = mood_len[mood], o = 0;
    while (d >= L) { d -= L; ++o; } while (d < 0) { d += L; --o; }
    return 12 * o + scales[mood][d];
}
int sonar_keys_note(unsigned key)
{
    int m = 53 + (int)key + 12 * sn_song.r[SN_KEYS].p.oct;
    return sn_song.keys_lock ? midi_of_deg(deg_of_midi(m)) : m;
}
unsigned sonar_key_degree(unsigned r, unsigned key)
{
    int d = deg_of_midi(53 + (int)key + 12 * sn_song.r[SN_KEYS].p.oct) - deg_of_midi(60);
    if (r == SN_PULSE) { while (d < 0) d += SN_PIECES; while (d >= SN_PIECES) d -= SN_PIECES; return (unsigned)d; }
    return (unsigned)clampi(d, 0, (int)sonar_max_degree(r));
}
const char *sonar_midi_name(int m)
{
    static char b[6]; m = clampi(m, 0, 127);
    int o = m / 12 - 1; const char *n = note_names[m % 12];
    memset(b, 0, sizeof b); strcpy(b, n); b[strlen(b)] = (char)('0' + (o < 0 ? 0 : o));
    return b;
}
const char *sonar_keys_chord_name(void) { return chord_names[sn_song.r[SN_KEYS].pad & 3]; }
void sonar_mood(int delta) { sn_song.mood = (uint8_t)((sn_song.mood + SN_MOODS + (delta > 0 ? 1 : -1)) % SN_MOODS); ++sn_revision; }
void sonar_tempo(int delta) { sn_song.bpm = (uint8_t)clampi(sn_song.bpm + delta, 0, 120); sonar_dsp_tempo(); ++sn_revision; }
void sonar_mutate_amt(int delta) { sn_song.mut = (uint8_t)clampi(sn_song.mut + delta * 5, 0, 100); ++sn_revision; }
void sonar_fx_mode(int delta) { sn_song.fx_mode = (uint8_t)((sn_song.fx_mode + 4 + (delta > 0 ? 1 : -1)) & 3); sonar_dsp_fx(); say2("SPACE ", fx_names[sn_song.fx_mode]); }
void sonar_fx_amt(int delta) { sn_song.fx_amt = (uint8_t)clampi(sn_song.fx_amt + delta * 5, 0, 150); sonar_dsp_fx(); ++sn_revision; }
void sonar_loops_volume(int delta)
{
    sn_song.loops_vol = (uint8_t)clampi(sn_song.loops_vol + delta * 2, 0, 100);
    for (unsigned r = 0; r < SN_RINGS; ++r) sonar_dsp_ring(r);
    ++sn_revision;
}
void sonar_volume(int delta) { sn_song.vol = (uint8_t)clampi(sn_song.vol + delta * 2, 0, 100); sonar_dsp_fx(); ++sn_revision; }
void sonar_sound(unsigned r, int delta)
{
    if (r >= SN_LANES) return;
    unsigned n = r == SN_PULSE ? 2 : r == SN_KEYS ? SN_KEY_SOUNDS : SN_SOUNDS;                  /* the glitch kit needs glitch, which this port leaves out */
    sn_song.r[r].ch = (uint8_t)((sn_song.r[r].ch + n + (unsigned)(delta > 0 ? 1 : n - 1) - n) % n);
    sonar_dsp_ring(r);
    if (r == SN_KEYS) sonar_dsp_keys();
    sonar_dsp_audition(r, r == SN_PULSE ? 1 : 4, 200);
    ++sn_revision;
}
void sonar_param(unsigned r, unsigned which, int delta)
{
    if (r >= SN_LANES) return;
    struct sn_ring *R = &sn_song.r[r];
    uint8_t *p = 0;
    switch (which) {
    case SP_OCT: R->p.oct = (int8_t)clampi(R->p.oct + (delta > 0 ? 1 : -1), -2, 2); break;
    case SP_SPD: R->p.spd = (uint8_t)clampi(R->p.spd + (delta > 0 ? 1 : -1), 0, 3); break;
    case SP_LEN: {
        static const uint8_t lens[8] = {4, 5, 6, 7, 8, 10, 12, 16};
        int i = 0; while (i < 7 && lens[i] < R->len) ++i;
        i = clampi(i + (delta > 0 ? 1 : -1), 0, 7); R->len = lens[i];
        if (sn_rt.pos[r] >= R->len) sn_rt.pos[r] = -1;
        break; }
    case SP_MOD: p = &R->p.mod; break;
    case SP_DEC: p = &R->p.dec; break;
    case SP_WILD: p = &R->p.wild; break;
    case SP_LVL: p = &R->p.lvl; break;
    case SP_TONE: p = &R->p.tone; break;
    case SP_ERA: p = &R->p.era; break;
    case SP_TAPE: p = &R->p.tape; break;
    case SP_SP: p = &R->p.sp; R->p.sp_on = 1; break;
    default: return;
    }
    if (p) *p = (uint8_t)clampi(*p + delta * 2, 0, 100);
    sonar_dsp_ring(r);
    ++sn_revision;
}
/* a white key: play the ring's degree d, and (while catching) write it into the ring */
static uint8_t catch_phase[SN_RINGS];
static const char *key_text;
static char key_line[40];
void sonar_key(unsigned r, unsigned d, unsigned key, unsigned down)
{
    if (r >= SN_RINGS) return;
    unsigned maxd = sonar_max_degree(r);
    if (d > maxd) d = maxd;
    sonar_dsp_live(r, d, sn_brush_y, key, down);
    if (!down || !sn_catching) return;
    struct sn_ring *R = &sn_song.r[r];
    sonar_snap((int)r);
    unsigned len = R->len ? R->len : 16, tgt;
    if (sn_rt.playing) {
        int p = sn_rt.pos[r];
        tgt = p < 0 ? 0 : (unsigned)p;
        R->st[tgt].skip = 0;
    } else { tgt = catch_phase[r] % len; catch_phase[r] = (uint8_t)((tgt + 1) % len); }
    R->st[tgt].on = 1; R->st[tgt].d = (uint8_t)d; R->st[tgt].y = sn_brush_y;
    memset(key_line, 0, sizeof key_line);
    strcpy(key_line, ring_names[r]); strcat(key_line, ": "); strcat(key_line, sonar_note_name(r, d)); strcat(key_line, " ADDED");
    key_text = key_line; say(key_line);
}
void sonar_step_toggle(unsigned r, unsigned step)
{
    if (r >= SN_RINGS || step >= SN_STEPS) return;
    sonar_snap((int)r);
    struct sn_dot *dot = &sn_song.r[r].st[step];
    if (dot->on) { memset(dot, 0, sizeof *dot); }
    else {
        static uint8_t brush_d[SN_RINGS] = {0, 4, 0, 1};
        dot->on = 1; dot->d = brush_d[r]; dot->y = sn_brush_y; dot->skip = 0;
        if (!sn_rt.playing) sonar_dsp_audition(r, dot->d, dot->y);
    }
    ++sn_revision;
}
void sonar_step_edit(unsigned r, unsigned step, int dd, int dy)
{
    if (r >= SN_RINGS || step >= SN_STEPS) return;
    struct sn_dot *dot = &sn_song.r[r].st[step];
    if (!dot->on) return;
    if (dd) { dot->d = (uint8_t)clampi(dot->d + dd, 0, (int)sonar_max_degree(r)); if (!sn_rt.playing) sonar_dsp_audition(r, dot->d, dot->y); }
    if (dy) dot->y = (uint8_t)clampi(dot->y + dy * 13, 0, 255);
    ++sn_revision;
}
void sonar_rotate(unsigned r, int delta)
{
    if (r >= SN_RINGS) return;
    struct sn_ring *R = &sn_song.r[r];
    unsigned len = R->len;
    struct sn_dot t[SN_STEPS];
    for (unsigned i = 0; i < len; ++i) t[(i + len + (delta > 0 ? 1 : len - 1)) % len] = R->st[i];
    memcpy(R->st, t, sizeof(struct sn_dot) * len);
    ++sn_revision;
}

/* ------------------------------------------------------------ songs kept in Flash: eight slots, saved by themselves */
enum { STORE_MAGIC = 0x53314345U /* 'EC1S' */, STORE_VERSION = 5 };
struct record { uint32_t magic, version, generation, crc; struct sn_song song; uint8_t drifting, pad[3]; };
_Static_assert(sizeof(struct record) <= FM1_FLASH_SECTOR_BYTES, "a song fits one Flash sector");
static struct record rec;
static char slot_names[SN_SLOTS][24];
static uint8_t slot_known[SN_SLOTS];
static uint32_t generation, saved_crc, seen_crc, changed_at, last_check, flash_at;
static uint32_t crc32(const void *data, unsigned n, uint32_t c)
{
    const uint8_t *p = data; c = ~c;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U))); }
    return ~c;
}
static uint32_t state_crc(void)
{
    struct sn_song s = sn_song;
    for (unsigned r = 0; r < SN_RINGS; ++r) for (unsigned i = 0; i < SN_STEPS; ++i) s.r[r].st[i].skip = 0;
    return crc32(&s, sizeof s, sn_drifting);
}
static int slot_read(unsigned slot)
{
    if (slot >= SN_SLOTS || fm1_flash_size() < SN_SLOTS * FM1_FLASH_SECTOR_BYTES) return -1;
    if (fm1_flash_read(slot * FM1_FLASH_SECTOR_BYTES, &rec, sizeof rec)) return -1;
    if (rec.magic != STORE_MAGIC || rec.version != STORE_VERSION) return -1;
    return rec.crc == crc32(&rec.song, (unsigned)(sizeof rec - offsetof(struct record, song)), 0) ? 0 : -1;
}
int sonar_store_save(unsigned slot)
{
    if (slot >= SN_SLOTS || fm1_flash_size() < SN_SLOTS * FM1_FLASH_SECTOR_BYTES) return -1;
    memset(&rec, 0, sizeof rec);
    rec.magic = STORE_MAGIC; rec.version = STORE_VERSION; rec.generation = ++generation;
    rec.song = sn_song; rec.drifting = sn_drifting;
    for (unsigned r = 0; r < SN_RINGS; ++r) for (unsigned i = 0; i < SN_STEPS; ++i) rec.song.r[r].st[i].skip = 0;
    rec.crc = crc32(&rec.song, (unsigned)(sizeof rec - offsetof(struct record, song)), 0);
    uint32_t at = slot * FM1_FLASH_SECTOR_BYTES;
    int bad = fm1_flash_erase(at) || fm1_flash_write(at, &rec, sizeof rec);
    flash_at = fm1_millis() | 1U;
    if (bad) return -1;
    slot_known[sn_slot] = 0; sn_slot = (uint8_t)slot; slot_known[slot] = 0;
    saved_crc = seen_crc = state_crc();
    return 0;
}
int sonar_store_load(unsigned slot)
{
    if (slot_read(slot)) return -1;
    sonar_snap(-1);
    sn_song = rec.song; sn_drifting = rec.drifting;
    for (unsigned r = 0; r < SN_RINGS; ++r) if (sn_rt.pos[r] >= sn_song.r[r].len) sn_rt.pos[r] = -1;
    apply_all();
    slot_known[sn_slot] = 0; sn_slot = (uint8_t)slot;
    saved_crc = seen_crc = state_crc();
    ++sn_revision;
    return 0;
}
const char *sonar_store_name(unsigned slot)
{
    if (slot >= SN_SLOTS) return "";
    if (slot == sn_slot) return sn_song.name;
    if (!slot_known[slot]) {
        slot_known[slot] = 1;
        if (!slot_read(slot)) memcpy(slot_names[slot], rec.song.name, sizeof slot_names[slot]);
        else strcpy(slot_names[slot], "EMPTY");
        slot_names[slot][23] = 0;
    }
    return slot_names[slot];
}
static int store_boot(void)
{
    int best = -1; uint32_t best_gen = 0;
    for (unsigned i = 0; i < SN_SLOTS; ++i)
        if (!slot_read(i)) { if (best < 0 || (int32_t)(rec.generation - best_gen) > 0) { best = (int)i; best_gen = rec.generation; } if (rec.generation > generation) generation = rec.generation; }
    if (best < 0 || slot_read((unsigned)best)) return -1;
    sn_song = rec.song; sn_drifting = rec.drifting; sn_slot = (uint8_t)best;
    apply_all();
    saved_crc = seen_crc = state_crc();
    say("WELCOME BACK");
    return 0;
}
/* Autosave: once the song has stayed unchanged for six seconds it is written to its slot. */
static void store_poll(uint32_t now)
{
    if (now - last_check < 1000U) return;
    last_check = now;
    uint32_t c = state_crc();
    if (c != seen_crc) { seen_crc = c; changed_at = now; return; }
    /* never while the loops play: a Flash write can hold up the audio for an instant. SAVE still saves at once */
    if (c != saved_crc && now - changed_at >= 6000U && !sn_rt.playing) (void)sonar_store_save(sn_slot);
}

/* ------------------------------------------------------------ start and the control loop */
void sonar_init(uint32_t seed_value)
{
    rng ^= seed_value * 2654435761u; if (!rng) rng = 1;
    memset(&sn_song, 0, sizeof sn_song);
    for (unsigned r = 0; r < SN_LANES; ++r) defaults(&sn_song.r[r], r);
    for (unsigned r = 0; r < SN_RINGS; ++r) sn_rt.pos[r] = -1;
    sn_song.bpm = 30; sn_song.mut = 15; sn_song.fx_mode = SN_FX_ON; sn_song.fx_amt = 100; sn_song.vol = 100; sn_song.keys_lock = 1; sn_song.loops_vol = 80; sn_song.keys_atk = 50; sn_song.keys_rel = 50;
    strcpy(sn_song.name, "EC-1 SONAR");
    sonar_dsp_init();
    if (store_boot()) {
        sn_first_boot = 1;
        /* the first time: a world to wake up in, waiting for PLAY */
        sonar_shuffle();
        sonar_play(0);
        hist_n = 0;
        sn_drifting = 0;
        say("PLAY STARTS. SCL: A NEW WORLD");
        saved_crc = ~0U;
    }
    sn_rt.playing = 0;
}
void sonar_poll(uint32_t now)
{
    for (unsigned r = 0; r < SN_RINGS; ++r) if (sn_rt.wrap[r]) { sn_rt.wrap[r] = 0; mutate(r); }
    if (sn_rt.drift_due) { sn_rt.drift_due = 0; drift_step(); }
    if (fading_out && sonar_dsp_loops_level() == 0) { fading_out = 0; sonar_play(0); say("STOPPED"); }   /* stays silent until PLAY */
    if (fading_out && !sn_rt.playing) { fading_out = 0; sonar_dsp_loops_fade(32767, 0); }
    store_poll(now);
    /* The work budget follows this FM-1: the platform counts audio blocks that
     * came out late. A late block cuts the budget at once and sets a ceiling just
     * under it; the budget climbs back only to that ceiling, and the ceiling
     * itself rises slowly, so it settles below the edge instead of testing it.
     * Late blocks right after a Flash write are the write's, not the voices'. */
    {
        static uint32_t seen, checked, calm_since, started, ceiling_at, sn_diag_base;
        static int32_t budget = 80, ceiling = 110;
        if (!started) { started = now ? now : 1; calm_since = ceiling_at = now; }
        if (now - checked >= 100U && now - started >= 2000U) {
            checked = now;
            struct fm1_audio_stats st; fm1_audio_stats_read(&st);
            uint32_t ov = st.overruns | 0x80000000U;
            if (!seen) { seen = ov; sn_diag_base = st.overruns; }
            sn_diag_late = st.overruns - sn_diag_base; sn_diag_budget = (uint32_t)budget;
            if (ov != seen) {
                seen = ov;
                if (!(flash_at && now - flash_at < 1500U)) {
                    budget = budget > 34 ? budget - 10 : 24;
                    ceiling = budget; calm_since = ceiling_at = now;
                    sonar_dsp_budget(budget);
                }
            } else {
                if (now - ceiling_at >= 120000U && ceiling < 110) { ceiling += 2; ceiling_at = now; }
                if (now - calm_since >= 4000U && budget < ceiling) { budget += 4; if (budget > ceiling) budget = ceiling; calm_since = now; sonar_dsp_budget(budget); }
            }
        }
    }
    (void)key_text;
}


/* the keys' wave page and envelope page */
static const char *const wave_names[SN_WAVES] = {"PAD", "SINE", "TRIANGLE", "SAW", "SQUARE", "PULSE", "CHOIR"};
const char *sonar_keys_wave_name(void) { return wave_names[sn_song.keys_wave % SN_WAVES]; }
void sonar_keys_edit(unsigned what, int delta)
{
    if (what == 0) sn_song.keys_wave = (uint8_t)((sn_song.keys_wave + SN_WAVES + (delta > 0 ? 1 : SN_WAVES - 1)) % SN_WAVES);
    else if (what == 1) sn_song.keys_detune = (uint8_t)clampi(sn_song.keys_detune + delta, 0, 30);
    else if (what == 2) sn_song.keys_atk = (uint8_t)clampi(sn_song.keys_atk + delta * 2, 0, 100);
    else sn_song.keys_rel = (uint8_t)clampi(sn_song.keys_rel + delta * 2, 0, 100);
    sonar_dsp_keys();
    ++sn_revision;
}
