/* cokn: the coop core's exports a coop effect calls (coop-sdk/coop_plugin.h, coop_screen.h), on kennel / firmware 246.
 * The screen calls are R246's own functions at the addresses coop's core and copa use (adapter/copa_screen.c); the
 * kennel SDK's op1_find_node warns on a missing id (which stops the OS once asserts are on), so the lookup is the
 * node search with warn 0, as copa's.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "cokn.h"
#include "coop_screen.h"

namespace cokn {
PosState g_pos[kPositions];
int32_t g_l[kBlock], g_r[kBlock];

void *Scratch(int pos, uint32_t *bytes) {
  if (pos < 0 || pos >= kPositions) return 0;
  const uintptr_t base = reinterpret_cast<uintptr_t>(op1_fx_scratch(pos));
  if (!base) return 0;
  const uintptr_t p = (base + 31) & ~static_cast<uintptr_t>(31);   /* coop promises 32-byte alignment */
  if (bytes) *bytes = OP1_FX_SCRATCH_BYTES - static_cast<uint32_t>(p - base);
  return reinterpret_cast<void *>(p);
}
}  // namespace cokn

#define SVG_FIND    ((void *(*)(void *, const char *, int))0x019EB8ACu)        /* Node.cpp find, warn flag */
#define NODE_XFORM  ((void (*)(void *, const int32_t *))0x019EAD58u)
#define LINE_RGBA   ((uint8_t *(*)(void *, const char *))0x01A3B854u)
#define FILL_RGBA   ((uint8_t *(*)(void *, const char *))0x01A3B96Cu)
#define SCREEN_DRAW ((void (*)(void *, int))0x01A3B4C8u)
#define RASTER      ((void *(*)(void))0x01A3B490u)
#define RASTER_LINE ((void (*)(void *, const int32_t *, const int32_t *, int32_t, const uint8_t *))0x01A24A4Cu)

extern "C" {

void *coop_fx_scratch(int slot, uint32_t *bytes) {
  void *p = cokn::Scratch(slot, bytes);
  if (!p && bytes) *bytes = 0;
  return p;
}

void *coop_scr_find(void *scr, const char *id) {
  void *d = scr ? *static_cast<void **>(scr) : 0;
  return d ? SVG_FIND(d, id, 0) : 0;
}

void coop_scr_transform(void *node, const int32_t *m9) {
  if (node) NODE_XFORM(node, m9);
}

void coop_scr_visible(void *node, int on) {
  if (node) static_cast<volatile uint8_t *>(node)[0x50] = static_cast<uint8_t>(on != 0);
}

uint8_t *coop_scr_line_rgba(void *scr, const char *id) { return coop_scr_find(scr, id) ? LINE_RGBA(scr, id) : 0; }
uint8_t *coop_scr_fill_rgba(void *scr, const char *id) { return coop_scr_find(scr, id) ? FILL_RGBA(scr, id) : 0; }

void coop_scr_draw(void *scr, int dt) { SCREEN_DRAW(scr, dt); }

void *coop_scr_raster(void) { return RASTER(); }

void coop_scr_line(void *raster, const int32_t *p0, const int32_t *p1, int32_t width_q10, const uint8_t *rgba) {
  if (raster) RASTER_LINE(raster, p0, p1, width_q10, rgba);
}

void coop_scr_knobs(int16_t *out4) { op1_fx_knobs_live(out4); }

int coop_scr_mods(void) { return cokn::ShiftHeld() ? 1 : 0; }

}  // extern "C"
