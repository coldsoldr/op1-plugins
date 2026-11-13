/* A coop effect as a hosted effect (opus_effect_host, core 2.6): the OS's filter type carries it with its mark in word
 * 4, as the coop core carries it (plugin_host.cc), so the render gets the same words: the four turns in the filter
 * type's ranges, the mark, then the declared controls (e6) and the latch bits.
 *
 * The audio, as coop's host does it (plugin_host.cc coop_fx_render_mono / render_st): the int16 bus widened to Q31
 * (<< 16), the render in place, back with saturation (>> 16); at the synth and drum positions both channels start as
 * the instrument's mono block (Opus's instrument bus is mono) and the result is their mean. Before the first block at a
 * position after a change, the effect's init(pos), the position's latches cleared, as coop does.
 *
 * The position buffers (e8): one per position (0 synth, 1 drum, 2 master), as large as this file's largest
 * "scratch", shared by the file's effects as coop's core shares them (init resets the state it finds there).
 *
 * The controls' words 5-7 are the sound's own: a turn writes the page's params (Opus keeps them); a click writes the
 * OS's live words and, at the synth and drum, the sound's patch, the way coop's core does (engine_ui.cc fx_set, the
 * same R246 addresses). At the master a click's word is kept with the next turn's save.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (LICENSE). */
#include "opus_plugin.h"
#include "op1fw.h"
#include "copa.h"

#define BLOCK 64

/* The filter type's ranges: R246's range table row 5 (0x01DE5C50 + 5 x 0x30), else these (coop's fx_rng, kennel's). */
static const int16_t fx_rng[4][2] = { { 64, 14528 }, { 0, 24576 }, { 0, 22528 }, { 8192, 32767 } };

/* The OS's effect words (coop engine_ui.cc): synth by slot, drum by kit, the master; the patches and their setter. */
#define SYNTH_SLOT  (*(volatile int *)0xFF8016D4u)
#define DRUM_KIT    (*(volatile int *)0xFF8016D8u)
#define FX_W_S(i)   ((volatile int16_t *)(0xFF801444u + (uint32_t)(i) * 16))
#define FX_W_D(i)   ((volatile int16_t *)(0xFF8015C4u + (uint32_t)(i) * 16))
#define FX_W_M      ((volatile int16_t *)0xFF8016C4u)
#define PATCH_S(i)  ((void *)(0x01DDD7E8u + (uint32_t)(i) * 0x38))
#define PATCH_D(i)  ((void *)(0x01DDD9A8u + (uint32_t)(i) * 0x38))
#define SET_FX      ((void (*)(void *, const int16_t *))0x019FF664u)        /* Patch::SetFxParams */

typedef struct {
    struct opus_hosted_effect h;   /* first: opus_called() is this */
    const copa_def *def;
    int mark;
    volatile int failed;           /* its init refused: it passes the sound through */
} fx_rec;

#define FX_MAX 16                  /* a file's effects: the screen entries below are one per record */
static fx_rec recs[FX_MAX];
static int nrecs;

static const fx_rec *volatile cur[3];      /* the effect started at each position (coop's g_cur) */
static volatile uint16_t latch[3];         /* each position's latch bits (params[8]) */
static uint8_t *bufs[3];
static uint32_t buf_bytes;
static int32_t L[BLOCK], R[BLOCK];

static inline const fx_rec *me(void) { return (const fx_rec *)opus_called(); }

/* The record the core is calling: opus_called(), else the one whose mark is in word 4 (the OS carries every hosted
 * effect as the filter with its mark there), else 0. */
static const fx_rec *me_or(int mark)
{
    const fx_rec *r = me();
    if (r)
        return r;
    for (int i = 0; i < FX_MAX && recs[i].def; ++i)
        if (recs[i].mark && recs[i].mark == mark)
            return &recs[i];
    return 0;
}

static int page_mark(void)
{
    struct opus_page pg;
    return opus_page_now(&pg) && pg.kind == OPUS_PAGE_EFFECT ? pg.live[4] : 0;
}
static inline const coop_fx_t *fx_of(const fx_rec *r) { return (const coop_fx_t *)r->def->desc; }
static inline int16_t sat16(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : (int16_t)x; }

int copa_fx_buffers(uint32_t bytes)
{
    bytes = (bytes + 31) & ~31u;
    for (int p = 0; p < 3; ++p) {
        bufs[p] = opus_alloc_audio(copa_self, bytes);
        if (!bufs[p])
            bufs[p] = opus_alloc(copa_self, bytes);
        if (!bufs[p])
            return -1;
    }
    buf_bytes = bytes;
    return 0;
}

void *coop_fx_scratch(int slot, uint32_t *bytes)
{
    if (slot < 0 || slot > 2 || !bufs[slot]) {
        if (bytes)
            *bytes = 0;
        return 0;
    }
    if (bytes)
        *bytes = buf_bytes;
    return bufs[slot];
}

static void range(int k, int *lo, int *hi)
{
    const int16_t *rg = (const int16_t *)(0x01DE5C50u + 5 * 0x30 + k * 6);
    *lo = rg[0];
    *hi = rg[1];
    if (*hi <= *lo) {
        *lo = fx_rng[k][0];
        *hi = fx_rng[k][1];
    }
}

static void params(const fx_rec *r, int pos, const short *par, int16_t *p)
{
    for (int k = 0; k < 4; ++k) {
        int lo, hi;
        range(k, &lo, &hi);
        p[k] = (int16_t)(par[k] < lo ? lo : par[k] > hi ? hi : par[k]);
    }
    p[4] = par[4];
    for (int k = 5; k < 8; ++k)
        p[k] = (int16_t)(r->def->nctl ? copa_word(r->def, k, par[k]) : 0);
    p[8] = (int16_t)(r->def->nctl ? latch[pos] : 0);
    for (int k = 9; k < COOP_FX_PARAMS_MIN; ++k)
        p[k] = 0;
}

/* coop's first block at a position after a change: init, the latches cleared. 0: pass the sound through. */
static int started(const fx_rec *r, int pos)
{
    if (r->failed)
        return 0;
    if (cur[pos] == r)
        return 1;
    latch[pos] = 0;
    cur[pos] = 0;
    if (fx_of(r)->init(pos) != 0) {
        ((fx_rec *)r)->failed = 1;
        return 0;
    }
    cur[pos] = r;
    return 1;
}

static void h_init(int pos)
{
    if (pos >= 0 && pos < 3)
        cur[pos] = 0;              /* the OS made its effect again here: coop's init at the next block */
}

static void h_render(int pos, short *buf, const short *par)
{
    const fx_rec *r = me_or(par[4]);
    if (!r || pos < 0 || pos > 1 || !started(r, pos))
        return;
    int16_t p[COOP_FX_PARAMS_MIN];
    params(r, pos, par, p);
    for (int i = 0; i < BLOCK; ++i)
        L[i] = R[i] = (int32_t)buf[i] << 16;
    fx_of(r)->render(pos, L, R, p, BLOCK);
    for (int i = 0; i < BLOCK; ++i)
        buf[i] = (short)(((int32_t)sat16(L[i] >> 16) + sat16(R[i] >> 16)) >> 1);
}

static void h_render_stereo(int pos, short *l, short *rr, const short *par)
{
    const fx_rec *r = me_or(par[4]);
    if (!r || pos != 2 || !started(r, pos))
        return;
    int16_t p[COOP_FX_PARAMS_MIN];
    params(r, pos, par, p);
    for (int i = 0; i < BLOCK; ++i) {
        L[i] = (int32_t)l[i] << 16;
        R[i] = (int32_t)rr[i] << 16;
    }
    fx_of(r)->render(pos, L, R, p, BLOCK);
    for (int i = 0; i < BLOCK; ++i) {
        l[i] = sat16(L[i] >> 16);
        rr[i] = sat16(R[i] >> 16);
    }
}

/* fn + turn, shift + fn + turn on its page. Plain turns and shift + turns are the OS's (the four knobs). */
static int h_turn(int pos, short *par, int encoder, int detents)
{
    const fx_rec *r = me_or(par[4]);
    if (!r)
        return 0;
    const copa_def *d = r->def;
    if (encoder < 0 || encoder > 3 || !d->nctl || !opus_fn_held())
        return 0;
    const int shift = fw_shift_held() != 0;
    const copa_ctl *c = copa_find(d, shift ? COPA_SHIFT_FN_TURN : COPA_FN_TURN, encoder);
    if (!c || c->kind == 'l')
        return 1;                  /* an undeclared layer: taken, nothing happens */
    const int v = copa_turn_value(c, encoder, par[c->word], detents, shift && c->action == COPA_FN_TURN);
    if (v < 0)
        return 1;
    par[c->word] = (short)v;
    copa_touched(d, c, pos, v);
    return 1;
}

static void set_word(int pos, int word, int value)
{
    int16_t v[8];
    volatile int16_t *w;
    void *patch = 0;
    if (pos == 2)
        w = FX_W_M;
    else if (pos == 1) {
        const int kit = DRUM_KIT & 7;
        w = FX_W_D(kit);
        patch = PATCH_D(kit);
    } else {
        const int slot = SYNTH_SLOT & 7;
        w = FX_W_S(slot);
        patch = PATCH_S(slot);
    }
    for (int k = 0; k < 8; ++k)
        v[k] = w[k];
    v[word] = (int16_t)value;
    w[word] = (int16_t)value;
    if (patch)
        SET_FX(patch, v);
}

static int h_key(int pos, int key, int pressed)
{
    const fx_rec *r = me_or(page_mark());
    if (!r)
        return 0;
    const copa_def *d = r->def;
    const int enc = key - 0x3B;
    if (enc < 0 || enc > 3 || !d->nctl || pos < 0 || pos > 2)
        return 0;
    int action;
    const int taken = copa_click(enc, pressed, &action);
    if (!action)
        return taken;
    const copa_ctl *c = copa_find(d, action, enc);
    if (!c)
        return 1;
    if (c->kind == 'l') {
        latch[pos] = (uint16_t)(latch[pos] ^ (1u << c->word));
        copa_touched(d, c, pos, (latch[pos] >> c->word) & 1);
        return 1;
    }
    if (c->kind != 's')
        return 1;
    struct opus_page pg;
    if (!opus_page_now(&pg) || pg.kind != OPUS_PAGE_EFFECT || pg.where != pos || !pg.par)
        return 1;
    const int v = copa_click_value(c, pg.par[c->word]);
    set_word(pos, c->word, v);
    copa_touched(d, c, pos, v);
    return 1;
}

/* The frame hook (display thread): a frame of one of this file's effects' pages, counted for the settings rows. */
void copa_fx_frame(void)
{
    const int mark = page_mark();
    for (int i = 0; mark && i < FX_MAX && recs[i].def; ++i)
        if (recs[i].mark == mark)
            copa_screen_called(recs[i].def, COPA_SCR_PAGE);
}

/* ---- its screen (e9): the view coop's core fills, from what the page shows ----
 * opus_called() is 0 when the core runs a hosted effect's screen_init at the start (crash record 1, 2026-11-13: Opus 2
 * build 7 dev 146), so each record gets screen entries of its own that know their record (SCREEN_ENTRIES below). */
static void screen_init_at(int i, void *scr)
{
    if (recs[i].def)
        copa_screen_called(recs[i].def, COPA_SCR_INIT);
    const coop_fx_screen_t *s = recs[i].def ? fx_of(&recs[i])->screen : 0;
    if (s && s->init)
        s->init(scr);
}

static void screen_draw_at(int i, void *scr, int dt)
{
    if (!recs[i].def)
        return;
    const fx_rec *r = &recs[i];
    const coop_fx_t *f = fx_of(r);
    copa_screen_called(r->def, page_mark() == r->mark ? COPA_SCR_OWN : COPA_SCR_OTHER);
    coop_fx_view_t v;
    volatile uint32_t *z = (volatile uint32_t *)&v;
    for (unsigned i = 0; i < sizeof v / 4; ++i)
        z[i] = 0;
    struct opus_page pg;
    int pos = 0;
    short live[8] = { 0 };
    if (opus_page_now(&pg) && pg.kind == OPUS_PAGE_EFFECT && pg.where >= 0 && pg.where <= 2) {
        pos = pg.where;
        for (int k = 0; k < 8; ++k)
            live[k] = pg.live[k];
    }
    v.slot = pos;
    for (int k = 0; k < 4; ++k) {
        int lo, hi;
        range(k, &lo, &hi);
        const int32_t q = ((int32_t)live[k] - lo) * 32767 / (hi - lo);
        v.knobs[k] = (int16_t)(q < 0 ? 0 : q > 32767 ? 32767 : q);
    }
    for (int k = 0; k < 3; ++k)
        v.words[k] = (int16_t)(r->def->nctl ? copa_word(r->def, 5 + k, live[5 + k]) : 0);
    v.latches = latch[pos];
    v.mods = copa_layer();
    if (r->def->scratch && bufs[pos]) {
        v.state = bufs[pos];
        v.state_bytes = buf_bytes;
    }
    v.live = cur[pos] == r;
    copa_shown(r->def, pos);
    if (f->screen && f->screen->update)
        f->screen->update(scr, dt, &v);
}

#define E(i) static void si##i(void *scr) { screen_init_at(i, scr); }              static void sd##i(void *scr, int dt) { screen_draw_at(i, scr, dt); }
E(0) E(1) E(2) E(3) E(4) E(5) E(6) E(7) E(8) E(9) E(10) E(11) E(12) E(13) E(14) E(15)
#undef E
static void (*const screen_inits[FX_MAX])(void *) = { si0, si1, si2, si3, si4, si5, si6, si7, si8, si9, si10, si11,
                                                      si12, si13, si14, si15 };
static void (*const screen_draws[FX_MAX])(void *, int) = { sd0, sd1, sd2, sd3, sd4, sd5, sd6, sd7, sd8, sd9, sd10,
                                                           sd11, sd12, sd13, sd14, sd15 };

int copa_fx_add(const copa_def *d, int index)
{
    if (nrecs == FX_MAX)
        return -1;
    const coop_fx_t *f = (const coop_fx_t *)d->desc;
    if (f->magic != COOP_FX_MAGIC || f->api != COOP_PLUGIN_API || (f->flags & ~COOP_FX_FLAGS) || !f->init ||
        !f->render)
        return -1;
    if (d->scratch && (!bufs[0] || buf_bytes < d->scratch))
        return -2;
    fx_rec *r = &recs[nrecs];
    r->def = d;
    struct opus_hosted_effect *h = &r->h;
    h->size = sizeof *h;
    h->name = d->name;
    h->mark = 0;                   /* the core's own: coop's numbers are each OP-1's own; sounds find it by name */
    for (int k = 0; k < 4; ++k) {
        h->defaults[k] = (short)(fx_rng[k][0] + d->defaults[k] * (fx_rng[k][1] - fx_rng[k][0]) / 32767);
    }
    for (int k = 5; k < 8; ++k)
        h->defaults[k] = (short)copa_ctl_default(d, k);
    h->init = h_init;
    h->render = h_render;
    h->render_stereo = h_render_stereo;
    h->key = h_key;
    h->turn = h_turn;
    if ((f->flags & COOP_FX_F_SCREEN) && f->screen) {
        h->svg = copa_svg_path(d, index);
        if (h->svg) {
            h->screen_init = screen_inits[nrecs];
            h->screen_draw = screen_draws[nrecs];
        }
    }
    const int mark = opus_effect_host(copa_self, h);
    if (mark < 0) {
        r->def = 0;
        return mark;
    }
    r->mark = mark;
    copa_mark_of(d, mark);
    ++nrecs;
    return 0;
}
