/* petri as a kennel effect (cokn.h). The coop plugin (plugins/petri) unchanged; kennel's four presses:
 *   blue press   page 2: blue picks the effect (11 steps a click each, cycling: mosaic .. warp).
 *   green press  page 2: green sets the filter (continuous; shift + turn a quarter click).
 *   white press  page 2: white sets the space (continuous; shift + turn a quarter click).
 *   orange press latch: hold; with shift held, reverse instead.
 * The turns are coop's: blue time, green activity, white repeats, orange mix.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "cokn.h"

extern "C" const coop_fx_t FX_petri;

namespace {

const int32_t kEffects = 11;

void Words(const int16_t *kw, int16_t *cw) {
  cw[0] = static_cast<int16_t>(cokn::StepOf(kw[0], kEffects));   /* params[5]: the effect */
  cw[1] = static_cast<int16_t>(cokn::Clamp15(kw[1]));            /* params[6]: filter, Q15 */
  cw[2] = static_cast<int16_t>(cokn::Clamp15(kw[2]));            /* params[7]: space, Q15 */
}

const cokn::Latch kLatches[1] = {
  { 3, 0, 1 },                                          /* orange: hold (bit 0), shift: reverse (bit 1) */
};

const cokn::Glue glue = { &FX_petri, Words, kLatches, 1, 0x6u, 0x7u };

}  // namespace

COKN_EFFECT(petri, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_PAGE, OP1_PRESS_LATCH)
