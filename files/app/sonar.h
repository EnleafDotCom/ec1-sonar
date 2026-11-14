/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * ec-1 Sonar for the M-VAVE FM-1.
 *
 * A port of the ec-1 Sonar Synth web app. Four rings loop around a disc at
 * their own lengths and speeds:
 *   haze   chords   (pads: angel, halo, bloom, choir ...)
 *   glint  melody   (shimmer, bell, theremin, signal ...)
 *   drone  bass     (braam, analog, halo ...)
 *   pulse  textures (boom, sonar ping, telemetry, airlock, hull, beam)
 * Every ring has its own little tape machine (tone, era: warm tape or
 * crushed bits, wow and flutter), and they all feed the space echo, a
 * three-head tape delay, and an eleven-second dark hall.
 *
 * The sound is synthesized by the app inside the audio interrupt
 * (sonar_dsp.c). The composer, worlds, drift, mutate, undo and the panel run
 * in the control task (sonar.c, sonar_ui.inc). */
#ifndef FM1_SONAR_H
#define FM1_SONAR_H
#include <stdint.h>
#include "fm1.h"

enum { SN_HAZE, SN_GLINT, SN_DRONE, SN_PULSE, SN_RINGS };
/* the keyboard's own instrument: a fifth lane with its own sound and tape, never sequenced */
enum { SN_KEYS = SN_RINGS, SN_LANES };
enum { SN_STEPS = 16, SN_MOODS = 6, SN_SOUNDS = 14, SN_KEY_SOUNDS = 8, SN_KITS = 3, SN_PIECES = 6, SN_SLOTS = 8 };
enum { SN_KIND_CHORD, SN_KIND_MEL, SN_KIND_DRUM };
/* sounds (ec-1's order) */
enum { S_MOOG, S_GLASS, S_NES, S_ATARI, S_VOX, S_PLUCK, S_BLOOM, S_LASER, S_SHIMMER, S_THEREMIN, S_BRAAM, S_SIGNAL, S_ANGEL, S_HALO };
enum { SN_FX_ON, SN_FX_ECHO, SN_FX_VERB, SN_FX_DRY };

/* A dot on a ring: scale degree (or pulse piece) and how wild it plays (0..255). */
struct sn_dot { uint8_t on, d, y, skip; };
/* Everything in a ring's tune sheet. Amounts are 0..100. */
struct sn_prm {
    int8_t oct;            /* -2..2 */
    uint8_t mod, dec, wild, lvl, tone, era, tape, sp, sp_on;
    uint8_t spd;           /* 0 1/4x, 1 1/2x, 2 1x, 3 2x */
};
struct sn_ring {
    uint8_t ch, len, mute, pad;   /* ch: sound (kit on pulse, pad bank on keys); pad: the keys' chord (0 single, 1 open, 2 triad, 3 wide) */
    struct sn_prm p;
    struct sn_dot st[SN_STEPS];
};
struct sn_song {
    char name[24];
    uint8_t mood, bpm, mut, fx_mode, fx_amt, vol, keys_lock, keys_latch;   /* fx_amt 0..150, mut/vol 0..100; keys snap to the scale, keys hold */
    uint8_t loops_vol;            /* the four tracks together, 0..100 */
    uint8_t keys_wave, keys_detune, keys_atk, keys_rel;   /* the keys' wave (0 = the pad's own), detune cents, attack and release (50 = as designed) */
    uint8_t pad2[2];
    struct sn_ring r[SN_LANES];   /* four rings, then the keys */
};

/* Shared with the audio interrupt. */
struct sn_runtime {
    volatile uint8_t playing, stop_req, restart;
    volatile int32_t gtick;                 /* 32nd ticks since play */
    volatile int8_t pos[SN_RINGS];          /* step each ring is on, -1 before the first */
    volatile uint8_t flash[SN_LANES];       /* counts down after a note, for the screen */
    volatile uint8_t last_d[SN_LANES];
    volatile uint8_t wrap[SN_RINGS];        /* a ring came round: mutate it (control task) */
    volatile uint8_t drift_due;             /* 64 ticks passed while drifting */
};

extern struct sn_song sn_song;
extern struct sn_runtime sn_rt;

/* audio interrupt */
FM1_AUDIO_CODE void sonar_render(int32_t *output, unsigned frames);

/* control task: the engine */
void sonar_dsp_init(void);
void sonar_dsp_tempo(void);       /* bpm, ring speeds, echo time */
void sonar_dsp_ring(unsigned r);  /* a ring's tune sheet changed */
void sonar_dsp_fx(void);          /* space mode, amount, volume */
void sonar_dsp_fade(unsigned r, unsigned blocks); /* the next ring level change glides */
void sonar_dsp_live(unsigned ring, unsigned d, unsigned y, unsigned key, unsigned down);
void sonar_dsp_audition(unsigned ring, unsigned d, unsigned y);
void sonar_dsp_loops_fade(int32_t target_q15, unsigned blocks);   /* 0 blocks: at once */
int32_t sonar_dsp_loops_level(void);
void sonar_dsp_budget(int32_t units);   /* how much voice work a block may take */
enum { SN_WAVES = 7 };                 /* PAD (its own), SINE, TRIANGLE, SAW, SQUARE, PULSE, CHOIR */
void sonar_dsp_keys(void);             /* the keys' wave, detune, attack and release changed */
void sonar_dsp_keys_shape(int8_t *out, unsigned n);   /* two cycles of the keys' wave, for the screen */
void sonar_dsp_keys_env(float *attack, float *sustain, float *release);  /* seconds, level 0..1, seconds */

/* control task: the composer */
void sonar_init(uint32_t seed);
void sonar_poll(uint32_t now);
void sonar_play(int on);
void sonar_shuffle(void);
void sonar_drift_toggle(void);
int sonar_undo(void);
void sonar_snap(int ring);        /* -1: every ring and the song */
void sonar_wipe(void);
void sonar_clear_ring(unsigned r);
void sonar_seed_ring(unsigned r);
void sonar_mute(unsigned r);
void sonar_remix(unsigned r);
void sonar_keys_chord(int delta);
const char *sonar_keys_wave_name(void);
void sonar_keys_edit(unsigned what, int delta);   /* 0 wave, 1 detune, 2 attack, 3 release */
int sonar_keys_note(unsigned key);                    /* the MIDI note a key plays on KEYS */
unsigned sonar_key_degree(unsigned r, unsigned key);  /* the ring degree a key writes with REC */
const char *sonar_midi_name(int m);
const char *sonar_keys_chord_name(void);   /* a new sound and era, the same rhythm */
void sonar_mood(int delta);
void sonar_tempo(int delta);
void sonar_mutate_amt(int delta);
void sonar_sound(unsigned r, int delta);
void sonar_param(unsigned r, unsigned which, int delta);
void sonar_fx_mode(int delta);
void sonar_fx_amt(int delta);
void sonar_volume(int delta);
void sonar_loops_volume(int delta);
void sonar_key(unsigned r, unsigned d, unsigned key, unsigned down);   /* a white key */
void sonar_step_toggle(unsigned r, unsigned step);
void sonar_step_edit(unsigned r, unsigned step, int dd, int dy);
void sonar_rotate(unsigned r, int delta);
int sonar_store_save(unsigned slot);
int sonar_store_load(unsigned slot);
const char *sonar_store_name(unsigned slot);

enum { SP_OCT, SP_MOD, SP_DEC, SP_WILD, SP_LVL, SP_TONE, SP_ERA, SP_TAPE, SP_SP, SP_SPD, SP_LEN };
unsigned sonar_kind(unsigned r);
unsigned sonar_max_degree(unsigned r);
const char *sonar_ring_name(unsigned r);
const char *sonar_sound_name(unsigned r);
const char *sonar_mood_name(void);
const char *sonar_fx_name(void);
const char *sonar_note_name(unsigned r, unsigned d);
extern uint32_t sn_diag_late, sn_diag_budget;
extern uint8_t sn_drifting, sn_catching, sn_slot, sn_brush_y, sn_first_boot;
void sonar_fade(int in);          /* PLAY held: fade the loops in from silence, or out and stop */
extern const char *sn_notice;
extern uint32_t sn_revision;
#endif
