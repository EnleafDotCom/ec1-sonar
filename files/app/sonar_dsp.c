/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * ec-1 Sonar: the voices, the four rings' tape machines, the space echo and
 * the hall, and the orbit clock. Runs inside the audio interrupt.
 *
 * Integer arithmetic only, no division, no library calls and no constant
 * tables (Flash may be busy while the song autosaves): every table lives in
 * RAM and is filled by sonar_dsp_init() from the control task. Envelopes and
 * LFOs move once per 128-frame block (344.5 Hz), the audio itself per sample.
 *
 * Internal level: 8192 = 1.0, the web app's full scale. */
#include "sonar.h"

#define AC FM1_AUDIO_CODE
/* Small helpers of the audio path go straight into their callers: an audio-code
 * function is never inlined, and a call per sample costs the FM-1 dearly. */
#define HOT static inline __attribute__((always_inline))
/* The control task's table building is kept small; the audio path stays fast. */
#define SETUP __attribute__((minsize))

enum { FS = 44100, BLK = 128, VOICES = 16, NOSC = 6, EVQ = 16, LIVEQ = 16 };
enum { ECHO_LEN = 16384, ECHO_DEC = 8 };
static volatile int32_t work_budget = 80;   /* voice work per block, tuned live by the control task: see sonar_render */            /* the echo runs at 5.5 kHz: its loop is darker than that */
enum { REV_DEC = 4 };                                /* the hall runs at 11 kHz */
enum { RL0 = 1123, RL1 = 1447, RL2 = 1693, RL3 = 1949, AP0 = 142, AP1 = 379 };
enum { W_OFF, W_SINE, W_SAW, W_SQR, W_P25, W_P12, W_TRI, W_VOX, W_TRI4, W_POLY4, W_POLY5, W_PLUCK };
enum { R_DRY, R_LATE, R_MOD, R_RING };

/* ------------------------------------------------------------ tables (RAM, filled by sonar_dsp_init) */
static uint32_t note_inc[130];          /* phase increment per MIDI note */
static int16_t cut_coef[130];           /* SVF f, Q14, index i = 30 Hz x 1.05^i */
static int16_t note_cut[128];           /* MIDI note -> cutoff index x 256 */
static int16_t sine_t[1024];            /* Q13 */
static int16_t wt[6][512];              /* vox, tri4, poly4, poly5, pluck, spare */
static int16_t tanh_t[514];             /* tanh(x) for x in -4..4, Q13 */
static int16_t tanh16_t[514];           /* tanh(1.6x)/tanh(1.6): the app's TANH curve */
static int16_t tia_note[3][128];        /* the Atari's divided pitches, note x 256 */
static uint8_t scale_t[SN_MOODS][8];    /* count, then steps */
static uint32_t chatter_inc[5];
static uint32_t rng = 0x9e3779b9u;
static uint16_t recip16[1024];           /* 65536 / n, so the interrupt never divides */

/* ------------------------------------------------------------ sound designs (filled by the control task) */
struct tpl_osc { uint8_t w, route; int16_t lv0, lv1; int32_t ratio, dk; };
struct tpl {
    uint8_t n, fm, fmode, nmode, ntrack, ctrack, q_sq, wy[3];
    struct tpl_osc o[NOSC];
    int32_t A0, A1, ra0, ra1, kd, sus, kr, rel_blocks, pk0, pk1;
    uint8_t drv_floor, vib_floor, pe_floor;
    int32_t pe0, pe1, pe_k, pe_n;
    int32_t vrate0, vrate1, vdep0, vdep1;
    int32_t fmi0, fmi1, fmk, fmend, brate0, brate1, bdep;
    int32_t mratio[8];
    int32_t c0s, c1s, c0e, c1e, ck0, ck1, cn;
    int32_t q10, q11, q20, q21, stages;
    int32_t crate, cdep0, cdep1;
    int32_t drv0, drv1;
    int32_t nlv0, nlv1, ncut, nq;
    int32_t rmdry0, rmdry1, rmwet0, rmwet1;
    int32_t late_delay, late_k, late0, late1;
    float rawA0, rawA1, rawR;     /* seconds, for the keys' attack and release (control task) */
};
static struct tpl tpls[SN_SOUNDS + SN_KEY_SOUNDS];   /* the rings' sounds, then the keys' pads */

/* each ring's settings, from its tune sheet */
struct ringdsp {
    int32_t gain, gain_tgt, gain_step, send;          /* Q15 */
    int32_t tone_a;                                    /* one-pole Q15 */
    int32_t warm_pre, warm_g, crush_g, crush_l, crush_sh, drop_p; /* Q8, Q15, Q15, levels, log2 levels, dropout odds */
    int32_t tp_pre, tp_post, tp_a, hiss, wow, flut, wdrift; /* Q8, Q15, Q15, Q15, note x256 depths */
    uint16_t wph, fph, dph, winc, finc, dinc;
    int32_t dur;                                       /* note length, blocks */
    int32_t detune;                                    /* random detune range, ratio Q16 */
    int32_t vib_inc, vib_dep;                          /* the ring's mod: vibrato */
    uint8_t div, chipw, kit_crush, gate_x10;
    /* state */
    int32_t lp1, lp2, tlp, hs_x, hs_y, drop, drop_t, kc_lp;
    int32_t quiet;
};
static struct ringdsp rd[SN_LANES];
static int32_t ring_bus[SN_LANES][BLK];
static int32_t ring_tape_pe[SN_LANES];
static volatile int32_t loops_fade = 32767, loops_fade_tgt = 32767, loops_fade_step;   /* PLAY held: the loops fade in or out */

/* ------------------------------------------------------------ voices */
struct osc { uint32_t ph, inc; int32_t ratio, lvl, dk; uint8_t w, route; };
struct voice {
    uint8_t on, ring, stage, key, fm, fmode, nmode, n, stages, burst, kind, has_ring, dying;
    struct osc o[NOSC];
    int32_t note;                      /* note x 256 */
    int32_t pe, pe_step, pe_k, pe_n;
    uint16_t vph, vinc, bph, binc, cph, cinc;
    int32_t vdep, bdep;
    int32_t fmi, fmi_end, fmk;
    int32_t cut, cut_end, cut_k, cut_step, cut_n, cdep, q1, q2, lp1, bp1, lp2, bp2;
    int32_t nlv, ncut, nq, nlp, nbp;
    int32_t drive, rmdry, rmwet;
    int32_t late, late_tgt, late_k, late_wait;
    int32_t amp, pk, sus, kd, kr, atk, atk_step, gate, rel_left, age;
    int32_t blip, blip_n, blip_p;
};
static struct voice v[VOICES];
static int32_t vb_dry[BLK], vb_late[BLK], vb_mod[BLK], vb_ring[BLK];

/* ------------------------------------------------------------ clock, events, live notes */
static int32_t tick_q8 = 0, tick_left = 0;
static int32_t ev_at[EVQ];
static uint8_t ev_ring[EVQ], ev_d[EVQ], ev_y[EVQ], ev_on[EVQ];
static int32_t ev_dur[EVQ];
static volatile uint8_t lq_ring[LIVEQ], lq_d[LIVEQ], lq_y[LIVEQ], lq_key[LIVEQ], lq_kind[LIVEQ];
static volatile uint32_t lq_w, lq_r;
static uint8_t chord_wide[SN_SOUNDS + SN_KEY_SOUNDS];
/* the keys' edits, worked out by the control task */
static uint8_t keys_w;                                   /* oscillator wave, 0 = the pad's own */
static int32_t keys_det;                                 /* detune, ratio Q16 per osc */
static int32_t keys_A0 = 1, keys_A1 = 1, keys_ra0 = 65536, keys_ra1 = 65536, keys_kr, keys_rel = 1;

/* ------------------------------------------------------------ the space */
static int16_t echo_buf[ECHO_LEN];
static uint32_t echo_w;
static int32_t echo_T, echo_wow_ph, echo_flut_ph;      /* delay in samples x 256 (5.5 kHz) */
static int32_t e_in_lp, e_hp_x, e_hp_y, e_lp, e_outL, e_outR, e_prevL, e_prevR;
static int16_t rev_l0[RL0], rev_l1[RL1], rev_l2[RL2], rev_l3[RL3], rev_a0[AP0], rev_a1[AP1];
static uint32_t rev_p0, rev_p1, rev_p2, rev_p3, rev_q0, rev_q1;
static int32_t rev_g[4], rev_d[4], rev_in_lp, r_acc, r_outL, r_outR, r_prevL, r_prevR;
static int32_t fx_echo, fx_rev, fx_e2r, fx_echo_t, fx_rev_t, fx_e2r_t;   /* Q15 */
static int32_t master_vol = 32767, comp_env, comp_gain = 32767, dc_xl, dc_yl, dc_xr, dc_yr;
static unsigned fx_phase;

/* ------------------------------------------------------------ small helpers */
HOT uint32_t xr(void) { uint32_t x = rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rng = x; return x; }
HOT uint32_t rr(uint32_t n) { return ((xr() >> 16) * n) >> 16; }      /* 0 .. n-1 */
HOT int32_t lerp(int32_t a, int32_t b, int32_t y) { return a + (((b - a) * y) >> 8); }      /* y 0..256 */
HOT int32_t clamp32(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
HOT uint32_t inc_of(int32_t note_q8)
{
    note_q8 = clamp32(note_q8, 0, 128 * 256 + 255);
    int32_t n = note_q8 >> 8, f = note_q8 & 255;
    uint32_t a = note_inc[n], b = note_inc[n + 1];
    return a + ((b - a) >> 8) * (uint32_t)f;
}
HOT int32_t coef_of(int32_t idx_q8)
{
    idx_q8 = clamp32(idx_q8, 0, 128 * 256 + 255);
    int32_t n = idx_q8 >> 8, f = idx_q8 & 255;
    return cut_coef[n] + (((cut_coef[n + 1] - cut_coef[n]) * f) >> 8);
}
HOT int32_t sat(const int16_t *t, int32_t x)          /* table shaper, x Q13 over -4..4 */
{
    int32_t p = x + 32768;
    if (p <= 0) return t[0];
    if (p >= 65535) return t[512];
    int32_t i = p >> 7, f = p & 127;
    return t[i] + (((t[i + 1] - t[i]) * f) >> 7);
}
HOT int32_t lfo(uint16_t ph) { return sine_t[ph >> 6]; } /* Q13 */
HOT int32_t kmul(int32_t x, int32_t k) { return (int32_t)(((int64_t)x * k) >> 16); }
HOT int32_t over(int32_t x, int32_t n) { return (int32_t)(((int64_t)x * recip16[clamp32(n, 1, 1023)]) >> 16); }   /* x / n */
static AC int midi_of(int base, int d, unsigned mood)
{
    int L = scale_t[mood][0], o = 0;
    while (d >= L) { d -= L; ++o; }
    while (d < 0) { d += L; --o; }
    return base + 12 * o + scale_t[mood][1 + d];
}

/* ------------------------------------------------------------ starting notes */
static AC struct voice *grab(void)
{
    struct voice *best = &v[0];
    int32_t best_score = 0x7fffffff;
    for (unsigned i = 0; i < VOICES; ++i) {
        if (!v[i].on) return &v[i];
        int32_t s = v[i].amp + (v[i].stage < 2 ? 1 << 20 : 0);
        if (s < best_score) { best_score = s; best = &v[i]; }
    }
    return best;
}
/* the work a voice takes per block: its oscillators, filters and noise */
HOT int32_t voice_cost(const struct voice *x)
{
    return (int32_t)x->n + (x->fmode ? 2 * (int32_t)x->stages : 0) + (x->nmode ? 2 : 0) + (x->has_ring ? 1 : 0) + 1;
}
static AC void clear_voice(struct voice *x)
{
    uint32_t *p = (uint32_t *)x;
    for (unsigned i = 0; i < sizeof *x / 4; ++i) p[i] = 0;
}
/* A melodic note: sound s at MIDI note m, wildness y (0..255), level Q8, gate blocks (-1 held) */
static AC void start_note(unsigned ring, unsigned s, int m, int y, int lvl_q8, int32_t gate, unsigned key)
{
    if (s >= SN_SOUNDS + SN_KEY_SOUNDS) return;
    const struct tpl *t = &tpls[s];
    const struct ringdsp *r = &rd[ring];
    struct voice *x = grab();
    clear_voice(x);
    int32_t yy = y + (y >> 7);                /* 0..256 */
    x->on = 1; x->ring = (uint8_t)ring; x->key = (uint8_t)key; x->kind = (uint8_t)s;
    x->n = t->n; x->fm = t->fm; x->fmode = t->fmode; x->nmode = t->nmode; x->stages = (uint8_t)t->stages;
    x->note = m * 256;
    if (s == S_ATARI) x->note = tia_note[y < 87 ? 0 : y < 171 ? 1 : 2][clamp32(m, 0, 127)];
    unsigned third = y < 85 ? 0 : y < 170 ? 1 : 2;
    for (unsigned i = 0; i < t->n; ++i) {
        struct osc *o = &x->o[i];
        o->w = t->o[i].w; o->route = t->o[i].route;
        if (i == 0 && t->wy[third]) o->w = t->wy[third];
        if (r->chipw && (o->w == W_SAW || o->w == W_SQR)) o->w = r->chipw == 2 ? W_P12 : o->w == W_SQR ? W_SQR : W_P25;
        o->lvl = clamp32(lerp(t->o[i].lv0, t->o[i].lv1, yy), 0, 32767);
        o->dk = t->o[i].dk;
        o->ratio = t->o[i].ratio;
        if (r->detune && o->route != R_MOD) o->ratio += (int32_t)rr((uint32_t)(2 * r->detune + 1)) - r->detune;
        if (s >= SN_SOUNDS && o->route != R_MOD && o->route != R_RING && !(t->fm && i == 0)) {   /* the keys' own edits */
            if (keys_w && o->route == R_DRY) o->w = keys_w;
            if (keys_det && i) o->ratio += (i & 1U) ? (int32_t)(((int64_t)o->ratio * keys_det) >> 16) : -(int32_t)(((int64_t)o->ratio * keys_det) >> 16);
        }
        o->ph = xr();
    }
    if (t->fm) {
        x->o[1].ratio = t->mratio[y >> 5];
        x->fmi = lerp(t->fmi0, t->fmi1, yy); x->fmi_end = kmul(x->fmi, t->fmend); x->fmk = t->fmk;
        x->binc = (uint16_t)lerp(t->brate0, t->brate1, yy); x->bdep = t->bdep;
    }
    /* pitch: an envelope from an offset, a vibrato */
    x->pe = clamp32(lerp(t->pe0, t->pe1, yy), -48 * 256, 48 * 256);
    if (t->pe_floor && x->pe < 0) x->pe = 0;
    if (s == S_LASER) {
        if (y > 204) { x->pe = -24 * 256; x->pe_n = 103; x->pe_step = 119; }
        else { int32_t n = 14 + ((86 * (256 - yy)) >> 8); x->pe_n = n; x->pe_step = -over(x->pe, n); }
    } else if (t->pe_n && x->pe) { x->pe_n = t->pe_n; x->pe_step = -over(x->pe, t->pe_n); }
    else x->pe_k = t->pe_k;
    x->vinc = (uint16_t)lerp(t->vrate0, t->vrate1, yy); x->vdep = lerp(t->vdep0, t->vdep1, yy);
    if (x->vdep < 0) x->vdep = 0;
    if (r->vib_dep) { if (!x->vinc) x->vinc = (uint16_t)r->vib_inc; x->vdep += r->vib_dep; }
    x->vph = (uint16_t)xr();
    /* filter */
    if (t->fmode) {
        int32_t trk = t->ctrack ? note_cut[clamp32(m, 0, 127)] : 0;
        x->cut = lerp(t->c0s, t->c1s, yy) + trk; x->cut_end = lerp(t->c0e, t->c1e, yy) + trk;
        if (t->cn) { x->cut_n = t->cn; x->cut_step = over(x->cut_end - x->cut, t->cn); }
        else x->cut_k = lerp(t->ck0, t->ck1, yy);
        int32_t qy = t->q_sq ? (yy * yy) >> 8 : yy;
        x->q1 = lerp(t->q10, t->q11, qy); x->q2 = lerp(t->q20, t->q21, qy);
        x->cinc = (uint16_t)t->crate; x->cdep = lerp(t->cdep0, t->cdep1, yy); x->cph = 49152;
    }
    if (t->nmode) {
        x->nlv = clamp32(lerp(t->nlv0, t->nlv1, yy), 0, 32767);
        x->ncut = t->ncut + (t->ntrack ? note_cut[clamp32(m, 0, 127)] : 0); x->nq = t->nq;
        if (!x->nlv) x->nmode = 0;
    }
    x->drive = lerp(t->drv0, t->drv1, yy);
    if (t->drv_floor && x->drive < 256) x->drive = 256;
    x->rmdry = clamp32(lerp(t->rmdry0, t->rmdry1, yy), 0, 32767); x->rmwet = clamp32(lerp(t->rmwet0, t->rmwet1, yy), 0, 65535);
    x->late_wait = t->late_delay; x->late_k = t->late_k; x->late_tgt = lerp(t->late0, t->late1, yy);
    /* amplitude */
    x->pk = (lerp(t->pk0, t->pk1, yy) * lvl_q8) >> 8;
    x->sus = t->sus; x->kd = t->kd; x->kr = t->kr; x->rel_left = t->rel_blocks;
    x->atk = clamp32(lerp(t->A0, t->A1, yy), 1, 2000);
    x->atk_step = (int32_t)(((int64_t)x->pk * lerp(t->ra0, t->ra1, yy)) >> 16);
    if (s >= SN_SOUNDS) {          /* the keys' attack and release */
        x->atk = clamp32(lerp(keys_A0, keys_A1, yy), 1, 4000);
        x->atk_step = (int32_t)(((int64_t)x->pk * lerp(keys_ra0, keys_ra1, yy)) >> 16);
        x->kr = keys_kr; x->rel_left = keys_rel;
    }
    if (x->atk_step < 1) x->atk_step = 1;
    if (gate >= 0 && gate < x->atk) gate = x->atk;
    x->gate = gate;
    sn_rt.flash[ring] = 24;
}

/* the pulse ring's pieces: boom, sonar, telemetry, airlock, hull, beam */
static AC struct voice *drum_voice(unsigned ring, int32_t note_q8, int32_t pk, int32_t att, int32_t tau_k)
{
    struct voice *x = grab();
    clear_voice(x);
    x->on = 1; x->ring = (uint8_t)ring; x->kind = 255;
    x->note = note_q8; x->pk = pk; x->atk = att < 1 ? 1 : att; x->atk_step = over(pk, x->atk);
    x->sus = 0; x->kd = tau_k; x->kr = tau_k; x->gate = x->atk; x->rel_left = 3000;
    sn_rt.flash[ring] = 24;
    return x;
}
static int32_t dk_tab[64];                                /* exp(-1/(tau x 344.5)) for tau = 0.02 x 1.1^i, Q16 */
static AC int32_t tau_k(int32_t tau_ms)
{
    /* nearest table entry: tau = 20 ms x 1.1^i */
    int32_t i = 0, t = 20;
    while (i < 63 && t < tau_ms) { t += ((t * 6554) >> 16) + 1; ++i; }
    return dk_tab[i];
}
static AC void hit(unsigned ring, unsigned piece, int y, int32_t oct_q8, int32_t dm_q8)
{
    /* oct_q8: the ring's octave as a pitch offset (2^(oct/2): six semitones a step), note x 256 */
    int32_t yy = y + (y >> 7);
    struct voice *x;
    if (piece == 0) {          /* boom: a sub impact with a rumble tail */
        int32_t n0 = 8978 + ((yy * 1492) >> 8) + oct_q8;              /* 62 Hz, up to +40% */
        x = drum_voice(ring, n0, 32767, 4, tau_k(((700 + ((yy * 800) >> 8)) * dm_q8) >> 8));
        x->n = 1; x->o[0].w = W_SINE; x->o[0].lvl = 32767; x->o[0].ratio = 65536;
        x->pe_n = 413; x->pe_step = -8;                                /* falls to .45 over 1.2 s */
        x = drum_voice(ring, 60 * 256, 18000, 10, tau_k(((900 + ((yy * 1000) >> 8)) * dm_q8) >> 8));
        x->n = 0; x->nmode = 2; x->nlv = 32767; x->ncut = 8783 + ((yy * 4256) >> 8); x->nq = 14600;
    } else if (piece == 1) {   /* sonar: a clean ping that the echo carries away */
        int32_t n0 = 21922 + (int32_t)(((uint32_t)y * 3U) >> 8) * 502 + oct_q8;   /* 1150 Hz, +12% steps */
        x = drum_voice(ring, n0, 9200, 1, tau_k(((500 + ((yy * 600) >> 8)) * dm_q8) >> 8));
        x->n = 2; x->o[0].w = W_SINE; x->o[0].lvl = 32767; x->o[0].ratio = 65536;
        x->o[1].w = W_SINE; x->o[1].lvl = 5850; x->o[1].ratio = 131727; x->o[1].dk = tau_k(200);
        x->pe_n = 135; x->pe_step = -1;                                /* sinks 3% */
    } else if (piece == 2) {   /* telemetry: a burst of computer chatter */
        x = drum_voice(ring, 0, 1966, 1, 65536);
        x->n = 1; x->o[0].w = W_SQR; x->o[0].lvl = 32767; x->o[0].ratio = 65536;
        x->burst = 1; x->blip_n = 3 + (int32_t)rr(4) + ((yy * 6) >> 8); x->blip = 0;
        x->kd = 65536; x->sus = 32767; x->gate = 1000000;
    } else if (piece == 3) {   /* airlock: pressurised hiss sweeping down */
        x = drum_voice(ring, 0, 12450, 41, tau_k(((500 + ((yy * 600) >> 8)) * dm_q8) >> 8));
        x->n = 0; x->nmode = 1; x->nlv = 32767; x->nq = 11703;         /* bandpass, Q 1.4 */
        x->ncut = 27800 + ((oct_q8 * 19) >> 4);                        /* 6 kHz ... */
        x->cut_n = 482 + ((yy * 344) >> 8); x->cut_step = -over(11273, x->cut_n);   /* ... down to 700 Hz */
    } else if (piece == 4) {   /* hull: a resonant metal clang, like a ship creaking */
        int32_t n0 = 13527 + ((yy * 1162) >> 8) + oct_q8;              /* 173 Hz x (1 + .3y) */
        x = drum_voice(ring, n0, 26000, 1, tau_k(((250 + ((yy * 500) >> 8)) * dm_q8) >> 8));
        x->n = 4;
        for (unsigned i = 0; i < 4; ++i) { x->o[i].w = W_SINE; x->o[i].ph = xr(); }
        x->o[0].ratio = 65536;  x->o[0].lvl = 12000; x->o[0].dk = tau_k(60);
        x->o[1].ratio = 167050; x->o[1].lvl = 9000;  x->o[1].dk = tau_k(42);
        x->o[2].ratio = 414050; x->o[2].lvl = 6500;  x->o[2].dk = tau_k(32);
        x->o[3].ratio = 650440; x->o[3].lvl = 4500;  x->o[3].dk = tau_k(28);
    } else {                   /* beam: a charging energy sweep */
        int up = y > 153;
        int32_t n0 = 45 * 256 + oct_q8;                               /* 110 Hz */
        x = drum_voice(ring, up ? n0 : n0 + 36 * 256, 5240, 7, tau_k(((250 + ((yy * 200) >> 8)) * dm_q8) >> 8));
        x->n = 1; x->o[0].w = W_SAW; x->o[0].lvl = 32767; x->o[0].ratio = 65536;
        x->pe_n = 172; x->pe_step = up ? 53 : -53;                     /* eight times the pitch in .5 s */
        x->fmode = 1; x->stages = 1; x->q1 = 6530;                     /* resonant lowpass */
        x->cut = up ? 13590 : 25672; x->cut_n = 172; x->cut_step = up ? 70 : -70;
    }
    (void)x;
}

static AC int wild_of(const struct sn_ring *R, unsigned y)
{
    int ye = (int)y + (int)((R->p.wild * 655U) >> 8) - 128;          /* y + wild - .5 */
    return ye < 0 ? 0 : ye > 255 ? 255 : ye;
}
static AC void play_notes(unsigned ring, unsigned d, unsigned y, int32_t gate, unsigned key)
{
    const struct sn_ring *R = &sn_song.r[ring];
    int ye = wild_of(R, y);
    sn_rt.last_d[ring] = (uint8_t)d;
    if (ring == SN_PULSE) {
        int dec = R->p.dec;                                           /* 2^((dec - .5) x 4): 1/4 .. 4 */
        int32_t dm = dec <= 50 ? 64 + ((dec * 983) >> 8) : 256 + (((dec - 50) * 3932) >> 8);
        unsigned piece = d; while (piece >= SN_PIECES) piece -= SN_PIECES;
        hit(ring, piece, ye, R->p.oct * 6 * 256, dm);
        return;
    }
    unsigned s = R->ch < SN_SOUNDS ? R->ch : 0, mood = sn_song.mood < SN_MOODS ? sn_song.mood : 0;
    if (ring == SN_KEYS) {
        /* the keys: a pad from their own bank, alone or voiced as a chord */
        /* d is the key: the keyboard is chromatic from F3; the scale lock snaps it to the mood */
        s = SN_SOUNDS + (R->ch < SN_KEY_SOUNDS ? R->ch : 0);
        int L = scale_t[mood][0], m = R->pad, note = 53 + (int)d + 12 * R->p.oct;
        int pc = note, o = 0;
        while (pc >= 12) { pc -= 12; ++o; }
        int i = 0;
        while (i + 1 < L && scale_t[mood][2 + i] <= pc) ++i;
        int next = i + 1 < L ? scale_t[mood][2 + i] : 12;
        if (next - pc < pc - scale_t[mood][1 + i]) ++i;
        int dg = o * L + i;
        if (sn_song.keys_lock) note = midi_of(0, dg, mood);
        int o2 = m == 2 ? 2 : 4, o3 = m == 1 ? L : m == 2 ? 4 : L + 2;
        int lvl = m ? 170 : 256;
        start_note(ring, s, note, ye, lvl, gate, key);
        if (m) { start_note(ring, s, midi_of(0, dg + o2, mood), ye, lvl, gate, key); start_note(ring, s, midi_of(0, dg + o3, mood), ye, lvl, gate, key); }
        return;
    }
    int base = (ring == SN_HAZE ? 48 : ring == SN_DRONE ? 36 : 60) + 12 * R->p.oct;
    if (ring == SN_HAZE) {
        int L = scale_t[mood][0], o2 = chord_wide[s] ? 4 : 2, o3 = chord_wide[s] ? L + 2 : 4;
        start_note(ring, s, midi_of(base, (int)d, mood), ye, 159, gate, key);
        start_note(ring, s, midi_of(base, (int)d + o2, mood), ye, 159, gate, key);
        start_note(ring, s, midi_of(base, (int)d + o3, mood), ye, 159, gate, key);
    } else start_note(ring, s, midi_of(base, (int)d, mood), ye, 256, gate, key);
}
static AC void live_up(unsigned key)
{
    for (unsigned i = 0; i < VOICES; ++i) if (v[i].on && v[i].key == key && v[i].stage < 2) v[i].gate = 0;
}

/* ------------------------------------------------------------ the orbit clock */
static AC void push_event(int32_t at, unsigned ring, unsigned d, unsigned y, int32_t dur)
{
    for (unsigned i = 0; i < EVQ; ++i) if (!ev_on[i]) {
        ev_on[i] = 1; ev_at[i] = at; ev_ring[i] = (uint8_t)ring; ev_d[i] = (uint8_t)d; ev_y[i] = (uint8_t)y; ev_dur[i] = dur;
        return;
    }
}
static AC void sched_tick(void)
{
    int32_t g = sn_rt.gtick;
    if (sn_drifting && g > 0 && (g & 63) == 0) sn_rt.drift_due = 1;
    for (unsigned r = 0; r < SN_RINGS; ++r) {
        struct sn_ring *R = &sn_song.r[r];
        unsigned div = rd[r].div ? rd[r].div : 2;
        if ((uint32_t)g & (div - 1)) continue;
        int len = R->len < 1 ? 1 : R->len > SN_STEPS ? SN_STEPS : R->len;
        int p = sn_rt.pos[r] + 1;
        if (p >= len) p = 0;
        sn_rt.pos[r] = (int8_t)p;
        if (p == 0 && g > 0) sn_rt.wrap[r] = 1;
        struct sn_dot *dot = &R->st[p];
        if (!dot->on || R->mute) continue;
        if (dot->skip) { dot->skip = 0; continue; }
        int32_t dur = rd[r].dur;
        unsigned rat = 1;
        /* now and then a note repeats, two or three times inside its step */
        if (r != SN_HAZE && (int32_t)(xr() & 1023) < ((sn_song.mut * 576) >> 8)) rat = (xr() & 1) ? 2 : 3;
        if (rat == 1) { play_notes(r, dot->d, dot->y, dur, 0); continue; }
        int64_t span = (int64_t)tick_q8 * div;
        int32_t sub = rat == 2 ? (int32_t)(span >> 1) : (int32_t)((span * 21846) >> 16);
        int32_t d2 = rat == 2 ? dur >> 1 : (dur * 21846) >> 16;
        if (d2 < 1) d2 = 1;
        play_notes(r, dot->d, dot->y, d2, 0);
        for (unsigned k = 1; k < rat; ++k) push_event(sub * (int32_t)k, r, dot->d, dot->y, d2);
    }
    sn_rt.gtick = g + 1;
}

/* ------------------------------------------------------------ one voice, one block */
static AC int voice_block(struct voice *x, int32_t *bus, int32_t tape_pe)
{
    unsigned n = BLK;
    /* -- control rate -- */
    ++x->age;
    if (x->pe_n > 0) { x->pe += x->pe_step; --x->pe_n; }
    else if (x->pe_k) x->pe = kmul(x->pe, x->pe_k);
    x->vph = (uint16_t)(x->vph + x->vinc);
    int32_t pitch = x->note + x->pe + tape_pe + (x->vdep ? (lfo(x->vph) * x->vdep) >> 13 : 0);
    if (x->burst) {
        if (x->blip <= 0) {
            if (x->blip_n <= 0) return 0;
            --x->blip_n; x->blip = 12 + (int32_t)rr(10); x->blip_p = x->blip;    /* a blip every 35 .. 65 ms */
            x->o[0].inc = chatter_inc[rr(5)];
        }
        --x->blip;
        x->amp = x->blip_p - x->blip <= 9 ? x->pk : 0;                         /* each 25 ms long */
    } else {
        uint32_t base = inc_of(pitch);
        for (unsigned i = 0; i < x->n; ++i) {
            struct osc *o = &x->o[i];
            o->inc = (uint32_t)(((uint64_t)base * (uint32_t)o->ratio) >> 16);
            if (o->dk) o->lvl = kmul(o->lvl, o->dk);
        }
    }
    int32_t f1 = 0, f2 = 0, nf = 0;
    if (x->fmode) {
        if (x->cut_n > 0) { x->cut += x->cut_step; --x->cut_n; }
        else if (x->cut_k) x->cut = x->cut_end + kmul(x->cut - x->cut_end, x->cut_k);
        x->cph = (uint16_t)(x->cph + x->cinc);
        int32_t c = x->cut + (x->cdep ? (lfo(x->cph) * x->cdep) >> 13 : 0);
        f1 = coef_of(c); f2 = f1;
    }
    if (x->nmode) {
        int32_t c = x->ncut;
        if (!x->fmode && x->cut_n > 0) { x->ncut += x->cut_step; --x->cut_n; c = x->ncut; }
        nf = coef_of(c);
    }
    int32_t fmi = 0;
    if (x->fm) {
        x->fmi = x->fmi_end + kmul(x->fmi - x->fmi_end, x->fmk);
        x->bph = (uint16_t)(x->bph + x->binc);
        fmi = x->fmi + (x->bdep ? (int32_t)(((int64_t)x->fmi * x->bdep >> 16) * lfo(x->bph) >> 13) : 0);
    }
    if (x->late_tgt) {
        if (x->late_wait > 0) --x->late_wait;
        else x->late = x->late_tgt + kmul(x->late - x->late_tgt, x->late_k);
    }
    /* amplitude envelope */
    int32_t a0 = x->amp;
    if (!x->burst) {
        if (x->gate >= 0 && x->age > x->gate && x->stage < 2) x->stage = 2;
        if (x->stage == 0) { x->amp += x->atk_step; if (x->amp >= x->pk || x->age >= x->atk) { x->amp = x->pk; x->stage = 1; } }
        else if (x->stage == 1) { int32_t s = (int32_t)(((int64_t)x->pk * x->sus) >> 15); x->amp = s + kmul(x->amp - s, x->kd); }
        else { x->amp = kmul(x->amp, x->kr); if (--x->rel_left <= 0) return 0; }
        if (x->stage > 0 && x->amp < 24 && (x->stage == 2 || x->sus == 0) && x->age > x->atk + 4) return 0;   /* -63 dB: a tail too quiet to hear stops costing time */
    }
    int32_t a1 = x->amp;
    if (!a0 && !a1) return 1;
    /* -- audio rate: oscillators by route -- */
    for (unsigned i = 0; i < n; ++i) { vb_dry[i] = 0; }
    unsigned has_late = 0, has_ring = 0;
    for (unsigned k = 0; k < x->n; ++k) {
        struct osc *o = &x->o[k];
        if (x->fm && k == 0) continue;                    /* the carrier renders after its modulator */
        if (o->route == R_LATE && x->late < 8) continue;  /* a shimmer not yet faded in costs nothing */
        int32_t *b = o->route == R_LATE ? vb_late : o->route == R_MOD ? vb_mod : o->route == R_RING ? vb_ring : vb_dry;
        if (o->route == R_LATE && !has_late) { has_late = 1; for (unsigned i = 0; i < n; ++i) vb_late[i] = 0; }
        if (o->route == R_RING) { has_ring = 1; for (unsigned i = 0; i < n; ++i) vb_ring[i] = 0; }
        if (o->route == R_MOD) for (unsigned i = 0; i < n; ++i) vb_mod[i] = 0;
        uint32_t ph = o->ph, inc = o->inc; int32_t lv = o->lvl; unsigned w = o->w;
        if (w == W_SINE)      for (unsigned i = 0; i < n; ++i) { b[i] += (sine_t[ph >> 22] * lv) >> 15; ph += inc; }
        else if (w == W_SAW)  for (unsigned i = 0; i < n; ++i) { b[i] += ((((int32_t)(ph >> 18)) - 8192) * lv) >> 15; ph += inc; }
        else if (w == W_SQR)  for (unsigned i = 0; i < n; ++i) { b[i] += ((ph < 0x80000000u ? 8192 : -8192) * lv) >> 15; ph += inc; }
        else if (w == W_P25)  for (unsigned i = 0; i < n; ++i) { b[i] += ((ph < 0x40000000u ? 12288 : -4096) * lv) >> 15; ph += inc; }
        else if (w == W_P12)  for (unsigned i = 0; i < n; ++i) { b[i] += ((ph < 0x20000000u ? 14336 : -2048) * lv) >> 15; ph += inc; }
        else if (w == W_TRI)  for (unsigned i = 0; i < n; ++i) { int32_t s = (int32_t)(ph >> 17) - 16384; s = (s < 0 ? -s : s) * 2 - 16384; b[i] += ((s >> 1) * lv) >> 15; ph += inc; }
        else if (w >= W_VOX && w <= W_PLUCK) { const int16_t *tb = wt[w - W_VOX]; for (unsigned i = 0; i < n; ++i) { b[i] += (tb[ph >> 23] * lv) >> 15; ph += inc; } }
        o->ph = ph;
    }
    if (x->fm) {      /* phase modulation: the carrier (osc 0) reads the modulator (osc 1, in vb_mod) */
        struct osc *o = &x->o[0];
        uint32_t ph = o->ph, inc = o->inc; int32_t lv = o->lvl;
        for (unsigned i = 0; i < n; ++i) {
            uint32_t pm = (uint32_t)((vb_mod[i] * fmi) >> 4) * 5215u;   /* index rad x 256: 2^32/(2 pi)/(8192 x 256) x 16 */
            vb_dry[i] += (sine_t[(ph + pm) >> 22] * lv) >> 15; ph += inc;
        }
        o->ph = ph;
    }
    if (x->nmode) {   /* filtered noise */
        int32_t lp = x->nlp, bp = x->nbp, q = x->nq, lv = x->nlv;
        uint32_t s = rng;
        for (unsigned i = 0; i < n; ++i) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            int32_t w = (int32_t)(s >> 18) - 8192;
            lp += (nf * bp) >> 14;
            int32_t hp = w - lp - ((q * bp) >> 14);
            bp += (nf * hp) >> 14;
            bp = clamp32(bp, -80000, 80000);
            int32_t o = x->nmode == 1 ? (bp * q) >> 14 : lp;
            vb_dry[i] += (o * lv) >> 15;
        }
        rng = s; x->nlp = lp; x->nbp = bp;
    }
    if (has_ring) {   /* ring modulation, as the choir does */
        for (unsigned i = 0; i < n; ++i) {
            int32_t d = vb_dry[i];
            vb_dry[i] = ((d * x->rmdry) >> 15) + ((((d * vb_ring[i]) >> 13) * x->rmwet) >> 15);
        }
    }
    if (x->drive) for (unsigned i = 0; i < n; ++i) vb_dry[i] = sat(tanh16_t, (vb_dry[i] * x->drive) >> 8);
    if (x->fmode) {
        int32_t lp = x->lp1, bp = x->bp1, q = x->q1;
        int bpmode = x->fmode == 3;
        for (unsigned i = 0; i < n; ++i) {
            lp += (f1 * bp) >> 14;
            int32_t hp = vb_dry[i] - lp - ((q * bp) >> 14);
            bp += (f1 * hp) >> 14;
            bp = clamp32(bp, -80000, 80000);
            vb_dry[i] = bpmode ? (bp * q) >> 14 : lp;
        }
        x->lp1 = lp; x->bp1 = bp;
        if (x->stages > 1) {
            lp = x->lp2; bp = x->bp2; q = x->q2;
            for (unsigned i = 0; i < n; ++i) {
                lp += (f2 * bp) >> 14;
                int32_t hp = vb_dry[i] - lp - ((q * bp) >> 14);
                bp += (f2 * hp) >> 14;
                bp = clamp32(bp, -80000, 80000);
                vb_dry[i] = lp;
            }
            x->lp2 = lp; x->bp2 = bp;
        }
    }
    if (has_late && x->late >= 8) { int32_t g = x->late; for (unsigned i = 0; i < n; ++i) vb_dry[i] += (vb_late[i] * g) >> 15; }
    /* amplitude, ramped across the block, into the ring */
    int32_t step = (a1 - a0) >> 7, a = a0;
    for (unsigned i = 0; i < n; ++i) { bus[i] += (vb_dry[i] * (a >> 1)) >> 14; a += step; }
    return 1;
}

/* ------------------------------------------------------------ the rings' tape machines */
static AC void ring_block(unsigned r, int32_t *outL, int32_t *outR, int32_t *fx)
{
    struct ringdsp *R = &rd[r];
    int32_t *b = ring_bus[r];
    if (R->gain != R->gain_tgt) {
        R->gain += R->gain_step;
        if (!R->gain_step || (R->gain_step > 0 && R->gain > R->gain_tgt) || (R->gain_step < 0 && R->gain < R->gain_tgt)) R->gain = R->gain_tgt;
    }
    int32_t g = R->gain, send = R->send;
    /* tape dropouts: the level sags as worn oxide passes the head */
    if (R->drop_p && R->drop_t <= 0 && (int32_t)(xr() & 0xfffff) < R->drop_p) { R->drop_t = 20 + (int32_t)rr(55); R->drop = 8000 + (int32_t)rr(13000); }
    int32_t drop = 32767;
    if (R->drop_t > 0) { --R->drop_t; drop = R->drop_t > 8 ? R->drop : 32767 - (((32767 - R->drop) * R->drop_t) >> 3); }
    int32_t lp1 = R->lp1, lp2 = R->lp2, tlp = R->tlp, hx = R->hs_x, hy = R->hs_y, kl = R->kc_lp;
    int32_t ta = R->tone_a, tpa = R->tp_a, pre = R->tp_pre, post = R->tp_post, hiss = R->hiss;
    int32_t wpre = R->warm_pre, wg = R->warm_g, cg = R->crush_g, cl = R->crush_l, csh = R->crush_sh;
    unsigned kit = R->kit_crush;
    uint32_t s = rng;
    for (unsigned i = 0; i < BLK; ++i) {
        int32_t x = b[i];
        if (kit) {      /* the damaged kit: crushed to 15 levels, then dulled */
            int32_t c = clamp32(x, -8192, 8192), q = (c * 7 + (c >= 0 ? 4096 : -4096)) >> 13;
            kl += ((q * 1170 - kl) * 12870) >> 15; x = kl;
        }
        /* the ring's own tape machine: saturation, duller highs, hiss, dropouts */
        x = (sat(tanh_t, (x * pre) >> 8) * post) >> 15;
        tlp += ((x - tlp) * tpa) >> 15; x = tlp;
        if (hiss) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            int32_t w = (int32_t)(s >> 18) - 8192, h = w - hx + ((hy * 19500 + 16384) >> 15); hx = w; hy = h;   /* 3.5 kHz highpass */
            x += (h * hiss) >> 15;
        }
        if (drop != 32767) x = (x * drop) >> 15;
        /* tone */
        lp1 += ((x - lp1) * ta) >> 15; lp2 += ((lp1 - lp2) * ta) >> 15; x = lp2;
        /* era: warm tape saturation, or crushed bits */
        int32_t y = 0;
        if (wg) y = (sat(tanh16_t, (x * wpre) >> 8) * wg) >> 15;
        if (cg) {
            int32_t c = clamp32(x, -8192, 8192), q = (c * cl + (c >= 0 ? 4096 : -4096)) >> 13;
            y += (((q << 13) >> csh) * cg) >> 15;
        }
        y = (y * g) >> 15;
        outL[i] += y; outR[i] += y;
        fx[i] += (y * send) >> 15;
    }
    rng = s;
    R->lp1 = lp1; R->lp2 = lp2; R->tlp = tlp; R->hs_x = hx; R->hs_y = hy; R->kc_lp = kl;
}

/* ------------------------------------------------------------ the space: echo and hall */
HOT int32_t echo_read(int32_t d_q8)
{
    int32_t di = d_q8 >> 8, f = d_q8 & 255;
    uint32_t m = ECHO_LEN - 1;
    int32_t a = echo_buf[(echo_w - (uint32_t)di) & m], b = echo_buf[(echo_w - (uint32_t)di - 1) & m];
    return a + (((b - a) * f) >> 8);
}
static AC void echo_step(int32_t in)
{
    /* tape speed wobble: wow .32 Hz and flutter 5.7 Hz (phase per 5.5 kHz step) */
    echo_wow_ph += 3805; echo_flut_ph += 67776;
    int32_t wow = lfo((uint16_t)(echo_wow_ph >> 10)), flut = lfo((uint16_t)(echo_flut_ph >> 10));
    int32_t T = echo_T;
    int32_t dwow = (wow * 22) >> 5, dflut = (flut * 4) >> 5;    /* .004 s and .0007 s, in samples x 256 */
    int32_t d1 = echo_read(T + dwow + dflut);
    /* the feedback path: highpass 180, lowpass 2400, saturated */
    int32_t h = d1 - e_hp_x + ((e_hp_y * 26580 + 16384) >> 15); e_hp_x = d1; e_hp_y = h;
    e_lp += ((h - e_lp) * 30700) >> 15;
    int32_t loop = sat(tanh_t, (e_lp * 333) >> 8);              /* tanh(1.3x) */
    int32_t w = in + ((loop * 21627) >> 15);                    /* feedback .66 */
    echo_buf[echo_w & (ECHO_LEN - 1)] = (int16_t)clamp32(w, -32768, 32767);
    echo_w = echo_w + 1;
    int32_t t2 = echo_read(T + (T >> 1) + dwow), t3 = echo_read(T * 2 + (T >> 2));
    e_prevL = e_outL; e_prevR = e_outR;
    e_outL = e_lp + ((t2 * 15000) >> 15) + ((t3 * 3000) >> 15);
    e_outR = e_lp + ((t2 * 4500) >> 15) + ((t3 * 11000) >> 15);
}
HOT int32_t ap(int16_t *buf, uint32_t len, uint32_t *p, int32_t x)
{
    int32_t d = buf[*p];
    int32_t y = d - ((x * 20000) >> 15);
    buf[*p] = (int16_t)clamp32(x + ((y * 20000) >> 15), -32768, 32767);
    if (++*p >= len) *p = 0;
    return y;
}
static AC void rev_step(int32_t in)
{
    rev_in_lp += ((in - rev_in_lp) * 26000) >> 15;              /* dark: about 3 kHz */
    int32_t x = ap(rev_a0, AP0, &rev_q0, rev_in_lp);
    x = ap(rev_a1, AP1, &rev_q1, x);
    int32_t a = rev_l0[rev_p0], b = rev_l1[rev_p1], c = rev_l2[rev_p2], d = rev_l3[rev_p3];
    /* damping inside the loop */
    rev_d[0] += ((a - rev_d[0]) * 24000) >> 15; rev_d[1] += ((b - rev_d[1]) * 24000) >> 15;
    rev_d[2] += ((c - rev_d[2]) * 24000) >> 15; rev_d[3] += ((d - rev_d[3]) * 24000) >> 15;
    a = rev_d[0]; b = rev_d[1]; c = rev_d[2]; d = rev_d[3];
    int32_t h0 = (a + b + c + d) >> 1, h1 = (a - b + c - d) >> 1, h2 = (a + b - c - d) >> 1, h3 = (a - b - c + d) >> 1;
    rev_l0[rev_p0] = (int16_t)clamp32(((h0 * rev_g[0]) >> 15) + x, -32768, 32767);
    rev_l1[rev_p1] = (int16_t)clamp32(((h1 * rev_g[1]) >> 15) + x, -32768, 32767);
    rev_l2[rev_p2] = (int16_t)clamp32(((h2 * rev_g[2]) >> 15) - x, -32768, 32767);
    rev_l3[rev_p3] = (int16_t)clamp32(((h3 * rev_g[3]) >> 15) - x, -32768, 32767);
    if (++rev_p0 >= RL0) rev_p0 = 0;
    if (++rev_p1 >= RL1) rev_p1 = 0;
    if (++rev_p2 >= RL2) rev_p2 = 0;
    if (++rev_p3 >= RL3) rev_p3 = 0;
    r_prevL = r_outL; r_prevR = r_outR;
    r_outL = (a + c) >> 1; r_outR = (b + d) >> 1;
}

/* ------------------------------------------------------------ the block */
static int32_t mixL[BLK], mixR[BLK], fxbuf[BLK], keysL[BLK], keysR[BLK];   /* the loops' bus and the keys' own bus */
static AC int32_t log2_q8(uint32_t x)        /* log2(x) x 256 for x >= 1 */
{
    if (!x) return 0;
    int32_t e = 31; while (!(x & 0x80000000u)) { x <<= 1; --e; }
    uint32_t m = (x >> 23) & 255;           /* fraction after the leading one */
    return e * 256 + (int32_t)m + (int32_t)((m * (256 - m) * 11) >> 13);   /* log2(1 + t) bows above t */
}
static AC int32_t exp2_q15(int32_t q8)        /* 2^(q8/256) x 32768, q8 <= 0..+ */
{
    int32_t i = q8 >> 8, f = q8 & 255;
    int32_t m = 32768 + ((f * (22713 + ((f * 10055) >> 8))) >> 8);   /* 2^f, f in 0..1 */
    if (i >= 0) return i > 2 ? 0x7fffffff : m << i;
    return -i >= 15 ? 0 : m >> -i;
}
#ifdef FM1_HOST_TEST
static uint32_t dbg_units, dbg_voices, dbg_rings, dbg_peak;
__attribute__((export_name("sonar_dbg"))) uint32_t sonar_dbg(int reset)
{
    uint32_t v = dbg_peak; if (reset) dbg_peak = 0; return v;
}
#endif
AC void sonar_render(int32_t *output, unsigned frames)
{
#ifdef FM1_HOST_TEST
    dbg_units = dbg_voices = dbg_rings = 0;
#endif
    if (!output || frames != BLK) return;
    /* panel keys */
    while (lq_r != lq_w) {
        unsigned i = lq_r & (LIVEQ - 1);
        unsigned kind = lq_kind[i], ring = lq_ring[i], d = lq_d[i], y = lq_y[i], key = lq_key[i];
        lq_r = lq_r + 1;
        if (ring >= SN_LANES) continue;
        if (kind == 1) play_notes(ring, d, y, ring == SN_PULSE ? 1 : -1, key);
        else if (kind == 2) live_up(key);
        else play_notes(ring, d, y, rd[ring].dur < 1 ? 1 : rd[ring].dur > 2800 ? 2800 : rd[ring].dur, 0);
    }
    if (sn_rt.stop_req) {
        sn_rt.stop_req = 0;
        for (unsigned i = 0; i < VOICES; ++i) if (v[i].on && v[i].stage < 2) v[i].gate = 0;
        for (unsigned i = 0; i < EVQ; ++i) ev_on[i] = 0;
    }
    if (sn_rt.restart) { sn_rt.restart = 0; tick_left = 0; }
    /* the clock */
    if (sn_rt.playing && tick_q8 > 0) {
        tick_left -= BLK * 256;
        int guard = 0;
        while (tick_left <= 0 && guard++ < 8) { sched_tick(); tick_left += tick_q8; }
        for (unsigned i = 0; i < EVQ; ++i) if (ev_on[i]) {
            ev_at[i] -= BLK * 256;
            if (ev_at[i] <= 0) { ev_on[i] = 0; play_notes(ev_ring[i], ev_d[i], ev_y[i], ev_dur[i], 0); }
        }
    }
    for (unsigned r = 0; r < SN_LANES; ++r) {
        if (sn_rt.flash[r]) sn_rt.flash[r] = (uint8_t)(sn_rt.flash[r] - 1);
        struct ringdsp *R = &rd[r];
        R->wph = (uint16_t)(R->wph + R->winc); R->fph = (uint16_t)(R->fph + R->finc); R->dph = (uint16_t)(R->dph + R->dinc);
        ring_tape_pe[r] = ((lfo(R->wph) * R->wow) >> 13) + ((lfo(R->fph) * R->flut) >> 13) + ((lfo(R->dph) * R->wdrift) >> 13);
        for (unsigned i = 0; i < BLK; ++i) ring_bus[r][i] = 0;
    }
    /* The work budget. The FM-1 must finish every block in time or the output
     * buzzes, so when too many voices would sound, the quietest fade out within
     * one block: fading tails first, then the softest notes. */
    {
        int32_t cost = 0;
        for (unsigned i = 0; i < VOICES; ++i) if (v[i].on && !v[i].dying) cost += voice_cost(&v[i]);
        int32_t budget = work_budget;
        while (cost > budget) {
            struct voice *q = 0; int32_t best = 0x7fffffff;
            for (unsigned i = 0; i < VOICES; ++i) {
                struct voice *x = &v[i];
                if (!x->on || x->dying || x->burst) continue;
                int32_t s = x->amp >> (x->stage == 2 ? 2 : 0);      /* a fading tail counts as quieter */
                if (s < best) { best = s; q = x; }
            }
            if (!q) break;
            cost -= voice_cost(q);
            q->stage = 2; q->kr = 31500; q->rel_left = 10; q->gate = 0; q->dying = 1;   /* fades out over about 15 ms: no pop */
        }
    }
    unsigned busy[SN_LANES];
    for (unsigned r = 0; r < SN_LANES; ++r) busy[r] = 0;
    for (unsigned i = 0; i < VOICES; ++i) {
        struct voice *x = &v[i];
        if (!x->on) continue;
        unsigned r = x->ring < SN_LANES ? x->ring : 0;
        if (!voice_block(x, ring_bus[r], ring_tape_pe[r])) x->on = 0;
        busy[r] = 1;
#ifdef FM1_HOST_TEST
        dbg_units += x->n + (x->fmode ? 2U * x->stages : 0U) + (x->nmode ? 2U : 0U) + 1U; ++dbg_voices;
#endif
    }
    for (unsigned i = 0; i < BLK; ++i) { mixL[i] = 0; mixR[i] = 0; fxbuf[i] = 0; keysL[i] = 0; keysR[i] = 0; }
    for (unsigned r = 0; r < SN_LANES; ++r) {
        if (busy[r]) rd[r].quiet = 0;
        else if (rd[r].quiet < 400) ++rd[r].quiet;
        if (rd[r].quiet >= 400) continue;                         /* a silent ring rests after a while */
        if (r == SN_KEYS) ring_block(r, keysL, keysR, fxbuf);    /* the keys skip the loops' compressor: their volumes stay apart */
        else ring_block(r, mixL, mixR, fxbuf);
#ifdef FM1_HOST_TEST
        ++dbg_rings;
#endif
    }
    if (loops_fade != loops_fade_tgt) {
        int32_t f = loops_fade + loops_fade_step;
        if ((loops_fade_step > 0 && f > loops_fade_tgt) || (loops_fade_step < 0 && f < loops_fade_tgt) || !loops_fade_step) f = loops_fade_tgt;
        loops_fade = f;
    }
    /* the space, at reduced rates, glides between modes */
    fx_echo += (fx_echo_t - fx_echo) >> 5; fx_rev += (fx_rev_t - fx_rev) >> 5; fx_e2r += (fx_e2r_t - fx_e2r) >> 5;
    for (unsigned i = 0; i < BLK; ++i) {
        int32_t in = fxbuf[i];
        e_in_lp += ((in - e_in_lp) * 9000) >> 15;
        r_acc += in;
        unsigned ph = fx_phase & 7U;
        if (ph == 0) echo_step(e_in_lp);
        if ((ph & 3U) == 0) {
            int32_t em = (e_outL + e_outR) >> 1;
            rev_step((r_acc >> 2) + ((em * fx_e2r) >> 15));
            r_acc = 0;
        }
        fx_phase = fx_phase + 1;
        int32_t ef = (int32_t)(ph + 1), rf = (int32_t)((ph & 3U) + 1);
        int32_t eL = e_prevL + (((e_outL - e_prevL) * ef) >> 3), eR = e_prevR + (((e_outR - e_prevR) * ef) >> 3);
        int32_t rL = r_prevL + (((r_outL - r_prevL) * rf) >> 2), rR = r_prevR + (((r_outR - r_prevR) * rf) >> 2);
        mixL[i] += ((eL * fx_echo) >> 15) + ((rL * fx_rev) >> 15);
        mixR[i] += ((eR * fx_echo) >> 15) + ((rR * fx_rev) >> 15);
    }
#ifdef FM1_HOST_TEST
    { uint32_t cost = dbg_units + dbg_rings * 4U; if (cost > (dbg_peak & 0xffff)) dbg_peak = cost | (dbg_voices << 16); }
#endif
    /* the compressor: -10 dB with a wide knee, made up, like the app's */
    int32_t peak = 0;
    for (unsigned i = 0; i < BLK; ++i) {
        int32_t a = mixL[i] < 0 ? -mixL[i] : mixL[i], b = mixR[i] < 0 ? -mixR[i] : mixR[i];
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    peak = (peak * 26214) >> 15;                                  /* the mix bus sits at .8 */
    if (peak > comp_env) comp_env += (peak - comp_env) >> 1; else comp_env -= (comp_env - peak) >> 6;
    int32_t e_db = log2_q8((uint32_t)(comp_env < 1 ? 1 : comp_env)) - 13 * 256;      /* re 1.0, octaves x 256 */
    int32_t g_q8;                                                 /* gain, octaves x 256 */
    if (e_db < -768) g_q8 = 212;                                  /* below -18 dB: +5 dB */
    else if (e_db < 0) g_q8 = 212 - (((e_db + 768) * 118) >> 8);
    else g_q8 = 212 - 353 - ((e_db * 213) >> 8);
    int32_t target = exp2_q15(g_q8);
    target = (int32_t)(((int64_t)target * 26214) >> 15);
    int32_t gstep = (target - comp_gain) >> 7, gg = comp_gain;
    int32_t fade = (int32_t)(((int64_t)loops_fade * loops_fade) >> 15);   /* faded after the compressor, on a curve, so it really fades */
    gg = (int32_t)(((int64_t)gg * fade) >> 15); gstep = (int32_t)(((int64_t)gstep * fade) >> 15);
    int32_t mv = master_vol;
    for (unsigned i = 0; i < BLK; ++i) {
        int32_t l = (int32_t)(((int64_t)mixL[i] * gg) >> 15) + ((keysL[i] * 42000) >> 15), r = (int32_t)(((int64_t)mixR[i] * gg) >> 15) + ((keysR[i] * 42000) >> 15);
        gg += gstep;
        int32_t hl = l - dc_xl + ((dc_yl * 32700 + 16384) >> 15), hr = r - dc_xr + ((dc_yr * 32700 + 16384) >> 15);   /* 10 Hz DC blocker */
        dc_xl = l; dc_yl = hl; dc_xr = r; dc_yr = hr;
        l = sat(tanh_t, hl); r = sat(tanh_t, hr);
        l = (l * mv) >> 15; r = (r * mv) >> 15;
        output[2 * i] += l << 10;
        output[2 * i + 1] += r << 10;
    }
    comp_gain = target;
}

/* ------------------------------------------------------------ control task */
SETUP static void live_push(unsigned kind, unsigned ring, unsigned d, unsigned y, unsigned key)
{
    if (lq_w - lq_r >= LIVEQ) return;
    unsigned i = lq_w & (LIVEQ - 1);
    lq_kind[i] = (uint8_t)kind; lq_ring[i] = (uint8_t)ring; lq_d[i] = (uint8_t)d; lq_y[i] = (uint8_t)y; lq_key[i] = (uint8_t)key;
    lq_w = lq_w + 1;
}
SETUP void sonar_dsp_live(unsigned ring, unsigned d, unsigned y, unsigned key, unsigned down) { live_push(down ? 1 : 2, ring, d, y, key + 1); }
SETUP void sonar_dsp_audition(unsigned ring, unsigned d, unsigned y) { live_push(0, ring, d, y, 0); }

/* float helpers for building tables (control task only) */
SETUP static float fexp2(float x)
{
    int i = (int)x; if ((float)i > x) --i;
    float f = x - (float)i, y = 1.f + f * (.6931472f + f * (.2402265f + f * (.0555041f + f * .0096181f)));
    while (i > 0) { y *= 2.f; --i; }
    while (i < 0) { y *= .5f; ++i; }
    return y;
}
SETUP static float flog2(float x)
{
    if (x <= 0) return -100.f;
    int e = 0;
    while (x >= 2.f) { x *= .5f; ++e; }
    while (x < 1.f) { x *= 2.f; --e; }
    float t = (x - 1.f) / (x + 1.f), t2 = t * t;
    return (float)e + 2.885390f * t * (1.f + t2 * (.333333f + t2 * (.2f + t2 * .142857f)));
}
SETUP static float fexp(float x) { return fexp2(x * 1.4426950f); }
SETUP static float fsin(float x)
{
    const float pi = 3.14159265f;
    while (x > pi) x -= 2.f * pi;
    while (x < -pi) x += 2.f * pi;
    float x2 = x * x;
    return x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f * (1.f - x2 / 72.f * (1.f - x2 / 110.f)))));
}
SETUP static float ftanh(float x) { if (x > 9.f) return 1.f; if (x < -9.f) return -1.f; float e = fexp(2.f * x); return (e - 1.f) / (e + 1.f); }
static const float KR = 44100.f / 128.f;
SETUP static int32_t kq(float tau) { if (tau < .0005f) return 0; return (int32_t)(fexp(-1.f / (tau * KR)) * 65536.f); }
SETUP static int32_t cidx(float hz) { if (hz < 30.f) hz = 30.f; return (int32_t)(flog2(hz / 30.f) / .0703893f * 256.f); }   /* log2(1.05) */
SETUP static int32_t cents_q16(float c) { return (int32_t)(fexp2(c / 1200.f) * 65536.f); }
SETUP static int32_t blocks(float s) { int32_t b = (int32_t)(s * KR + .5f); return b < 1 ? 1 : b; }
SETUP static int32_t damp(float q) { return (int32_t)(16384.f / q); }
SETUP static float qdb(float db) { return fexp2(db / 6.0206f); }     /* the web lowpass Q is in dB */
SETUP static int32_t lv(float x) { return (int32_t)(x * 32767.f); }

static struct tpl *T;
SETUP static void tpl_begin(unsigned s)
{
    T = &tpls[s]; uint8_t *p = (uint8_t *)T; for (unsigned i = 0; i < sizeof *T; ++i) p[i] = 0;
    T->q10 = T->q11 = T->q20 = T->q21 = damp(qdb(1)); T->stages = 1;      /* the web lowpass default, Q 1 dB */
}
SETUP static void o_add(unsigned w, float ratio, float lv0, float lv1, unsigned route)
{
    struct tpl_osc *o = &T->o[T->n++];
    o->w = (uint8_t)w; o->route = (uint8_t)route; o->ratio = (int32_t)(ratio * 65536.f);
    o->lv0 = (int16_t)lv(lv0); o->lv1 = (int16_t)lv(lv1);
}
SETUP static float ct(float cents) { return fexp2(cents / 1200.f); }
/* attack (at y = 0 and 1), decay (time constant D/3 to the sustain), sustain, release (R/4), peak */
SETUP static void amp(float A0, float A1, float D, float S, float R, float pk0, float pk1)
{
    T->A0 = blocks(A0); T->A1 = blocks(A1); T->ra0 = 65536 / T->A0; T->ra1 = 65536 / T->A1; T->rawA0 = A0; T->rawA1 = A1; T->rawR = R;
    T->kd = kq(D / 3.f > .005f ? D / 3.f : .005f); T->sus = lv(S); T->kr = kq(R / 4.f); T->rel_blocks = blocks(R * 1.6f + .05f);
    T->pk0 = lv(pk0); T->pk1 = lv(pk1);
}
/* a lowpass from a start cutoff to an end cutoff (each at y = 0 and 1), gliding with a time constant */
SETUP static void lp(float s0, float s1, float e0, float e1, float tau0, float tau1)
{
    T->fmode = 1; T->c0s = cidx(s0); T->c1s = cidx(s1); T->c0e = cidx(e0); T->c1e = cidx(e1); T->ck0 = kq(tau0); T->ck1 = kq(tau1);
}
SETUP static void q1(float db0, float db1) { T->q10 = damp(qdb(db0)); T->q11 = damp(qdb(db1)); }
SETUP static int32_t hzrate(float hz) { return (int32_t)(hz / KR * 65536.f); }
SETUP static int32_t cq8(float cents) { return (int32_t)(cents / 100.f * 256.f); }

SETUP static void build_sounds(void)
{
    /* the pads voice their chords wide: root, fifth, the third an octave up */
    chord_wide[S_ANGEL] = chord_wide[S_HALO] = chord_wide[S_BLOOM] = chord_wide[S_VOX] = 1;

    tpl_begin(S_MOOG);      /* analog: two saws and a sub square, driven into a resonant ladder */
    o_add(W_SAW, 1, .38f, .38f, R_DRY); o_add(W_SAW, 1.007f, .38f, .38f, R_DRY); o_add(W_SQR, .5f, .133f, .133f, R_DRY);
    T->drv0 = 256; T->drv1 = 7 * 256; T->drv_floor = 1;
    lp(360, 16000, 120, 2205, .18f, .06f); T->stages = 2; T->q_sq = 1;
    q1(.7f, .7f); T->q20 = damp(qdb(1)); T->q21 = damp(qdb(18));
    amp(.004f, .004f, .4f, .6f, .12f, .55f, .55f);

    tpl_begin(S_GLASS);     /* bell: FM, the index falling away */
    o_add(W_SINE, 1, 1, 1, R_DRY); o_add(W_SINE, 2, 1, 1, R_MOD); T->fm = 1;
    for (int b = 0; b < 8; ++b) {
        float y = ((float)b + .5f) / 8.f, r = y < .7f ? 2.f + (float)(int)(y * 4.f + .5f) * .5f : 1.414f + (y - .7f) * 20.f;
        T->mratio[b] = (int32_t)(r * 65536.f);
    }
    T->fmi0 = (int32_t)(1.2f * 256.f); T->fmi1 = (int32_t)(10.2f * 256.f); T->fmend = 5243; T->fmk = kq(.45f);
    amp(.002f, .002f, 1.6f, 0, .9f, .42f, .42f);

    tpl_begin(S_NES);       /* computer: chip pulses, a pitch blip when wild */
    o_add(W_P25, 1, 1, 1, R_DRY); T->wy[0] = W_SQR; T->wy[1] = W_P25; T->wy[2] = W_P12;
    { float top = 12.f * flog2(3.4f) * 256.f; T->pe1 = (int32_t)top; T->pe0 = (int32_t)(-top * .7f / .3f); }
    T->pe_n = blocks(.08f); T->pe_floor = 1;
    amp(.001f, .001f, .25f, .7f, .04f, .26f, .26f);

    tpl_begin(S_ATARI);     /* lo-res: divided pitches, buzzing polynomial waves */
    o_add(W_SQR, 1, 1, 1, R_DRY); T->wy[0] = W_SQR; T->wy[1] = W_POLY4; T->wy[2] = W_POLY5;
    amp(.001f, .001f, .3f, .8f, .03f, .26f, .26f);

    tpl_begin(S_VOX);       /* choir: a sung vowel, ring modulated when wild */
    o_add(W_VOX, 1, 1, 1, R_DRY); o_add(W_SINE, 1.5f, 1, 1, R_RING);
    T->rmdry0 = lv(1); T->rmdry1 = lv(.2f); T->rmwet0 = 0; T->rmwet1 = lv(1.3f);
    T->vrate0 = T->vrate1 = hzrate(5.5f); T->vdep0 = T->vdep1 = cq8(7);
    lp(1500, 7500, 1500, 7500, .01f, .01f);
    amp(.08f, .08f, .5f, .8f, .3f, .6f, .6f);

    tpl_begin(S_PLUCK);     /* harp: a plucked string through a lowpass */
    o_add(W_PLUCK, 1, 1, 1, R_DRY);
    T->drv0 = -3431; T->drv1 = 2714; T->drv_floor = 1;            /* only drives past y .6 */
    lp(9000, 12000, 700, 11200, .2f, .3f);
    amp(.001f, .001f, 3.0f, 0, .3f, .7f, .7f);

    tpl_begin(S_BLOOM);     /* pad: three saws opening up */
    o_add(W_SAW, ct(-9), .3f, .3f, R_DRY); o_add(W_SAW, 1, .3f, .3f, R_DRY); o_add(W_SAW, ct(9), .3f, .3f, R_DRY);
    o_add(W_TRI, 2, 0, .15f, R_DRY);
    lp(300, 300, 500, 4000, .5f, .5f); q1(.8f, .8f);
    T->vrate0 = T->vrate1 = hzrate(5.5f); T->vdep0 = -cq8(96); T->vdep1 = cq8(41);   /* vibrato only past y .7 */
    amp(.75f, .35f, 1, .8f, 1.2f, .26f, .26f);

    tpl_begin(S_LASER);     /* comet: a pitch that streaks down into the note */
    o_add(W_SINE, 1, 1, 1, R_DRY); T->wy[0] = W_SINE; T->wy[1] = W_SINE; T->wy[2] = W_SQR;
    T->pe0 = (int32_t)(12.f * flog2(4.f) * 256.f); T->pe1 = (int32_t)(12.f * flog2(16.f) * 256.f);
    amp(.001f, .001f, .25f, .35f, .1f, .35f, .18f);

    tpl_begin(S_SHIMMER);   /* stacked sine partials with a slow bloom and a breath of noise */
    o_add(W_SINE, 1, .3f, .3f, R_DRY); o_add(W_SINE, 2, .135f, .135f, R_DRY); o_add(W_SINE, 3, .048f, .048f, R_DRY);
    o_add(W_SINE, 4.01f, .018f, .06f, R_DRY);
    T->nmode = 1; T->ntrack = 1; T->ncut = cidx(30.f * 4.f); T->nq = damp(8); T->nlv0 = lv(-.072f); T->nlv1 = lv(.108f);
    amp(2.0f, .8f, 2, .85f, 2.5f, .34f, .34f);

    tpl_begin(S_THEREMIN);  /* a sine that scoops up into pitch with a singing vibrato */
    o_add(W_SINE, 1, 1, 1, R_DRY); o_add(W_TRI, 1, .18f, .18f, R_DRY);
    T->pe0 = T->pe1 = (int32_t)(12.f * flog2(.94f) * 256.f); T->pe_k = kq(.06f);
    T->vrate0 = hzrate(5.5f); T->vrate1 = hzrate(7.5f); T->vdep0 = cq8(10.4f); T->vdep1 = cq8(62);
    amp(.12f, .12f, 1, .9f, .7f, .3f, .3f);

    tpl_begin(S_BRAAM);     /* a huge detuned brass cluster through a slowly opening filter */
    o_add(W_SAW, ct(-24), .18f, .18f, R_DRY); o_add(W_SAW, ct(-8), .18f, .18f, R_DRY); o_add(W_SAW, ct(6), .18f, .18f, R_DRY);
    o_add(W_SAW, ct(21), .18f, .18f, R_DRY); o_add(W_SAW, .5f, .08f, .08f, R_DRY);
    T->drv0 = 384; T->drv1 = 1408;
    lp(140, 140, 500, 3100, .5f, .5f); q1(2, 8);
    amp(.3f, .3f, 2.5f, .8f, 1.6f, .4f, .4f);

    tpl_begin(S_SIGNAL);    /* alien FM: inharmonic ratios and a slowly breathing index */
    o_add(W_SINE, 1, 1, 1, R_DRY); o_add(W_SINE, 1.414f, 1, 1, R_MOD); T->fm = 1;
    { static const float rs[4] = {1.414f, 3.14f, 2.71f, .5f}; for (int b = 0; b < 8; ++b) T->mratio[b] = (int32_t)(rs[b / 2] * 65536.f); }
    T->fmi0 = 256; T->fmi1 = 7 * 256; T->fmend = 65536; T->fmk = 0;
    T->brate0 = hzrate(.3f); T->brate1 = hzrate(3.3f); T->bdep = 52429;
    amp(.4f, .4f, 2, .7f, 1.4f, .3f, .3f);

    tpl_begin(S_ANGEL);     /* a choir that fades in slowly, with an octave of air above it */
    o_add(W_VOX, 1, .16f, .16f, R_DRY); o_add(W_VOX, 2, .024f, .084f, R_DRY);
    o_add(W_SINE, ct(-6), .09f, .09f, R_DRY); o_add(W_SINE, ct(6), .09f, .09f, R_DRY);
    o_add(W_SINE, 2, .044f, .044f, R_DRY); o_add(W_SINE, 3.0052f, .012f, .036f, R_DRY);
    T->nmode = 1; T->ntrack = 1; T->ncut = cidx(30.f * 3.f); T->nq = damp(4); T->nlv0 = lv(-.015f); T->nlv1 = lv(.035f);
    T->vrate0 = T->vrate1 = hzrate(5.5f); T->vdep0 = T->vdep1 = cq8(7);
    lp(2500, 7500, 2500, 7500, .01f, .01f); q1(.5f, .5f);
    amp(3.0f, 1.6f, 3, .9f, 3.5f, .42f, .42f);

    tpl_begin(S_HALO);      /* a wide detuned pad that breathes, with a shimmer that swells in late */
    o_add(W_SAW, ct(-14), .145f, .145f, R_DRY); o_add(W_SAW, ct(-5), .145f, .145f, R_DRY); o_add(W_SAW, ct(5), .145f, .145f, R_DRY);
    o_add(W_SAW, ct(14), .145f, .145f, R_DRY); o_add(W_SINE, 2, 1, 1, R_LATE); o_add(W_SINE, 3, 1, 1, R_LATE);
    lp(400, 400, 900, 3900, 1.2f, 1.2f); q1(.6f, .6f);
    T->crate = hzrate(.12f); T->cdep0 = cidx(1150) - cidx(900); T->cdep1 = cidx(4500) - cidx(3900);
    T->late_delay = blocks(1); T->late_k = kq(1.5f); T->late0 = lv(.07f); T->late1 = lv(.17f);
    amp(3.5f, 2, 3, .9f, 4, .4f, .4f);

    /* ---- the keys' own bank: pads and angelic tones to play over the loops ---- */
    tpl_begin(SN_SOUNDS + 0);   /* SERAPH: a choir with an octave of air and a halo of partials */
    o_add(W_VOX, 1, .18f, .18f, R_DRY); o_add(W_VOX, ct(7), .12f, .12f, R_DRY); o_add(W_VOX, 2, .03f, .08f, R_DRY);
    o_add(W_SINE, ct(-5), .1f, .1f, R_DRY); o_add(W_SINE, 2, 1, 1, R_LATE); o_add(W_SINE, 3.003f, .5f, .5f, R_LATE);
    T->vrate0 = T->vrate1 = hzrate(5.2f); T->vdep0 = T->vdep1 = cq8(8);
    lp(2200, 6000, 2200, 6000, .01f, .01f); q1(.5f, .5f);
    T->late_delay = blocks(.6f); T->late_k = kq(1.2f); T->late0 = lv(.04f); T->late1 = lv(.1f);
    amp(1.4f, .7f, 3, .9f, 4.5f, .66f, .66f);

    tpl_begin(SN_SOUNDS + 1);   /* HALO PAD: wide detuned saws that breathe, shimmer swelling in late */
    o_add(W_SAW, ct(-12), .14f, .14f, R_DRY); o_add(W_SAW, ct(-4), .14f, .14f, R_DRY); o_add(W_SAW, ct(4), .14f, .14f, R_DRY);
    o_add(W_SAW, ct(12), .14f, .14f, R_DRY); o_add(W_SINE, 2, 1, 1, R_LATE); o_add(W_SINE, 3, 1, 1, R_LATE);
    lp(350, 500, 1000, 3200, 1, 1); q1(.6f, .6f);
    T->crate = hzrate(.15f); T->cdep0 = cidx(1300) - cidx(1000); T->cdep1 = cidx(3900) - cidx(3200);
    T->late_delay = blocks(.8f); T->late_k = kq(1.4f); T->late0 = lv(.06f); T->late1 = lv(.14f);
    amp(1.2f, .6f, 3, .9f, 4.5f, .5f, .5f);

    tpl_begin(SN_SOUNDS + 2);   /* CHOIR: two singers a little apart, gentle vibrato */
    o_add(W_VOX, ct(-8), .32f, .32f, R_DRY); o_add(W_VOX, ct(8), .32f, .32f, R_DRY); o_add(W_VOX, .5f, .0f, .12f, R_DRY);
    T->vrate0 = hzrate(4.8f); T->vrate1 = hzrate(5.6f); T->vdep0 = cq8(10); T->vdep1 = cq8(22);
    lp(3500, 7000, 3500, 7000, .01f, .01f); q1(.5f, .5f);
    amp(.45f, .25f, 2, .85f, 2.6f, .5f, .5f);

    tpl_begin(SN_SOUNDS + 3);   /* CATHEDRAL: an organ of pure partials, a chorus of slight detune */
    o_add(W_SINE, 1, .34f, .34f, R_DRY); o_add(W_SINE, ct(4), .2f, .2f, R_DRY); o_add(W_SINE, 2, .2f, .2f, R_DRY);
    o_add(W_SINE, 3, .1f, .14f, R_DRY); o_add(W_SINE, 4, .05f, .09f, R_DRY); o_add(W_SINE, 6, 1, 1, R_LATE);
    T->late_delay = blocks(.4f); T->late_k = kq(1); T->late0 = lv(.02f); T->late1 = lv(.05f);
    amp(.3f, .15f, 2, .9f, 3.5f, .48f, .48f);

    tpl_begin(SN_SOUNDS + 4);   /* AURORA: shimmering sine stack with a breath of air */
    o_add(W_SINE, 1, .3f, .3f, R_DRY); o_add(W_SINE, 2.001f, .15f, .15f, R_DRY); o_add(W_SINE, 3.003f, .07f, .1f, R_DRY);
    o_add(W_SINE, 4.01f, .03f, .07f, R_DRY); o_add(W_SINE, ct(9), .12f, .12f, R_DRY);
    T->nmode = 1; T->ntrack = 1; T->ncut = cidx(30.f * 4.f); T->nq = damp(6); T->nlv0 = lv(.01f); T->nlv1 = lv(.06f);
    T->vrate0 = T->vrate1 = hzrate(.4f); T->vdep0 = T->vdep1 = cq8(6);
    amp(1.6f, .8f, 3, .88f, 5, .55f, .55f);

    tpl_begin(SN_SOUNDS + 5);   /* GLASS PAD: a soft FM bell that blooms instead of strikes */
    o_add(W_SINE, 1, 1, 1, R_DRY); o_add(W_SINE, 3, 1, 1, R_MOD); T->fm = 1;
    for (int b = 0; b < 8; ++b) T->mratio[b] = (int32_t)((b < 4 ? 3.f : b < 6 ? 2.f : 3.5f) * 65536.f);
    T->fmi0 = (int32_t)(.8f * 256.f); T->fmi1 = (int32_t)(2.6f * 256.f); T->fmend = 26214; T->fmk = kq(2.5f);
    amp(.5f, .25f, 3, .8f, 4, .32f, .32f);

    tpl_begin(SN_SOUNDS + 6);   /* STRINGS: slow bowed saws opening up */
    o_add(W_SAW, ct(-9), .26f, .26f, R_DRY); o_add(W_SAW, 1, .26f, .26f, R_DRY); o_add(W_SAW, ct(9), .26f, .26f, R_DRY);
    lp(500, 700, 1100, 3000, 1, .7f); q1(.8f, .8f);
    T->vrate0 = T->vrate1 = hzrate(5); T->vdep0 = T->vdep1 = cq8(9);
    amp(.7f, .35f, 2, .85f, 3, .34f, .34f);

    tpl_begin(SN_SOUNDS + 7);   /* ETHER: open fifths and octaves, very slow to arrive and leave */
    o_add(W_SINE, 1, .3f, .3f, R_DRY); o_add(W_SINE, 1.4983f, .14f, .18f, R_DRY); o_add(W_SINE, 2, .12f, .12f, R_DRY);
    o_add(W_VOX, 1, .06f, .12f, R_DRY); o_add(W_SINE, 4, 1, 1, R_LATE);
    T->late_delay = blocks(1); T->late_k = kq(2); T->late0 = lv(.03f); T->late1 = lv(.07f);
    T->vrate0 = T->vrate1 = hzrate(.25f); T->vdep0 = T->vdep1 = cq8(5);
    amp(2.2f, 1.2f, 3, .92f, 6, .55f, .55f);
}


SETUP void sonar_dsp_init(void)
{
    recip16[0] = 65535; recip16[1] = 65535; for (int n = 2; n < 1024; ++n) recip16[n] = (uint16_t)(65536 / n);
    for (int n = 0; n < 130; ++n) note_inc[n] = (uint32_t)(440.f * fexp2(((float)n - 69.f) / 12.f) * 97391.548f);
    for (int i = 0; i < 130; ++i) {
        float hz = 30.f * fexp2((float)i * .0703893f), c = 2.f * fsin(3.14159265f * (hz > 15000.f ? 15000.f : hz) / 44100.f);
        if (c > 1.f) c = 1.f;
        cut_coef[i] = (int16_t)(c * 16384.f);
    }
    for (int n = 0; n < 128; ++n) note_cut[n] = (int16_t)cidx(440.f * fexp2(((float)n - 69.f) / 12.f));
    for (int i = 0; i < 1024; ++i) sine_t[i] = (int16_t)(fsin((float)i / 1024.f * 6.2831853f) * 8191.f);
    for (int i = 0; i < 514; ++i) {
        float x = ((float)i - 256.f) / 64.f;
        tanh_t[i] = (int16_t)(ftanh(x) * 8191.f);
        tanh16_t[i] = (int16_t)(ftanh(x * 1.6f) / .9216686f * 8191.f * .999f);
    }
    /* the choir's vowel (the web app's VOX: formants at 730, 1090, 2440 Hz for a C4 voice) */
    {
        float a[21]; float peak = 0;
        static float tmp[512];
        for (int k = 1; k <= 20; ++k) {
            float fk = (float)k * 261.63f, s = 0;
            static const float F[3][3] = {{730, 1, 180}, {1090, .5f, 220}, {2440, .25f, 340}};
            for (int j = 0; j < 3; ++j) { float d = fk - F[j][0]; s += F[j][1] * fexp(-(d * d) / (2.f * F[j][2] * F[j][2])); }
            a[k] = s + .015f;
        }
        for (int i = 0; i < 512; ++i) {
            float ph = (float)i / 512.f * 6.2831853f, s = 0;
            for (int k = 1; k <= 20; ++k) s += a[k] * fsin(ph * (float)k);
            tmp[i] = s; if (s > peak) peak = s; if (-s > peak) peak = -s;
        }
        for (int i = 0; i < 512; ++i) wt[0][i] = (int16_t)(tmp[i] / peak * 8191.f * .9f);
        /* pluck: a bright string, partials falling off */
        peak = 0;
        for (int i = 0; i < 512; ++i) {
            float ph = (float)i / 512.f * 6.2831853f, s = 0;
            for (int k = 1; k <= 24; ++k) s += fsin(ph * (float)k) / ((float)k * (1.f + (float)k * .15f));
            tmp[i] = s; if (s > peak) peak = s; if (-s > peak) peak = -s;
        }
        for (int i = 0; i < 512; ++i) wt[4][i] = (int16_t)(tmp[i] / peak * 8191.f * .9f);
    }
    /* 4-bit triangle and the Atari's polynomial waves */
    for (int i = 0; i < 512; ++i) { int s = i * 32 / 512, t = s < 16 ? 15 - s : s - 16; wt[1][i] = (int16_t)(((float)t / 7.5f - 1.f) * 8191.f); }
    {
        int r = 15; int8_t p4[15]; for (int i = 0; i < 15; ++i) { r = ((r << 1) | (((r >> 3) ^ (r >> 2)) & 1)) & 15; p4[i] = (int8_t)(r & 1 ? 1 : -1); }
        int q = 31; int8_t p5[31]; for (int i = 0; i < 31; ++i) { q = ((q << 1) | (((q >> 4) ^ (q >> 2)) & 1)) & 31; p5[i] = (int8_t)(q & 1 ? 1 : -1); }
        int m4 = 0, m5 = 0; for (int i = 0; i < 15; ++i) m4 += p4[i]; for (int i = 0; i < 31; ++i) m5 += p5[i];
        for (int i = 0; i < 512; ++i) { wt[2][i] = (int16_t)(p4[i * 15 / 512] * 7000 - m4 * 7000 / 15); wt[3][i] = (int16_t)(p5[i * 31 / 512] * 7000 - m5 * 7000 / 31); }
    }
    /* the Atari's TIA: pitches 31400 / (k x (a + 1)) */
    {
        static const int ks[3][2] = {{2, 6}, {15, 45}, {31, 93}};
        for (int set = 0; set < 3; ++set) for (int n = 0; n < 128; ++n) {
            float f = 440.f * fexp2(((float)n - 69.f) / 12.f), best = f, bd = 1e9f;
            for (int j = 0; j < 2; ++j) for (int a = 0; a < 32; ++a) {
                float c = 31400.f / (float)(ks[set][j] * (a + 1)), dd = flog2(c / f); if (dd < 0) dd = -dd;
                if (dd < bd) { bd = dd; best = c; }
            }
            tia_note[set][n] = (int16_t)((69.f + 12.f * flog2(best / 440.f)) * 256.f);
        }
    }
    { static const float hz[5] = {1320, 1760, 2093, 2637, 3136}; for (int i = 0; i < 5; ++i) chatter_inc[i] = (uint32_t)(hz[i] * 97391.548f); }
    for (int i = 0; i < 64; ++i) { float tau = .02f * fexp2((float)i * .1375035f); dk_tab[i] = kq(tau); }   /* 1.1^i */
    static const uint8_t moods[SN_MOODS][8] = {
        {7, 0, 2, 4, 6, 7, 9, 11}, {5, 0, 2, 4, 7, 9}, {7, 0, 2, 3, 5, 7, 9, 10},
        {7, 0, 2, 3, 5, 7, 8, 10}, {5, 0, 1, 5, 7, 10}, {6, 0, 2, 4, 6, 8, 10}};
    for (int m = 0; m < SN_MOODS; ++m) for (int i = 0; i < 8; ++i) scale_t[m][i] = moods[m][i];
    /* the hall: about 12 seconds to fade */
    static const int lens[4] = {RL0 * 4, RL1 * 4, RL2 * 4, RL3 * 4};
    for (int i = 0; i < 4; ++i) rev_g[i] = (int32_t)(fexp2(-3.f * 3.3219281f * (float)lens[i] / (44100.f * 12.f)) * 32767.f);
    build_sounds();
    for (unsigned r = 0; r < SN_LANES; ++r) { rd[r].tp_pre = 256; rd[r].tp_post = 32767; rd[r].tp_a = 32767; rd[r].quiet = 400; }
}
SETUP void sonar_dsp_tempo(void)
{
    unsigned bpm = sn_song.bpm;
    tick_q8 = bpm ? (int32_t)(44100.f * 60.f / ((float)bpm * 8.f) * 256.f) : 0;
    if (tick_q8 > 0x7fffff00) tick_q8 = 0x7fffff00;
    float T = bpm ? 60.f / (float)bpm * .75f : .9f;
    if (T < .28f) T = .28f;
    if (T > .9f) T = .9f;
    echo_T = (int32_t)(T * 44100.f / ECHO_DEC * 256.f);
    for (unsigned r = 0; r < SN_LANES; ++r) sonar_dsp_ring(r);
}
static const float ring_vol[SN_LANES] = {.36f, .44f, .6f, .85f, .5f}, ring_gate[SN_LANES] = {3, 1.2f, 3.5f, 1, 1.5f}, auto_sp[SN_LANES] = {1, 1, .75f, .8f, .9f};
static uint32_t fade_blocks[SN_LANES];
SETUP void sonar_dsp_fade(unsigned r, unsigned b) { if (r < SN_LANES) fade_blocks[r] = b; }
SETUP void sonar_dsp_ring(unsigned r)
{
    if (r >= SN_LANES) return;
    const struct sn_ring *R = &sn_song.r[r];
    struct ringdsp *d = &rd[r];
    float lvl = R->p.lvl / 100.f, e = R->p.era / 100.f, a = R->p.tape / 100.f, tone = R->p.tone / 100.f;
    float g = R->mute ? 0.f : ring_vol[r] * lvl * 1.25f;
    if (r < SN_RINGS) g *= sn_song.loops_vol / 80.f;          /* LOOPS VOL: 80 is as designed */
    int32_t tgt = (int32_t)(g * 32767.f);
    uint32_t fb = fade_blocks[r] ? fade_blocks[r] : 10;
    fade_blocks[r] = 0;
    d->gain_step = (tgt - d->gain) / (int32_t)fb;
    if (!d->gain_step && tgt != d->gain) d->gain_step = tgt > d->gain ? 1 : -1;
    d->gain_tgt = tgt;
    d->send = (int32_t)((R->p.sp_on ? R->p.sp / 100.f : auto_sp[r]) * 32767.f);
    float fc = 300.f * fexp2(tone * 6.f);
    if (fc > 18000.f) fc = 18000.f;
    d->tone_a = (int32_t)((1.f - fexp(-6.2831853f * fc / 44100.f)) * 32767.f);
    float ec = fsin((1.f - e) * 1.5707963f), es = fsin(e * 1.5707963f);
    d->warm_g = (int32_t)(ec * .7f * 32767.f);
    d->warm_pre = (int32_t)((1.f + (1.f - e) * 1.4f) * 256.f);
    d->crush_g = es < .02f ? 0 : (int32_t)(es * .85f * 32767.f);
    int bits = (int)(9.f - e * 6.f + .5f);
    d->crush_l = 1 << (bits - 1); d->crush_sh = bits - 1;
    d->kit_crush = r == SN_PULSE && R->ch == 1;
    d->drop_p = a < .15f ? 0 : (int32_t)(a * 247.f);
    d->tp_pre = (int32_t)((1.f + a * 1.6f) * 256.f);
    d->tp_post = (int32_t)(32767.f / (1.f + a * .9f));
    float tfc = 20000.f - a * 14000.f;
    d->tp_a = a < .01f ? 32767 : (int32_t)((1.f - fexp(-6.2831853f * tfc / 44100.f)) * 32767.f);
    d->hiss = (int32_t)(a * .006f * 32767.f);
    float rate = 1.f + (float)r * .17f;
    d->winc = (uint16_t)(.55f * rate / KR * 65536.f); d->dinc = (uint16_t)(.13f * rate / KR * 65536.f); d->finc = (uint16_t)(6.3f * rate / KR * 65536.f);
    d->wow = (int32_t)(a * .30f * 256.f); d->wdrift = (int32_t)(a * .035f * 256.f); d->flut = (int32_t)(a * .082f * 256.f);
    d->detune = (int32_t)((cents_q16((1.f - e) * 8.f) - 65536));
    float md = R->p.mod / 100.f;
    d->vib_inc = md > .01f ? (int32_t)((4.f + md * 4.f) / KR * 65536.f) : 0;
    d->vib_dep = md > .01f ? (int32_t)(md * md * 90.f / 100.f * 256.f) : 0;
    d->chipw = e > .8f ? 2 : e > .55f ? 1 : 0;
    static const uint8_t divs[4] = {8, 4, 2, 1};
    d->div = divs[R->p.spd & 3];
    float sd = sn_song.bpm ? 60.f / (float)sn_song.bpm / 8.f : 60.f / 30.f / 8.f;
    float dm = fexp2((R->p.dec / 100.f - .5f) * 4.f);
    float dur = sd * (float)d->div * ring_gate[r] * dm;
    if (dur > 14.f) dur = 14.f;
    d->dur = blocks(dur);
    d->quiet = d->quiet > 0 ? d->quiet : 0;
}
SETUP void sonar_dsp_fx(void)
{
    unsigned m = sn_song.fx_mode & 3;
    float amt = sn_song.fx_amt / 100.f;
    int echo = m == SN_FX_ON || m == SN_FX_ECHO, rev = m == SN_FX_ON || m == SN_FX_VERB;
    fx_echo_t = echo ? (int32_t)(.7f * amt * 32767.f) : 0;
    fx_e2r_t = echo && rev ? 32767 / 2 : 0;
    fx_rev_t = rev ? (int32_t)(1.35f * amt * 32767.f * .55f) : 0;
    float vol = sn_song.vol / 100.f;
    master_vol = (int32_t)(vol * vol * 32767.f);
}

SETUP void sonar_dsp_loops_fade(int32_t target, unsigned blocks)
{
    if (target < 0) target = 0;
    if (target > 32767) target = 32767;
    if (!blocks) { loops_fade = loops_fade_tgt = target; loops_fade_step = 0; return; }
    int32_t step = (target - loops_fade) / (int32_t)blocks;
    if (!step) step = target > loops_fade ? 1 : -1;
    loops_fade_step = step; loops_fade_tgt = target;
}
SETUP int32_t sonar_dsp_loops_level(void) { return loops_fade; }

/* ------------------------------------------------------------ the keys' edits (control task) */
static const uint8_t wave_ids[SN_WAVES] = {0, W_SINE, W_TRI, W_SAW, W_SQR, W_P25, W_VOX};
SETUP void sonar_dsp_keys(void)
{
    const struct tpl *t = &tpls[SN_SOUNDS + sn_song.r[SN_KEYS].ch % SN_KEY_SOUNDS];
    keys_w = wave_ids[sn_song.keys_wave % SN_WAVES];
    keys_det = (int32_t)((fexp2((float)sn_song.keys_detune / 1200.f) - 1.f) * 65536.f);
    float am = fexp2(((float)sn_song.keys_atk - 50.f) / 25.f), rm = fexp2(((float)sn_song.keys_rel - 50.f) / 25.f);
    float a0 = t->rawA0 * am, a1 = t->rawA1 * am, R = t->rawR * rm;
    if (a0 < .002f) a0 = .002f;
    if (a1 < .002f) a1 = .002f;
    int32_t b0 = blocks(a0), b1 = blocks(a1);
    if (b0 > 4000) b0 = 4000;
    if (b1 > 4000) b1 = 4000;
    keys_ra0 = 65536 / b0; keys_ra1 = 65536 / b1; keys_A0 = b0; keys_A1 = b1;
    keys_kr = kq(R / 4.f); keys_rel = blocks(R * 1.6f + .05f);
}
SETUP static float shape_of(unsigned w, float ph)
{
    ph -= (float)(int)ph;
    if (w == W_SINE) return fsin(ph * 6.2831853f);
    if (w == W_SAW) return ph * 2.f - 1.f;
    if (w == W_SQR) return ph < .5f ? 1.f : -1.f;
    if (w == W_P25) return ph < .25f ? 1.5f : -.5f;
    if (w == W_P12) return ph < .125f ? 1.75f : -.25f;
    if (w == W_TRI) return ph < .5f ? ph * 4.f - 1.f : 3.f - ph * 4.f;
    if (w >= W_VOX && w <= W_PLUCK) return (float)wt[w - W_VOX][(int)(ph * 512.f) & 511] / 8192.f;
    return 0;
}
SETUP void sonar_dsp_keys_shape(int8_t *out, unsigned n)
{
    const struct tpl *t = &tpls[SN_SOUNDS + sn_song.r[SN_KEYS].ch % SN_KEY_SOUNDS];
    static float acc[256];
    if (n > 256) n = 256;
    float det = (float)keys_det / 65536.f, peak = .0001f;
    for (unsigned i = 0; i < n; ++i) {
        float ph = 2.f * (float)i / (float)n, s = 0;
        for (unsigned k = 0; k < t->n; ++k) {
            const struct tpl_osc *o = &t->o[k];
            if (o->route != R_DRY) continue;
            unsigned w = o->w;
            if (keys_w && !(t->fm && k == 0)) w = keys_w;
            float ratio = (float)o->ratio / 65536.f;
            if (k) ratio *= 1.f + ((k & 1U) ? det : -det);
            float lvl = (float)(o->lv0 + o->lv1) / 65534.f;
            if (t->fm && k == 0) {   /* the bell: show its modulated carrier */
                float mr = (float)t->mratio[4] / 65536.f, idx = (float)(t->fmi0 + t->fmi1) / 512.f * .5f;
                s += lvl * fsin(ph * 6.2831853f + idx * fsin(ph * mr * 6.2831853f));
            } else s += lvl * shape_of(w, ph * ratio);
        }
        acc[i] = s; if (s > peak) peak = s; if (-s > peak) peak = -s;
    }
    for (unsigned i = 0; i < n; ++i) out[i] = (int8_t)(acc[i] / peak * 120.f);
}
SETUP void sonar_dsp_keys_env(float *attack, float *sustain, float *release)
{
    const struct tpl *t = &tpls[SN_SOUNDS + sn_song.r[SN_KEYS].ch % SN_KEY_SOUNDS];
    float am = fexp2(((float)sn_song.keys_atk - 50.f) / 25.f), rm = fexp2(((float)sn_song.keys_rel - 50.f) / 25.f);
    *attack = (t->rawA0 + t->rawA1) * .5f * am; *sustain = (float)t->sus / 32767.f; *release = t->rawR * rm;
}

SETUP void sonar_dsp_budget(int32_t units) { work_budget = units < 16 ? 16 : units; }
