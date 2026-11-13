/* A coop synth engine as an Opus engine (opus_engine_add). Both play on the OS's six voices, 128 samples a block at
 * 88.2 kHz, with the same per-sample phase step (2^32 a cycle a sample); the OS does the envelope, the velocity and the
 * mix around each voice's render in both. So the calls pass through:
 *   coop init()                        opl_init, once (where coop's core ran it: the main thread at load), with the
 *                                      L1 lease open
 *   note_on(voice, state, vel, par)    note_on: velocity full scale (the OS applies the note's velocity after the
 *                                      render, as for every Opus engine)
 *   render(voice, state, out, pitch,   render; COOP_ENGINE_F_44K: the 64 samples at 44.1 kHz made 128 as coop's core
 *          params)                     does (each sample twice)
 *   note_off(voice, state)             release
 * The voice index (coop's `voice`) is kept in the Opus voice's state, set by init(voice, index); the coop state
 * (voice_bytes) follows it, 32-byte aligned within the voice.
 *
 * The declared controls (e4) on T1 as coop dispatches them (copa_ctl.c); the words go through opus_engine_knob_set.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (LICENSE). */
#include "opus_plugin.h"
#include "op1fw.h"
#include "copa.h"

#define HEAD 32                    /* the voice's index, then the coop state */

typedef struct {
    struct opus_engine e;          /* first: opus_called() is this */
    const copa_def *def;
} eng_rec;

#define ENG_MAX 8
static eng_rec recs[ENG_MAX];
static int nrecs;

static inline const eng_rec *me(void) { return (const eng_rec *)opus_called(); }
static inline const coop_engine_t *eng_of(const eng_rec *r) { return (const coop_engine_t *)r->def->desc; }
static inline int index_of(void *voice) { return *(volatile int32_t *)voice; }
static inline void *state_of(const coop_engine_t *e, void *voice) { return e->voice_bytes ? (uint8_t *)voice + HEAD : 0; }

static void e_init(void *voice, int index) { *(volatile int32_t *)voice = index < 0 ? 0 : index % COOP_VOICES; }

static void e_note_on(void *voice, const short *knobs)
{
    const coop_engine_t *e = eng_of(me());
    e->note_on(index_of(voice), state_of(e, voice), 32767, knobs);
}

static void e_release(void *voice, const short *knobs)
{
    (void)knobs;
    const coop_engine_t *e = eng_of(me());
    if (e->note_off)
        e->note_off(index_of(voice), state_of(e, voice));
}

static void e_render(void *voice, short *out, const short *par, const unsigned *pitch)
{
    const coop_engine_t *e = eng_of(me());
    e->render(index_of(voice), state_of(e, voice), out, (const int32_t *)pitch, par);
    if (e->flags & COOP_ENGINE_F_44K)
        for (int i = COOP_VOICE_BLOCK_44K - 1; i >= 0; --i)
            out[2 * i] = out[2 * i + 1] = out[i];
}

/* fn + turn, shift + fn + turn on T1; turn and shift + turn stay the OS's. */
static int e_turn(int encoder, int detents, int page)
{
    const copa_def *d = me()->def;
    if (page != 0 || encoder < 0 || encoder > 3 || !d->nctl || !opus_fn_held())
        return 0;
    const int shift = fw_shift_held() != 0;
    const copa_ctl *c = copa_find(d, shift ? COPA_SHIFT_FN_TURN : COPA_FN_TURN, encoder);
    if (!c || c->kind == 'l')
        return 1;
    short k[8];
    opus_engine_knobs(k);
    const int v = copa_turn_value(c, encoder, k[c->word], detents, shift && c->action == COPA_FN_TURN);
    if (v < 0)
        return 1;
    opus_engine_knob_set(c->word, v);
    copa_touched(d, c, -1, v);
    return 1;
}

static int e_key(int key, int pressed, int page)
{
    const copa_def *d = me()->def;
    const int enc = key - 0x3B;
    if (enc < 0 || enc > 3 || !d->nctl || (pressed && page != 0))
        return 0;
    int action;
    const int taken = copa_click(enc, pressed, &action);
    if (!action || page != 0)
        return taken;
    const copa_ctl *c = copa_find(d, action, enc);
    if (!c || c->kind != 's')
        return 1;
    short k[8];
    opus_engine_knobs(k);
    const int v = copa_click_value(c, k[c->word]);
    opus_engine_knob_set(c->word, v);
    copa_touched(d, c, -1, v);
    return 1;
}

static void e_screen_init(void *scr)
{
    const coop_screen_t *s = eng_of(me())->screen;
    if (s && s->init)
        s->init(scr);
}

static void e_screen_draw(void *scr, int dt)
{
    const eng_rec *r = me();
    const coop_screen_t *s = eng_of(r)->screen;
    copa_shown(r->def, -1);
    if (s && s->update)
        s->update(scr, dt);
}

int copa_engine_add(const copa_def *d, int index)
{
    if (nrecs == ENG_MAX)
        return -1;
    const coop_engine_t *ce = (const coop_engine_t *)d->desc;
    if (ce->magic != COOP_ENGINE_MAGIC || ce->api != COOP_PLUGIN_API || (ce->flags & ~COOP_ENGINE_FLAGS) ||
        ce->voice_bytes > COOP_VOICE_BYTES_MAX || !ce->note_on || !ce->render)
        return -1;
    if (ce->init) {
        copa_l1_open = 1;
        const int failed = ce->init();
        copa_l1_open = 0;
        if (failed)
            return -5;
    }
    eng_rec *r = &recs[nrecs];
    r->def = d;
    struct opus_engine *e = &r->e;
    e->size = sizeof *e;
    e->name = d->name;
    for (int k = 0; k < 4; ++k)
        e->knobs[k] = d->defaults[k];
    for (int k = 5; k < 8; ++k)
        e->knobs[k] = (short)copa_ctl_default(d, k);
    e->voice = HEAD + ((ce->voice_bytes + 31) & ~31u);
    e->init = e_init;
    e->note_on = e_note_on;
    e->release = e_release;
    e->render = e_render;
    e->turn = e_turn;
    e->key = e_key;
    e->type = d->name;             /* a sound made under coop names its engine so: it loads as this one */
    e->mark = 0;
    for (int k = 0; k < 4; ++k)
        e->shift_knobs[k] = -1;
    if (ce->screen) {
        e->svg = copa_svg_path(d, index);
        if (e->svg) {
            e->screen_init = e_screen_init;
            e->screen_draw = e_screen_draw;
        }
    }
    const int mark = opus_engine_add(copa_self, e);
    if (mark < 0)
        return mark;
    ++nrecs;
    return 0;
}
