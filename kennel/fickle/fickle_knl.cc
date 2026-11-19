/* fickle as a kennel effect (cokn.h). The coop plugin (plugins/fickle) unchanged; kennel's four presses:
 *   blue press   page 2: blue picks the wet mode and the routing together, one word, 9 steps a click each (cycling):
 *                reverb in, reverb both, reverb loop, delay in, delay both, delay loop, slip in, slip both, slip loop.
 *                The default is reverb both (coop's defaults).
 *   green press  page 2: green picks the loop mode (13 steps, cycling: tape 1 .. str rev).
 *   white press  latch: loop (off listening, on playing).
 *   orange press latch: freeze; with shift held, dub instead.
 * The turns are coop's: blue time, green modify, white clock, orange mix.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "cokn.h"

extern "C" const coop_fx_t FX_fickle;

namespace {

const int32_t kWetRoutes = 9;   /* 3 wet modes x 3 routings */
const int32_t kLoopModes = 13;

void Words(const int16_t *kw, int16_t *cw) {
  const int32_t wr = cokn::StepOf(kw[0], kWetRoutes);
  cw[0] = static_cast<int16_t>(wr / 3);                 /* params[5]: the wet mode */
  cw[1] = static_cast<int16_t>(wr % 3);                 /* params[6]: the routing */
  cw[2] = static_cast<int16_t>(cokn::StepOf(kw[1], kLoopModes));   /* params[7]: the loop mode (green's word 6) */
}

const cokn::Latch kLatches[2] = {
  { 2, 1, -1 },                                         /* white: loop (bit 1) */
  { 3, 0, 2 },                                          /* orange: freeze (bit 0), shift: dub (bit 2) */
};

const cokn::Glue glue = { &FX_fickle, Words, kLatches, 2, 0u, 0x3u };

}  // namespace

COKN_EFFECT(fickle, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_LATCH, OP1_PRESS_LATCH)
