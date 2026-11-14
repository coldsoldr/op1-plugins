/* The engines' key filter (adapter/copa_input.c) beside a hosted effect's key (adapter/copa_fx.c h_key), on a PC.
 *
 * Opus runs every key filter before the hosted effect's own key. The filter used to act on every encoder release, so
 * on an effect page it consumed the press the hosted key had recorded and took the release: no effect click ever
 * acted on the OP-1 (2.2.0, petri and fickle). Here: an effect page's click passes the filter and acts in the hosted
 * path; an engine's click still acts in the filter; a release whose press nobody took passes on.
 *
 *   make -C tests
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (LICENSE). */
#include <stdio.h>
#include <stdlib.h>
#include "opus_plugin.h"
#include "op1fw.h"
#include "copa.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++fails; } } while (0)

/* ---- one engine with a click on white (word 7, steps) ---- */
static const char *const subs[] = { "off", "-1", "-2" };
static const copa_ctl eng_ctls[] = {
    { .name = "sub", .steps = subs, .def = 0, .action = COPA_CLICK, .enc = 2, .word = 7, .kind = 's', .count = 3,
      .detents = 1, .wrap = 1 },
};
static const copa_def eng = { .kind = COPA_ENGINE, .name = "eng", .ctl = eng_ctls, .nctl = 1 };

/* ---- the core's and the firmware's names, stubbed ---- */
static int g_shift, g_fn, g_engine_page, g_knob7;
static int (*g_filter)(int key, int pressed);
int fw_shift_held(void) { return g_shift; }
int opus_fn_held(void) { return g_fn; }
void opus_page_fill(int x0, int y0, int x1, int y1, int colour) { (void)x0; (void)y0; (void)x1; (void)y1; (void)colour; }
void opus_page_text(struct opl_self *self, int slot, int x, int y, int size, int colour, const char *text)
{
    (void)self; (void)slot; (void)x; (void)y; (void)size; (void)colour; (void)text;
}
int opus_page_now(struct opus_page *pg)
{
    static short par[8];
    par[7] = (short)g_knob7;
    pg->par = par;
    return 1;
}
const copa_def *copa_engine_page(const struct opus_page *pg) { (void)pg; return g_engine_page ? &eng : 0; }
int opus_engine_knobs(short par[8]) { par[7] = (short)g_knob7; return 1; }
int opus_engine_knob_set(int k, int value) { if (k == 7) g_knob7 = value; return 1; }
void opus_on_knob(struct opl_self *self, int (*fn)(int index, short *delta)) { (void)self; (void)fn; }
void opus_on_key_filter(struct opl_self *self, int (*fn)(int key, int pressed)) { (void)self; g_filter = fn; }
struct opl_self *copa_self;
uint32_t copa_test_cycles;

/* What the core does with a key: the filters first, then (if none took it) the hosted effect's key on its page. The
 * hosted key here is copa_fx.c h_key's click model: copa_click, then act on the action it returns. */
static int g_effect_action;
static void key_event(int key, int pressed, int effect_page)
{
    if (g_filter(key, pressed))
        return;
    if (!effect_page)
        return;
    int action;
    copa_click(key - 0x3B, pressed, &action);
    if (action)
        g_effect_action = action;
}

int main(void)
{
    copa_input_start();
    CHECK(g_filter != 0);

    /* an effect page: the orange click (freeze) reaches the hosted key, press and release */
    g_engine_page = 0;
    g_effect_action = 0;
    key_event(0x3E, 1, 1);
    key_event(0x3E, 0, 1);
    CHECK(g_effect_action == COPA_CLICK);

    /* every encoder, several times over */
    for (int n = 0; n < 3; ++n)
        for (int k = 0x3B; k <= 0x3E; ++k) {
            g_effect_action = 0;
            key_event(k, 1, 1);
            key_event(k, 0, 1);
            CHECK(g_effect_action == COPA_CLICK);
        }

    /* fn + click on an effect page: the fn click */
    g_fn = 1;
    g_effect_action = 0;
    key_event(0x3C, 1, 1);
    key_event(0x3C, 0, 1);
    CHECK(g_effect_action == COPA_FN_CLICK);
    g_fn = 0;

    /* an engine page: its declared white click acts in the filter, and the hosted path never sees it */
    g_engine_page = 1;
    g_knob7 = 0;
    g_effect_action = 0;
    key_event(0x3D, 1, 1);
    key_event(0x3D, 0, 1);
    CHECK(g_knob7 == 1 && g_effect_action == 0);

    /* an undeclared push on the engine page: passes on (the OS's), press and release */
    CHECK(g_filter(0x3B, 1) == 0);
    CHECK(g_filter(0x3B, 0) == 0);

    /* a release without any press: passes on */
    CHECK(g_filter(0x3E, 0) == 0);

    if (fails) {
        printf("input_test: %d failed\n", fails);
        return 1;
    }
    printf("input_test: all passed\n");
    return 0;
}
