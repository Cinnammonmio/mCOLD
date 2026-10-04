// A drawing surface the size of the panel, in the panel's landscape view.
//
// Three inks, not two, even on the mono panel in hand today: the design
// uses an accent (red on the four-ink panel the board was built for) to
// say "do not trust this number" or "this is an alarm". The layout draws
// in accent everywhere the design does, and the panel driver decides
// what accent becomes -- black on mono, red on four-ink. So swapping the
// panel changes the driver, not every screen.
//
// The API is a small subset of Adafruit GFX's, with the same meanings,
// so the bench layout (bench/epd29-s3) ports line for line.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfxfont.h"

enum class Ink : uint8_t { White = 0, Black = 1, Accent = 2 };

static const int CANVAS_W = 250;
static const int CANVAS_H = 122;

struct Canvas {
  // Two bits a pixel, row-major in landscape: 250 x 122 = 7,625 bytes.
  uint8_t px[(CANVAS_W * CANVAS_H + 3) / 4];

  // Every drawing call is shifted by this: a layout drawn for a smaller
  // area lands inside the part of the panel the case leaves visible.
  // get() and the panel driver see physical pixels, unshifted.
  int ox = 0, oy = 0;

  void clear(Ink c = Ink::White);
  void pixel(int x, int y, Ink c);
  Ink get(int x, int y) const;

  void hline(int x, int y, int w, Ink c);
  void vline(int x, int y, int h, Ink c);
  void fill_rect(int x, int y, int w, int h, Ink c);
  void rect(int x, int y, int w, int h, Ink c);
  void line(int x0, int y0, int x1, int y1, Ink c);
  void fill_circle(int cx, int cy, int r, Ink c);

  // Text on a baseline, as Adafruit GFX's print() with a custom font.
  // Returns the x just past the last glyph's advance.
  int text(int x, int y, const char *s, const GFXfont *f, Ink c);

  // Width of the inked area of `s` -- what Adafruit's getTextBounds()
  // reports as w, which is what the bench layout centres and aligns on.
  static int text_width(const char *s, const GFXfont *f);

  // A cheap fingerprint of the picture, to tell whether a refresh would
  // change anything.
  uint32_t hash(void) const;
};
