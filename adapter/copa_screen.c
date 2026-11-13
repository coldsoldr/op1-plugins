/* The coop screen calls (coop-sdk/coop_screen.h) on Opus. coop's core implements them with R246's own screen
 * functions (co-op firmware/core/sdram/engine_screen.cc); Opus runs the same firmware #246 and hands an engine's and a
 * hosted effect's screen the same kind of OS screen object (opus_plugin.h, struct opus_engine: "scr is an OS screen
 * object ... the OS's 0x01A3B4C8(scr, dt)"), so these are coop's calls at coop's addresses. The kennel SDK, which
 * te-op1-cfw's earlier Opus ports ran on, calls the same ones. Display thread only.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (LICENSE). */
#include <stdint.h>
#include "opus_plugin.h"
#include "op1fw.h"
#include "coop_screen.h"
#include "copa.h"

#define NODE_XFORM  ((void (*)(void *, const int32_t *))0x019EAD58u)          /* Node: set the matrix (Q10) */
#define LINE_RGBA   ((uint8_t *(*)(void *, const char *))0x01A3B854u)        /* Screen: an element's stroke colour */
#define FILL_RGBA   ((uint8_t *(*)(void *, const char *))0x01A3B96Cu)        /* Screen: its fill colour */
#define SCREEN_DRAW ((void (*)(void *, int))0x01A3B4C8u)                     /* Screen::draw (fw_screen_draw) */
#define RASTER_LINE ((void (*)(void *, const int32_t *, const int32_t *, int32_t, const uint8_t *))0x01A24A4Cu)

static void *document(void *scr) { return scr ? *(void **)scr : 0; }

/* fw_svg_node_find with warn 0: a missing id returns 0 instead of stopping the OP-1. */
void *coop_scr_find(void *scr, const char *id)
{
    void *d = document(scr);
    return d ? fw_svg_node_find(d, id, 0) : 0;
}

void coop_scr_transform(void *node, const int32_t *m9)
{
    if (node)
        NODE_XFORM(node, m9);
}

void coop_scr_visible(void *node, int on)
{
    if (node)
        ((volatile uint8_t *)node)[0x50] = (uint8_t)(on != 0);
}

/* The style getters assert on a missing id: asked only for an id the document has. */
uint8_t *coop_scr_line_rgba(void *scr, const char *id) { return coop_scr_find(scr, id) ? LINE_RGBA(scr, id) : 0; }
uint8_t *coop_scr_fill_rgba(void *scr, const char *id) { return coop_scr_find(scr, id) ? FILL_RGBA(scr, id) : 0; }

void coop_scr_draw(void *scr, int dt) { SCREEN_DRAW(scr, dt); }

void *coop_scr_raster(void) { return fw_screen_raster(); }

void coop_scr_line(void *raster, const int32_t *p0, const int32_t *p1, int32_t width_q10, const uint8_t *rgba)
{
    if (raster)
        RASTER_LINE(raster, p0, p1, width_q10, rgba);
}

/* The engine page's words: the current sound's knobs (with a note sounding, as its newest voice hears them, the LFO
 * in: opus_engine_knobs since core 2.8). */
void coop_scr_words(int16_t *out8)
{
    short k[8];
    opus_engine_knobs(k);
    for (int i = 0; i < 8; ++i)
        out8[i] = k[i];
}

void coop_scr_knobs(int16_t *out4)
{
    int16_t w[8];
    coop_scr_words(w);
    for (int i = 0; i < 4; ++i)
        out4[i] = w[i];
}

int coop_scr_mods(void) { return copa_layer(); }
