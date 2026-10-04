// The screen templates of docs/display-design.md, ported from
// display-mock/render.py -- which is the layout's source of truth: every
// coordinate, size and spacing here is the one there. Change the layout
// there first, look at the PNGs, then carry the change over.
//
// Text is measured by advance width, as render.py's textlength() does,
// not by inked width. (The bench port used inked width, which gives a
// space no width at all and set "NO ACTIVE TRIP" as two words.)
#pragma once

#include <stdbool.h>

#include "canvas.h"

// Everything the footer shows. `mem` is percent of storage USED, so both
// readings run toward their own bad news.
struct Foot {
  bool trip, wifi, cloud, gnss, shock, charging;
  int batt;      // %, or -1 when the fuel gauge has nothing
  int mem;
};

struct Row {
  const char *label, *value;
};

// '|' in a data string draws the middle dot, '~' the degree ring: the
// fonts are ASCII, so both are drawn, not set.

// A -- Monitor: the temperature, centred and largest. `red`: not to be
// trusted (out of band, or no reading); `alarm`: red frame as well.
// With no min/max (`lo` null), `note` sits where they would.
void scr_monitor(Canvas &c, const char *device, const char *clock,
                 const Foot &f, const char *temp, const char *lo,
                 const char *hi, const char *note, bool red, bool alarm);

// A6 -- Charging: state of charge instead of the temperature.
void scr_charge(Canvas &c, const char *device, const char *clock,
                const Foot &f, int soc, const char *state, const Row *rows,
                int nrows);

// B -- Takeover: one headline, one sentence, one data line.
void scr_takeover(Canvas &c, const char *device, const char *clock,
                  const Foot &f, const char *title, const char *sub,
                  const char *data, bool alarm, bool red_title);

// C -- Detail: label left, value right, up to three rows.
void scr_detail(Canvas &c, const char *device, const char *clock,
                const Foot &f, const char *title, const char *state,
                const Row *rows, int nrows);

// The part of the panel the case leaves visible, in pixels hidden on
// each side (config epd_inset_*). Every template draws inside it.
void scr_set_insets(int top, int bottom, int left, int right);

// Six nested frames 3 px apart, for measuring those insets by eye.
void scr_calibrate(Canvas &c);

// The fourteen design states with the mockup's sample data, for checking
// the panel against display-mock/out/. 0..13 = A1..A6, B1..B6, C1, C2.
void scr_demo(Canvas &c, int page);
const char *scr_demo_name(int page);
static const int SCR_DEMO_PAGES = 14;
