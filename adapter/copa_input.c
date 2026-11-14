/* The engines' declared controls: every turn (opus_on_knob) and every encoder push (opus_on_key_filter), before the OS,
 * Opus's core and the other plugins. On an OP-1 (2026-11-13, Opus 2 build 7 dev 146) an engine's own turn handler never
 * saw FN + turn: the turn reached the OS and moved knob 3 (rings' structure), and shift + FN + a push did nothing. These
 * hooks see them. They act only while one of this file's engines shows its T1 (opus_page_now), and only on FN's layers
 * and the declared clicks; everything else goes on as it came. The effects keep the hosted effect's own turn and key
 * (copa_fx.c): FN + turn reached them on the same OP-1.
 *
 * Turns: FN + turn and shift + FN + turn on the page are taken (an undeclared one does nothing, as under coop); the
 * turn's 1/1024 OS units are counted into detents (4 OS units a detent). Clicks: a push is taken only when its layer
 * declares a click on that encoder (so Opus's own clicks, opus.scales' on the synth's T1, stay theirs); its release
 * acts when shift and FN are as they were at the press (copa_click), and is taken with its press.
 *
 * KeyHandler thread. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (LICENSE). */
#include "opus_plugin.h"
#include "op1fw.h"
#include "copa.h"

#define KEY_ENC0 0x3B

/* The engine whose T1 shows, if it is one of ours with controls. */
typedef struct {
    const copa_def *d;
    int pos;                       /* -1: an engine (the popup's) */
    struct opus_page pg;
} target;

static int find_target(target *t)
{
    if (!opus_page_now(&t->pg))
        return 0;
    t->d = copa_engine_page(&t->pg);
    t->pos = -1;
    return t->d && t->d->nctl > 0;
}

/* The stored knob, as the user set it (pg.par, else the current sound's). */
static int word_of(const target *t, int word)
{
    if (t->pg.par)
        return t->pg.par[word];
    short k[8];
    return opus_engine_knobs(k) ? k[word] : t->pg.live[word];
}

static void write_word(const target *t, int word, int value)
{
    (void)t;
    opus_engine_knob_set(word, value);
}

static int s_units[4];             /* OS units not yet a detent, per encoder */

static int on_knob(int index, short *delta)
{
    if (!opus_fn_held())
        return 0;
    target t;
    if (!find_target(&t))
        return 0;
    const int enc = index & 3;
    const int shift = fw_shift_held() != 0;
    s_units[enc] += *delta >> 10;
    const int detents = s_units[enc] / 4;
    s_units[enc] -= detents * 4;
    const copa_ctl *c = copa_find(t.d, shift ? COPA_SHIFT_FN_TURN : COPA_FN_TURN, enc);
    if (!c || c->kind == 'l' || !detents)
        return 1;                  /* taken: an undeclared layer, or less than a detent so far */
    const int v = copa_turn_value(c, enc, word_of(&t, c->word), detents, shift && c->action == COPA_FN_TURN);
    if (v >= 0) {
        write_word(&t, c->word, v);
        copa_touched(t.d, c, t.pos, v);
    }
    return 1;
}

static const copa_def *s_press_def[4];
static int s_press_pos[4];

static int on_key(int key, int pressed)
{
    const int enc = key - KEY_ENC0;
    if (enc < 0 || enc > 3)
        return 0;
    int action;
    if (pressed) {
        static const uint8_t actions[4] = { COPA_CLICK, COPA_SHIFT_CLICK, COPA_FN_CLICK, COPA_SHIFT_FN_CLICK };
        target t;
        if (!find_target(&t) || !copa_find(t.d, actions[copa_layer()], enc))
            return 0;              /* not ours: the OS's, Opus's, another plugin's */
        s_press_def[enc] = t.d;
        s_press_pos[enc] = t.pos;
        return copa_click(enc, 1, &action);
    }
    if (!s_press_def[enc])
        return 0;                  /* a press this filter did not take (a hosted effect's, the OS's): its release too */
    const copa_def *const pressed_def = s_press_def[enc];
    s_press_def[enc] = 0;
    const int taken = copa_click(enc, 0, &action);
    if (!taken || !action)
        return taken;
    target t;
    if (!find_target(&t) || t.d != pressed_def || t.pos != s_press_pos[enc])
        return 1;                  /* the page changed under the click: nothing */
    const copa_ctl *c = copa_find(t.d, action, enc);
    if (!c)
        return 1;
    if (c->kind == 's') {
        const int v = copa_click_value(c, word_of(&t, c->word));
        write_word(&t, c->word, v);
        copa_touched(t.d, c, t.pos, v);
    }
    return 1;
}

void copa_input_start(void)
{
    opus_on_knob(copa_self, on_knob);
    opus_on_key_filter(copa_self, on_key);
}
