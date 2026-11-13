/* The declared controls of coop plugins, as the coop core dispatches them (co-op firmware/core/sdram/engine_ui.cc, e4
 * to e6): on the plugin's own page, click, shift + click, fn + turn, fn + click, shift + fn + turn and shift + fn +
 * click (fn: Opus's FN key); turn and shift + turn stay the OS's. A click acts on its release, and only if shift and fn
 * are as they were at the press; an undeclared layer on the page does nothing and is taken; a steps control moves one
 * step at a time (a click wraps); a continuous one moves its step a detent, or with "fine" 1/16 of it on shift + fn,
 * the remainder kept. Every change shows the control's name and value for a moment, top left in the encoder's colour.
 *
 * Threads: turns and clicks on the KeyHandler thread; the popup on the display thread (a sequence word, then fields).
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (LICENSE). */
#include "opus_plugin.h"
#include "op1fw.h"
#include "copa.h"

#define COARSE_Q15   328                                       /* 1 % of 32767 a detent */
#define FINE_DIV     16
#define POPUP_CYCLES 480000000u                                /* ~1.2 s at 400 MHz, as coop */
#define SHOWN_CYCLES 40000000u                                 /* the page drew within ~0.1 s: it is the one shown */

int copa_layer(void) { return (fw_shift_held() ? 1 : 0) | (opus_fn_held() ? 2 : 0); }

const copa_ctl *copa_find(const copa_def *d, int action, int enc)
{
    for (int k = 0; k < d->nctl; ++k)
        if (d->ctl[k].action == action && d->ctl[k].enc == enc)
            return &d->ctl[k];
    if (action == COPA_SHIFT_FN_TURN)                           /* the fine of a "fine" fn + turn */
        for (int k = 0; k < d->nctl; ++k)
            if (d->ctl[k].action == COPA_FN_TURN && d->ctl[k].enc == enc && d->ctl[k].fine)
                return &d->ctl[k];
    return 0;
}

static int clamp_step(const copa_ctl *c, int v) { return v < 0 ? 0 : v >= c->count ? c->count - 1 : v; }

static int s_acc[4], s_fine[4];

int copa_turn_value(const copa_ctl *c, int enc, int cur, int detents, int fine)
{
    enc &= 3;
    if (c->kind == 's') {
        const int per = c->detents ? c->detents : 1;
        s_acc[enc] += detents;
        int moved = 0;
        while (s_acc[enc] >= per) { s_acc[enc] -= per; ++moved; }
        while (s_acc[enc] <= -per) { s_acc[enc] += per; --moved; }
        if (!moved)
            return -1;
        if (c->wrap) {
            int v = (clamp_step(c, cur) + moved) % c->count;
            return v < 0 ? v + c->count : v;
        }
        return clamp_step(c, clamp_step(c, cur) + moved);
    }
    if (c->kind != 'c')
        return -1;
    if (cur < 0)
        cur = 0;
    else if (cur > 32767)
        cur = 32767;
    const int step = c->step ? c->step : COARSE_Q15;
    int d;
    if (fine && c->fine_step)
        d = detents * c->fine_step;
    else if (fine) {
        s_fine[enc] += detents * step;
        d = s_fine[enc] / FINE_DIV;
        s_fine[enc] -= d * FINE_DIV;
    } else
        d = detents * step;
    const int v = cur + d;
    return v < 0 ? 0 : v > 32767 ? 32767 : v;
}

int copa_click_value(const copa_ctl *c, int cur)
{
    const int v = clamp_step(c, cur) + 1;
    return v >= c->count ? 0 : v;
}

int copa_word(const copa_def *d, int word, int v)
{
    for (int k = 0; k < d->nctl; ++k) {
        const copa_ctl *c = &d->ctl[k];
        if (c->kind == 'l' || c->word != word)
            continue;
        if (c->kind == 's')
            return clamp_step(c, v);
        return v < 0 ? 0 : v;                                   /* a short: at most 32767 already */
    }
    return 0;
}

int copa_ctl_default(const copa_def *d, int word)
{
    for (int k = 0; k < d->nctl; ++k)
        if (d->ctl[k].kind != 'l' && d->ctl[k].word == word)
            return d->ctl[k].def;
    return 0;
}

static uint8_t s_press[4];                                      /* 0 none, else 1 + the layer at the press */

int copa_click(int enc, int pressed, int *action)
{
    static const uint8_t actions[4] = { COPA_CLICK, COPA_SHIFT_CLICK, COPA_FN_CLICK, COPA_SHIFT_FN_CLICK };
    const int layer = copa_layer();
    *action = 0;
    enc &= 3;
    if (pressed) {
        s_press[enc] = (uint8_t)(1 + layer);
        return 1;
    }
    if (!s_press[enc])
        return 0;
    const int was = s_press[enc] - 1;
    s_press[enc] = 0;
    if (was == layer)                                           /* else part of a combo: nothing */
        *action = actions[layer];
    return 1;
}

/* ---- the popup ---- */
static volatile uint32_t s_pop_at;                              /* 0 none; written last */
static const copa_def *volatile s_pop_def;
static const copa_ctl *volatile s_pop_ctl;
static volatile int s_pop_pos, s_pop_value;
static const copa_def *volatile s_shown_def;
static volatile int s_shown_pos;
static volatile uint32_t s_shown_at;

void copa_touched(const copa_def *d, const copa_ctl *c, int pos, int value)
{
    s_pop_at = 0;
    s_pop_def = d;
    s_pop_ctl = c;
    s_pop_pos = pos;
    s_pop_value = value;
    s_pop_at = copa_now() | 1u;
}

void copa_shown(const copa_def *d, int pos)
{
    s_shown_def = d;
    s_shown_pos = pos;
    s_shown_at = copa_now() | 1u;
}

static int put(char *s, int o, int max, const char *t)
{
    while (*t && o < max)
        s[o++] = *t++;
    return o;
}

void copa_frame(unsigned short *buf)
{
    (void)buf;
    const uint32_t at = s_pop_at, now = copa_now();
    if (!at)
        return;
    if (now - at > POPUP_CYCLES) {                              /* expired: cleared, so CYCLES wrapping can't bring it back */
        if (s_pop_at == at)
            s_pop_at = 0;
        return;
    }
    if (!s_shown_at || now - s_shown_at > SHOWN_CYCLES || s_shown_def != s_pop_def || s_shown_pos != s_pop_pos)
        return;
    const copa_ctl *c = s_pop_ctl;
    const int v = s_pop_value;
    char s[32];
    int o = put(s, 0, 10, c->name);
    o = put(s, o, 12, "  ");
    if (c->kind == 's')
        o = put(s, o, 30, c->steps[clamp_step(c, v)]);
    else if (c->kind == 'l')
        o = put(s, o, 30, v ? "on" : "off");
    else {
        const int pc = (v < 0 ? 0 : v) * 100 / 32767;
        if (pc >= 100)
            s[o++] = '1';
        if (pc >= 10)
            s[o++] = (char)('0' + (pc / 10) % 10);
        s[o++] = (char)('0' + pc % 10);
    }
    s[o] = 0;
    opus_page_fill(8, 3, 200, 21, OPUS_COLOR_BACK);             /* coop's black band under the text */
    opus_page_text(copa_self, 0, 12, 6, 12, OPUS_COLOR_E(c->enc), s);
}
