// What the e-paper shows, and when it changes (docs/display-design.md).
//
// The screen is chosen from the box's state, drawn off-screen, and sent
// to the panel only if it differs from what is already on the glass --
// and then only as often as the policy allows:
//
//   nothing changed                       no refresh
//   alarm raised, cleared or acknowledged at once
//   trip started or stopped               at once
//   anything else (temperature, clock)    at most once per DISPLAY_MIN_S
//
// A refresh costs a few seconds of panel current; an image costs nothing
// to keep, since the glass holds it unpowered. Which is also why every
// screen carries the time it was drawn (rule 5 of the design): a frame
// left behind by a dead battery must not pass for a live one.
//
// This module reads state and draws it. Nothing else depends on it.
#pragma once

#include <stdint.h>

#include "canvas.h"
#include "power.h"

void display_start(const char *sn);

// The visible area from config epd_inset_*; after a change, redraw.
void display_apply_insets(void);

// Latest power reading, from the power task.
void display_note_power(const PowerStatus &ps);

// Redraw now, whatever the policy says (console).
void display_refresh(void);

// Pause the live display, for showing the design's demo pages.
void display_hold(bool on);

// 1 or 3: the two landscape orientations (GxEPD2's rotation numbers).
void display_set_rotation(int r);
int display_rotation(void);

// Draw a canvas to the panel the same way the live display does.
bool display_show(const Canvas &c);

// The picture the box is left with when the battery is cut off: the
// panel keeps it with no power, so it says why the box is dark.
void display_battery_off(float cell_volts);

static const uint32_t DISPLAY_MIN_S = 300;   // one sample period
