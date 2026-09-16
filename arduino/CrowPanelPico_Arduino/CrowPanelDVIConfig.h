// SPDX-License-Identifier: MIT
//
// TMDS/DVI pin configuration for the Elecrow CrowPanel Pico Display 4.3".
//
// This is defined here rather than reused from PicoDVI's bundled
// `picodvi_dvi_cfg` (in software/include/common_dvi_pin_configs.h) even
// though the two are numerically identical. That preset is documented as
// being for a *different* board - "the not-HDMI socket on Rev C PicoDVI
// boards" - and the match is coincidental (both happen to wire TMDS clock
// to GP8/9 and RGB to GP10-15). Depending on it would silently couple this
// project to an unrelated upstream board preset that PicoDVI is free to
// rename, retune, or remove in a future release. Defining the CrowPanel's
// own wiring here keeps that an explicit, local fact about this hardware.
#pragma once

#include <PicoDVI.h>

// Wiring (matches code.py's picodvi.Framebuffer pin assignment exactly):
//   clock +/- : GP9 / GP8
//   red   +/- : GP11 / GP10
//   green +/- : GP13 / GP12
//   blue  +/- : GP15 / GP14
static const struct dvi_serialiser_cfg crowpanel_dvi_cfg = {
    .pio = DVI_DEFAULT_PIO_INST,
    .sm_tmds = {0, 1, 2},
    .pins_tmds = {10, 12, 14},
    .pins_clk = 8,
    .invert_diffpairs = true,
    .prog_offs = 0, // Set by dvi_serialiser_init() at runtime; not a fixed pin/wiring fact
};
