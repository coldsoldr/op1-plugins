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
    int mark;
} eng_rec;

#define ENG_MAX 8
static eng_rec recs[ENG_MAX];
static int nrecs;

static inline const eng_rec *me(void) { return (const eng_rec *)opus_called(); }
static inline const coop_engine_t *eng_of(const eng_rec *r) { return r ? (const coop_engine_t *)r->def->desc : 0; }

/* The record the core is calling: opus_called(), else the one whose mark is in knob 4, else 0. */
static const eng_rec *me_or(int mark)
{
    const eng_rec *r = me();
    if (r)
        return r;
    for (int i = 0; i < ENG_MAX && recs[i].def; ++i)
        if (recs[i].mark && recs[i].mark == mark)
            return &recs[i];
    return 0;
}

static inline int index_of(void *voice) { return *(volatile int32_t *)voice; }
static inline void *state_of(const coop_engine_t *e, void *voice) { return e->voice_bytes ? (uint8_t *)voice + HEAD : 0; }

static short last_words[2];        /* the last render's words 5 and 6 (the settings rows) */
static int have_words;

/* The def whose engine this is (the settings rows). */
static const copa_def *def_of(const coop_engine_t *e)
{
    for (int i = 0; i < ENG_MAX && recs[i].def; ++i)
        if (recs[i].def->desc == e)
            return recs[i].def;
    return 0;
}

/* ---- note on and release in the audio block, as coop's core called them ----
 * Opus calls note_on and release in the key thread (docs/api.md, Engines), which the audio interrupt can cut into; a
 * coop engine's note_on is written for the audio thread: rings' swaps and copies voice states the render is playing
 * and uses the L1 scratch the render uses. On an Opus 2 OP-1 (2026-11-13) a note struck while rings' voices still rang
 * stopped the plugin. So the calls are queued here (the knobs copied) and made at the next render, in the order they
 * came, before that render: the audio interrupt then never cuts into one. The key thread clears a voice's flag, writes
 * its request, then sets the flag; the interrupt only acts on a set flag, so it never reads half a request. */
#define P_ON  1u
#define P_OFF 2u
static volatile uint8_t pend[COOP_VOICES];
static volatile uint32_t pend_seq[COOP_VOICES];
static uint32_t seq;
static const coop_engine_t *volatile pend_eng[COOP_VOICES];
static void *volatile pend_voice[COOP_VOICES];
static short pend_knobs[COOP_VOICES][8];

static void e_init(void *voice, int index)
{
    const int i = index < 0 ? 0 : index % COOP_VOICES;
    *(volatile int32_t *)voice = i;
    pend[i] = 0;                   /* the voice was built again: silent, nothing owed */
    copa_screen_called(recs[0].def, COPA_E_INIT);
}

static void queue(void *voice, const short *knobs, unsigned what)
{
    const coop_engine_t *e = eng_of(me_or(knobs[4]));
    if (!e)
        return;
    const int i = index_of(voice);
    const unsigned had = pend[i];
    pend[i] = 0;
    pend_eng[i] = e;
    pend_voice[i] = voice;
    if (what == P_ON)
        for (int k = 0; k < 8; ++k)
            pend_knobs[i][k] = knobs[k];
    pend_seq[i] = ++seq;
    if (what == P_ON)
        copa_screen_called(def_of(e), COPA_E_QUEUED);
    /* a release after a note not yet made: both, in order; a note after anything: the note alone */
    pend[i] = (uint8_t)(what == P_OFF ? (had & P_ON) | P_OFF : P_ON);
}

static void e_note_on(void *voice, const short *knobs) { queue(voice, knobs, P_ON); }

static void e_release(void *voice, const short *knobs) { queue(voice, knobs, P_OFF); }

/* The audio block: the queued calls, oldest first, at most NOTES_A_BLOCK note ons a block (the rest wait for the next
 * block, in order). A note on can be dear: on an Opus 2 OP-1 (2026-11-13) rings' six note ons of a chord, each
 * rebuilding a voice cut by a model change, and the strikes after them took 630,000 cycles in one block (the block is
 * 580,525) and rings was switched off; two a block still reached 586,000 there. One a block spreads a six-note chord
 * over six blocks, 8.7 ms. A block is told as rings tells it: a voice index no higher than the last render's. */
#define NOTES_A_BLOCK 1
static int notes_left;
static uint8_t fresh[COOP_VOICES];         /* a note on made, its first render not yet (the readout) */

static void flush(void)
{
    for (;;) {
        int i = -1;
        for (int k = 0; k < COOP_VOICES; ++k)
            if (pend[k] && (i < 0 || (int32_t)(pend_seq[k] - pend_seq[i]) < 0))
                i = k;
        if (i < 0)
            return;
        if ((pend[i] & P_ON) && notes_left <= 0)
            return;                /* the next block's: the older first, so nothing passes it */
        const unsigned what = pend[i];
        pend[i] = 0;
        const coop_engine_t *e = pend_eng[i];
        void *voice = pend_voice[i];
        if (what & P_ON) {
            --notes_left;
            fresh[i] = 1;
            copa_screen_called(def_of(e), COPA_E_MADE);
            if (have_words && (pend_knobs[i][5] != last_words[0] || pend_knobs[i][6] != last_words[1]))
                copa_screen_called(def_of(e), COPA_E_DIFF);
            const uint32_t t0 = copa_now();
            e->note_on(i, state_of(e, voice), 32767, pend_knobs[i]);
            copa_note_time(copa_now() - t0);
        }
        if ((what & P_OFF) && e->note_off)
            e->note_off(i, state_of(e, voice));
    }
}

/* The safety valve: once this file's engines have taken VALVE_CYCLES of a block, its other voices are silent for that
 * block (a short gap) instead of the block running late: the block is 580,525 cycles, and the OS, the effects and the
 * other plugins need theirs. On an Opus 2 OP-1 (2026-11-13) rings alone reached 573,000 after a model change and was
 * switched off. A block is told as rings tells it: a voice index no higher than the last render's. */
#define VALVE_CYCLES 400000u
static int last_index = 99;
static uint32_t block_used;

static void e_render_timed(void *voice, short *out, const short *par, const unsigned *pitch);

static void e_render(void *voice, short *out, const short *par, const unsigned *pitch)
{
    const int index = index_of(voice);
    if (index <= last_index) {
        block_used = 0;
        notes_left = NOTES_A_BLOCK;
    }
    last_index = index;
    if (block_used > VALVE_CYCLES) {
        for (int i = 0; i < COOP_VOICE_BLOCK; ++i)
            out[i] = 0;
        copa_valve();
        return;
    }
    const int first = fresh[index];
    const uint32_t t0 = copa_now();
    e_render_timed(voice, out, par, pitch);
    const uint32_t took = copa_now() - t0;
    block_used += took;
    if (first)
        fresh[index] = 0;
    copa_render_time(index, took, first || fresh[index]);
}

static void e_render_timed(void *voice, short *out, const short *par, const unsigned *pitch)
{
    flush();
    const coop_engine_t *e = eng_of(me_or(par[4]));
    if (e) {                       /* the settings rows: do words 5 and 6 change between renders */
        if (have_words && (par[5] != last_words[0] || par[6] != last_words[1]))
            copa_screen_called(def_of(e), COPA_E_FLIP);
        last_words[0] = par[5];
        last_words[1] = par[6];
        have_words = 1;
    }
    if (!e) {                      /* not ours to know: silence, never a fault */
        for (int i = 0; i < COOP_VOICE_BLOCK; ++i)
            out[i] = 0;
        return;
    }
    e->render(index_of(voice), state_of(e, voice), out, (const int32_t *)pitch, par);
    if (e->flags & COOP_ENGINE_F_44K)
        for (int i = COOP_VOICE_BLOCK_44K - 1; i >= 0; --i)
            out[2 * i] = out[2 * i + 1] = out[i];
}

/* ---- for copa_input.c: is a page one of this file's engines' T1 ---- */
const copa_def *copa_engine_page(const struct opus_page *pg)
{
    if (pg->kind != OPUS_PAGE_ENGINE || pg->t != 1)
        return 0;
    const int mark = pg->par ? pg->par[4] : pg->live[4];
    for (int i = 0; i < ENG_MAX && recs[i].def; ++i)
        if (pg->module == &recs[i].e || (recs[i].mark && recs[i].mark == mark))
            return recs[i].def;
    return 0;
}

/* Each record's own screen entries: opus_called() is 0 in a screen's init at the start (copa_fx.c). */
static void screen_init_at(int i, void *scr)
{
    if (recs[i].def)
        copa_screen_called(recs[i].def, COPA_SCR_INIT);
    const coop_screen_t *s = recs[i].def ? eng_of(&recs[i])->screen : 0;
    if (s && s->init)
        s->init(scr);
}

static void screen_draw_at(int i, void *scr, int dt)
{
    if (!recs[i].def)
        return;
    const coop_screen_t *s = eng_of(&recs[i])->screen;
    copa_screen_called(recs[i].def, COPA_SCR_OWN);
    copa_shown(recs[i].def, -1);
    if (s && s->update)
        s->update(scr, dt);
}

#define E(i) static void si##i(void *scr) { screen_init_at(i, scr); }              static void sd##i(void *scr, int dt) { screen_draw_at(i, scr, dt); }
E(0) E(1) E(2) E(3) E(4) E(5) E(6) E(7)
#undef E
static void (*const screen_inits[ENG_MAX])(void *) = { si0, si1, si2, si3, si4, si5, si6, si7 };
static void (*const screen_draws[ENG_MAX])(void *, int) = { sd0, sd1, sd2, sd3, sd4, sd5, sd6, sd7 };

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
    e->turn = 0;                   /* the controls: copa_input.c (the engine's turn never sees FN's layers) */
    e->key = 0;
    e->type = d->name;             /* a sound made under coop names its engine so: it loads as this one */
    e->mark = 0;
    for (int k = 0; k < 4; ++k)
        e->shift_knobs[k] = -1;
    if (ce->screen) {
        e->svg = copa_svg_path(d, index);
        if (e->svg) {
            e->screen_init = screen_inits[nrecs];
            e->screen_draw = screen_draws[nrecs];
        }
    }
    const int mark = opus_engine_add(copa_self, e);
    if (mark < 0) {
        r->def = 0;
        return mark;
    }
    r->mark = mark;
    copa_mark_of(d, mark);
    ++nrecs;
    return 0;
}
