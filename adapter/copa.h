/* copa: the adapter between coop plugins (coop-sdk/coop_plugin.h, coldsoldr/co-op firmware/sdk) and Opus 2's core
 * (plugin-kit/sdk/opus_plugin.h). A plugin's sources stay coop sources, byte for byte; tools/build.py compiles them,
 * generates copa_defs[] from their plugin.json and links them with this adapter into one .opmod.
 *
 * What the coop core gave a plugin, and where it comes from here (docs/porting.md has the whole table):
 *   an effect in the filter slot, its mark in word 4     opus_effect_host (core 2.6): the same carriage
 *   render(slot, int32 l, int32 r, params[16], n)         the hosted render, int16 widened to Q31 and back as coop does
 *   the e6 controls (plugin.json "controls")              copa_ctl.c: the same layers, steps, fine steps and popup
 *   the e8 position buffers (coop_fx_scratch)             three buffers per file, from the audio pool at the start
 *   the e9 effect screens (coop_fx_view_t)                the hosted effect's screen, the view from opus_page_now
 *   the engines on iter's voice loop                      opus_engine_add: 128 samples at 88.2 kHz, the same pitch
 *   the screen calls (coop_screen.h)                      copa_screen.c: the same R246 functions coop calls
 *   coop_l1_lease / coop_l1_scratch                       opus_fast, from opl_init (where coop ran an engine's init)
 *   coop_tempo_bpm_q16                                    opus_tempo (core 2.21)
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (LICENSE). */
#ifndef COPA_H
#define COPA_H
#include <stdint.h>
#include "coop_plugin.h"

enum { COPA_FX, COPA_ENGINE };

/* coop's control actions (co-op firmware/core/sdram/engine_ui.cc): 0 turn and 2 shift + turn stay the OS's. */
enum { COPA_CLICK = 1, COPA_SHIFT_CLICK = 3, COPA_FN_TURN = 4, COPA_FN_CLICK = 5, COPA_SHIFT_FN_TURN = 6,
       COPA_SHIFT_FN_CLICK = 7 };

/* One declared control, from plugin.json "controls" (generated). */
typedef struct copa_ctl {
    const char *name;              /* shown in the popup, lower case */
    const char *const *steps;      /* kind 's': the step names */
    int16_t def;                   /* the default: a step index or Q15 */
    int16_t step;                  /* continuous: Q15 a detent (0: 328, 1 %) */
    int16_t fine_step;             /* continuous with fine: Q15 a detent on shift + fn (0: step / 16, remainder kept) */
    uint8_t action;                /* COPA_* above */
    uint8_t enc;                   /* 0 blue, 1 green, 2 white, 3 orange */
    uint8_t word;                  /* 5..7; a latch: its bit of params[8] */
    uint8_t kind;                  /* 's' steps, 'c' continuous, 'l' latch */
    uint8_t count;                 /* steps: how many */
    uint8_t detents;               /* steps on a turn: detents a step */
    uint8_t wrap;                  /* steps on a turn: past the last to the first */
    uint8_t fine;                  /* continuous fn + turn: shift + fn + turn moves it finely */
} copa_ctl;

/* One coop plugin in this file (generated from its plugin.json by tools/build.py). */
typedef struct copa_def {
    int kind;                      /* COPA_FX or COPA_ENGINE */
    const char *name;              /* in the OS's lists and in sounds: the coop name with spaces for '_' */
    const void *desc;              /* its coop_fx_t or coop_engine_t */
    const char *svg_file;          /* its screen's file name in the plugin's folder, or 0 */
    const char *svg;               /* the SVG's text */
    unsigned svg_len;
    int16_t defaults[4];           /* the four turns, Q15 */
    uint32_t scratch;              /* an effect's position buffer, bytes (plugin.json "scratch"), or 0 */
    const copa_ctl *ctl;
    int nctl;
} copa_def;
extern const copa_def copa_defs[];
extern const int copa_def_count;

struct opl_self;
extern struct opl_self *copa_self;

/* ---- copa_ctl.c: the controls (KeyHandler thread) and their popup (display thread) ---- */
int copa_layer(void);                                          /* bit 0 shift, bit 1 fn: coop_scr_mods */
const copa_ctl *copa_find(const copa_def *d, int action, int enc);
int copa_turn_value(const copa_ctl *c, int enc, int cur, int detents, int fine);   /* -1: no move */
int copa_click_value(const copa_ctl *c, int cur);              /* a click on a steps control: the next step, wrapping */
int copa_word(const copa_def *d, int word, int v);             /* params[5..7] as the coop core clamps them */
int copa_ctl_default(const copa_def *d, int word);
void copa_touched(const copa_def *d, const copa_ctl *c, int pos, int value);   /* the popup: pos -1 an engine */
void copa_shown(const copa_def *d, int pos);                   /* from a screen's draw: this page shows */
void copa_frame(unsigned short *buf);                          /* opus_on_frame: the popup over the page */
/* The click model of coop (engine_ui.cc): a press is remembered with its layer; the release acts only when the layer
 * is the same. -> 1 taken; *action the click's action on a release that acts, else 0. */
int copa_click(int enc, int pressed, int *action);

/* ---- copa_fx.c, copa_engine.c ---- */
int copa_fx_add(const copa_def *d, int index);
int copa_engine_add(const copa_def *d, int index);
int copa_fx_buffers(uint32_t bytes);                           /* the three position buffers: 0, or -1 no room */

/* ---- copa_host.c ---- */
const char *copa_svg_path(const copa_def *d, int index);       /* the SVG on the flash, written when needed */
extern int copa_l1_open;                                       /* 1 while an engine's coop init runs */
int copa_l1_scratch_make(void);
#ifdef __bfin__
static inline uint32_t copa_now(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }
#else                              /* the host tests (tests/) */
extern uint32_t copa_test_cycles;
static inline uint32_t copa_now(void) { return copa_test_cycles; }
#endif
#endif
