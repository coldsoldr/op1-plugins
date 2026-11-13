/* The adapter's control logic (adapter/copa_ctl.c) on a PC, against coop's rules (co-op firmware/core/sdram/
 * engine_ui.cc): steps with detents and wrap, the continuous step and its fine remainder, a declared fine step, the
 * click on release with the layer kept, and the coop core's clamp of words 5-7.
 *
 *   make -C tests        (or: gcc -I tests/host -I adapter -I coop-sdk tests/ctl_test.c adapter/copa_ctl.c)
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include "opus_plugin.h"
#include "op1fw.h"
#include "copa.h"

/* ---- the core's and the firmware's names copa_ctl.c calls, stubbed ---- */
static int g_shift, g_fn;
int fw_shift_held(void) { return g_shift; }
int opus_fn_held(void) { return g_fn; }
void opus_page_fill(int x0, int y0, int x1, int y1, int colour) { (void)x0; (void)y0; (void)x1; (void)y1; (void)colour; }
void opus_page_text(struct opl_self *self, int slot, int x, int y, int size, int colour, const char *text)
{
    (void)self; (void)slot; (void)x; (void)y; (void)size; (void)colour; (void)text;
}
struct opl_self *copa_self;
uint32_t copa_test_cycles;

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++fails; } } while (0)

static const char *const shapes[] = { "saw", "square", "triangle" };
static const char *const heads[] = { "3", "2+3", "1+2+3", "1+3" };
static const char *const sync[] = { "off", "1/16t", "1/16", "1/8t", "1/16.", "1/8", "1/4t", "1/8.", "1/4", "1/2t",
                                    "1/4.", "1/2" };
static const copa_ctl ctls[] = {
    { .name = "shape", .steps = shapes, .def = 0, .action = COPA_FN_TURN, .enc = 0, .word = 5, .kind = 's', .count = 3,
      .detents = 2, .wrap = 0 },
    { .name = "heads", .steps = heads, .def = 0, .action = COPA_CLICK, .enc = 2, .word = 7, .kind = 's', .count = 4,
      .detents = 1, .wrap = 1 },
    { .name = "early", .def = 16384, .action = COPA_FN_TURN, .enc = 1, .word = 6, .kind = 'c', .detents = 1,
      .fine = 1 },
    { .name = "gain", .def = 16384, .step = 1365, .fine_step = 273, .action = COPA_FN_TURN, .enc = 3, .word = 7,
      .kind = 'c', .detents = 1, .fine = 1 },
    { .name = "freeze", .action = COPA_CLICK, .enc = 3, .word = 0, .kind = 'l', .detents = 1 },
    { .name = "sync", .steps = sync, .action = COPA_SHIFT_FN_CLICK, .enc = 1, .word = 5, .kind = 's', .count = 12,
      .detents = 1, .wrap = 1 },
};
static const copa_def def = { .kind = COPA_FX, .name = "test", .ctl = ctls, .nctl = 6 };

int main(void)
{
    /* find: the action and the encoder; shift + fn + turn falls back to a "fine" fn + turn */
    CHECK(copa_find(&def, COPA_FN_TURN, 0) == &ctls[0]);
    CHECK(copa_find(&def, COPA_SHIFT_FN_TURN, 1) == &ctls[2]);
    CHECK(copa_find(&def, COPA_SHIFT_FN_TURN, 0) == 0);        /* shape has no fine */
    CHECK(copa_find(&def, COPA_FN_TURN, 2) == 0);

    /* steps on a turn: two detents a step, no wrap */
    CHECK(copa_turn_value(&ctls[0], 0, 0, 1, 0) == -1);
    CHECK(copa_turn_value(&ctls[0], 0, 0, 1, 0) == 1);
    CHECK(copa_turn_value(&ctls[0], 0, 1, 2, 0) == 2);
    CHECK(copa_turn_value(&ctls[0], 0, 2, 4, 0) == 2);           /* held at the last */
    CHECK(copa_turn_value(&ctls[0], 0, 2, -6, 0) == 0);
    CHECK(copa_turn_value(&ctls[0], 0, 99, 2, 0) == 2);          /* a stored value out of range is clamped first */

    /* continuous: 328 a detent; fine: 1/16 of it, the remainder kept (20 and 21 alternate) */
    CHECK(copa_turn_value(&ctls[2], 1, 16384, 1, 0) == 16384 + 328);
    CHECK(copa_turn_value(&ctls[2], 1, 32700, 1, 0) == 32767);
    CHECK(copa_turn_value(&ctls[2], 1, 100, -1, 0) == 0);
    int v = 1000;
    v = copa_turn_value(&ctls[2], 1, v, 1, 1);
    CHECK(v == 1020);
    v = copa_turn_value(&ctls[2], 1, v, 1, 1);
    CHECK(v == 1041);
    /* a declared step and fine step */
    CHECK(copa_turn_value(&ctls[3], 3, 16384, 1, 0) == 16384 + 1365);
    CHECK(copa_turn_value(&ctls[3], 3, 16384, -2, 1) == 16384 - 546);

    /* a click wraps */
    CHECK(copa_click_value(&ctls[1], 2) == 3);
    CHECK(copa_click_value(&ctls[1], 3) == 0);
    CHECK(copa_click_value(&ctls[1], 40) == 0);                 /* clamped to the last, then wrapped */

    /* clicks act on the release, with the layer of the press */
    int action;
    g_shift = g_fn = 0;
    CHECK(copa_click(2, 1, &action) == 1 && action == 0);
    CHECK(copa_click(2, 0, &action) == 1 && action == COPA_CLICK);
    CHECK(copa_click(2, 0, &action) == 0 && action == 0);       /* a release without its press: not ours */
    g_shift = g_fn = 1;
    CHECK(copa_click(1, 1, &action) == 1 && action == 0);
    CHECK(copa_click(1, 0, &action) == 1 && action == COPA_SHIFT_FN_CLICK);
    CHECK(copa_find(&def, action, 1) == &ctls[5]);
    g_shift = 0;
    CHECK(copa_click(1, 1, &action) == 1);
    g_shift = 1;                                                /* shift came in between: a combo, not a click */
    CHECK(copa_click(1, 0, &action) == 1 && action == 0);
    g_shift = g_fn = 0;
    CHECK(copa_layer() == 0);
    g_fn = 1;
    CHECK(copa_layer() == 2);
    g_fn = 0;

    /* words 5-7 as the coop core hands them to render */
    CHECK(copa_word(&def, 5, 7) == 2);                          /* shape: the last step */
    CHECK(copa_word(&def, 5, -3) == 0);
    CHECK(copa_word(&def, 6, -1) == 0);
    CHECK(copa_word(&def, 6, 20000) == 20000);
    CHECK(copa_ctl_default(&def, 6) == 16384);
    CHECK(copa_ctl_default(&def, 0) == 0);                      /* the latch's bit 0 is not word 0 */

    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("ctl_test: all passed\n");
    return 0;
}
