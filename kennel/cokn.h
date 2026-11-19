/* cokn: a coop effect (coop-sdk/coop_plugin.h, e6 controls, e8 scratch, e9 screen) as a hundekraft kennel effect
 * (te-op1-cfw third_party/kennel/synth_sdk/op1_plugin.h). Each .knl file is one coop effect plus its glue
 * (kennel/<id>/<id>_knl.cc), which says how kennel's controls reach the coop words, and this header, which does the rest.
 *
 * What differs, and what the glue decides:
 *   - Kennel has no fn layer. A knob press is either a page (words 5..7: blue 5, green 6, white and orange 7) or a latch
 *     (op1_fx_page[pos][knob], not saved). The glue maps the stored words 5..7 to the coop words (Words) and says
 *     which knobs are latches and which coop latch bit each press flips, with and without shift held (Latch).
 *   - Shift + a latch press: the latch flag flips as for any press; on that flip (seen once a render block) the plugin
 *     reads shift (the stock flag 0x01dd7f78) and flips the shift bit instead of the plain one. The plugin keeps its
 *     own latch bits, so the kennel flag is only an edge.
 *   - A page-2 word moves ~1536 a click (kennel's dispatcher, Test 52); steps are read in clicks (StepOf), as the
 *     te-op1-cfw kennel ports do (knl::ClickMode); shift + a continuous page word is a quarter-click fine step (Fine,
 *     knl::Fine's rule).
 *   - The audio: kennel's int16 blocks of 64 widened to coop's Q31 (<< 16) and back with saturation; at the synth and
 *     drum the mono block feeds both channels and the result is their mean (as coop's own core and copa do).
 *   - The screen: coop_fx_view_t from what kennel shows: the shown position, the knobs as the stock screens see them,
 *     the coop words, the plugin's latch bits, mods bit 0 shift and bit 1 "fn" while any page knob is on page 2.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef COKN_H
#define COKN_H

#include <stdint.h>

extern "C" {
#include "op1_plugin.h"
}
#include "coop_plugin.h"
#include "coop_screen.h"

namespace cokn {

const int32_t kPositions = 3;                         /* 0 synth, 1 drum, 2 master */
const int32_t kBlock = OP1_FX_BLOCK;                  /* 64 */
const int32_t kClick = 1536;                          /* a page-2 word per encoder click */
const uint32_t kShiftFlag = 0x01dd7f78;

inline bool ShiftHeld() { return *reinterpret_cast<const volatile uint8_t *>(kShiftFlag) != 0; }
inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

/* A stored page-2 word as one of n steps, a click each, cycling (knl::ClickMode). */
inline int32_t StepOf(int32_t word, int32_t n) { return ((Clamp15(word) + kClick / 2) / kClick) % n; }
/* The word that holds step k (page2_defaults). */
inline int32_t WordOf(int32_t k) { return k * kClick; }

/* A press: the knob (0 blue .. 3 orange) is a latch whose flip toggles coop latch bit `plain`, or `shifted` while
 * shift is held (-1: that flip does nothing). */
struct Latch {
  int8_t knob, plain, shifted;
};

/* How a plugin's kennel controls reach its coop words. */
struct Glue {
  const coop_fx_t *fx;
  void (*words)(const int16_t *kw, int16_t *cw);      /* kennel's stored words 5..7 -> coop's params[5..7] */
  const Latch *latches;
  int32_t nlatches;
  uint32_t fine_mask;                                 /* bit k: word 5 + k is continuous, shift + turn = fine */
  uint32_t page_mask;                                 /* bit k: knob k is a page knob (for mods' fn bit) */
};

struct PosState {
  uint8_t last[4];                                    /* op1_fx_page as last seen */
  int16_t fine_last[3];
  uint8_t fine_seen;
  uint8_t live;                                       /* the coop init ran here since the effect was made */
  volatile uint16_t bits;                             /* coop latch bits (params[8]) */
};

extern PosState g_pos[kPositions];
extern int32_t g_l[kBlock], g_r[kBlock];

inline int16_t Sat16(int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : static_cast<int16_t>(x)); }

/* kennel init(pos): the effect was made (again) at pos. */
inline void Init(const Glue &g, int pos) {
  if (pos < 0 || pos >= kPositions) return;
  PosState &s = g_pos[pos];
  s.live = 0;
  s.bits = 0;
  s.fine_seen = 0;
  for (int32_t k = 0; k < 4; ++k) s.last[k] = op1_fx_page[pos][k];
  if (g.fx->init(pos) == 0) s.live = 1;
}

/* Shift + turn on a continuous page word: a quarter click (knl::Fine). */
inline void Fine(const Glue &g, int pos) {
  if (!g.fine_mask) return;
  PosState &s = g_pos[pos];
  volatile int16_t *w = const_cast<volatile int16_t *>(op1_fx_knobs(pos));
  if (!s.fine_seen) {
    for (int32_t k = 0; k < 3; ++k) s.fine_last[k] = w[5 + k];
    s.fine_seen = 1;
    return;
  }
  const bool shift = ShiftHeld();
  for (int32_t k = 0; k < 3; ++k) {
    if (!((g.fine_mask >> k) & 1)) continue;
    int32_t v = w[5 + k];
    const int32_t prev = s.fine_last[k];
    if (v == prev) continue;
    const int32_t d = v - prev, mag = d < 0 ? -d : d;
    const int32_t clicks = (mag + kClick / 2) / kClick;
    if (shift && clicks <= 3) {
      const int32_t n = clicks < 1 ? 1 : clicks;
      v = Clamp15(prev + (d < 0 ? -n : n) * (kClick / 4));
      w[5 + k] = static_cast<int16_t>(v);
    }
    s.fine_last[k] = static_cast<int16_t>(v);
  }
}

/* The latch presses since the last block. */
inline void Presses(const Glue &g, int pos) {
  PosState &s = g_pos[pos];
  uint16_t bits = s.bits;
  for (int32_t i = 0; i < g.nlatches; ++i) {
    const Latch &l = g.latches[i];
    const uint8_t now = op1_fx_page[pos][l.knob];
    if (now == s.last[l.knob]) continue;
    s.last[l.knob] = now;
    const int32_t b = ShiftHeld() ? l.shifted : l.plain;
    if (b >= 0) bits = static_cast<uint16_t>(bits ^ (1u << b));
  }
  s.bits = bits;
}

inline void Params(const Glue &g, int pos, const int16_t *kp, int16_t *p) {
  for (int32_t k = 0; k < 5; ++k) p[k] = kp[k];       /* the knobs in the filter type's range units, the selector */
  g.words(kp + 5, p + 5);
  p[COOP_FX_LATCH] = static_cast<int16_t>(g_pos[pos].bits);
  for (int32_t k = COOP_FX_LATCH + 1; k < COOP_FX_PARAMS_MIN; ++k) p[k] = 0;
}

inline bool Begin(const Glue &g, int pos, const int16_t *kp, int16_t *p) {
  if (pos < 0 || pos >= kPositions || !g_pos[pos].live) return false;   /* not bound: the dry passes */
  Fine(g, pos);
  Presses(g, pos);
  Params(g, pos, kp, p);
  return true;
}

inline void Render(const Glue &g, int pos, int16_t *buf, const int16_t *kp) {
  int16_t p[COOP_FX_PARAMS_MIN];
  if (!Begin(g, pos, kp, p)) return;
  for (int32_t i = 0; i < kBlock; ++i) g_l[i] = g_r[i] = static_cast<int32_t>(buf[i]) << 16;
  g.fx->render(pos, g_l, g_r, p, kBlock);
  for (int32_t i = 0; i < kBlock; ++i) buf[i] = static_cast<int16_t>((Sat16(g_l[i] >> 16) + Sat16(g_r[i] >> 16)) >> 1);
}

inline void RenderStereo(const Glue &g, int pos, int16_t *l, int16_t *r, const int16_t *kp) {
  int16_t p[COOP_FX_PARAMS_MIN];
  if (!Begin(g, pos, kp, p)) return;
  for (int32_t i = 0; i < kBlock; ++i) {
    g_l[i] = static_cast<int32_t>(l[i]) << 16;
    g_r[i] = static_cast<int32_t>(r[i]) << 16;
  }
  g.fx->render(pos, g_l, g_r, p, kBlock);
  for (int32_t i = 0; i < kBlock; ++i) {
    l[i] = Sat16(g_l[i] >> 16);
    r[i] = Sat16(g_r[i] >> 16);
  }
}

/* ---- the screen (GUI thread) ---- */

void *Scratch(int pos, uint32_t *bytes);              /* cokn.cc: the position's buffer, 32-byte aligned */

inline void ScreenInit(const Glue &g, void *scr) {
  if (g.fx->screen && g.fx->screen->init) g.fx->screen->init(scr);
}

inline void ScreenUpdate(const Glue &g, void *scr, int dt) {
  coop_fx_view_t v;
  volatile uint32_t *z = reinterpret_cast<volatile uint32_t *>(&v);
  for (uint32_t i = 0; i < sizeof v / 4; ++i) z[i] = 0;
  int pos = op1_fx_screen_pos();
  if (pos < 0 || pos >= kPositions) pos = 0;
  v.slot = pos;
  op1_fx_knobs_live(v.knobs);
  const int16_t *kw = op1_fx_knobs(pos);
  int16_t w[3] = { kw[5], kw[6], kw[7] };
  int16_t cw[3];
  g.words(w, cw);
  for (int32_t k = 0; k < 3; ++k) v.words[k] = cw[k];
  v.latches = g_pos[pos].bits;
  int32_t mods = ShiftHeld() ? 1 : 0;
  for (int32_t k = 0; k < 4; ++k)
    if (((g.page_mask >> k) & 1) && op1_fx_page[pos][k]) mods |= 2;
  v.mods = mods;
  uint32_t bytes = 0;
  v.state = Scratch(pos, &bytes);
  v.state_bytes = v.state ? bytes : 0;
  v.live = g_pos[pos].live;
  if (g.fx->screen && g.fx->screen->update) g.fx->screen->update(scr, dt, &v);
  else coop_scr_draw(scr, dt);
}

}  // namespace cokn

/* The kennel descriptors for one coop effect: KNL_<id> (op1_fx_stereo_t) and KSCREEN_<id> (op1_screen_t), from a
 * cokn::Glue named `glue` and the four presses. */
#define COKN_EFFECT(id, p0, p1, p2, p3)                                                                             \
  namespace {                                                                                                     \
  void knl_init(int pos) { cokn::Init(glue, pos); }                                                               \
  void knl_render(int pos, int16_t *buf, const int16_t *p) { cokn::Render(glue, pos, buf, p); }                    \
  void knl_render_st(int pos, int16_t *l, int16_t *r, const int16_t *p) { cokn::RenderStereo(glue, pos, l, r, p); } \
  void knl_scr_init(void *scr) { cokn::ScreenInit(glue, scr); }                                                   \
  void knl_scr_update(void *scr, int dt) { cokn::ScreenUpdate(glue, scr, dt); }                                   \
  }                                                                                                               \
  extern "C" const op1_fx_stereo_t KNL_##id = { { knl_init, knl_render, { p0, p1, p2, p3 } }, knl_render_st };     \
  extern "C" const op1_screen_t KSCREEN_##id = { knl_scr_init, knl_scr_update };

#endif
