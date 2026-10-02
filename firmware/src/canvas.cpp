#include "canvas.h"

#include <stdlib.h>
#include <string.h>

// Shapes and text follow Adafruit GFX's algorithms exactly -- same circle
// helper, same Bresenham, same glyph walk and the same bounds rule -- so a
// layout tuned on the bench with GxEPD2 lands on the same pixels here.

void Canvas::clear(Ink c) {
  const uint8_t v = (uint8_t)c;
  memset(px, v | v << 2 | v << 4 | v << 6, sizeof(px));
}

void Canvas::pixel(int x, int y, Ink c) {
  if (x < 0 || y < 0 || x >= CANVAS_W || y >= CANVAS_H) return;
  const int i = y * CANVAS_W + x;
  const int sh = (i & 3) * 2;
  px[i >> 2] = (uint8_t)((px[i >> 2] & ~(3 << sh)) | ((uint8_t)c << sh));
}

Ink Canvas::get(int x, int y) const {
  if (x < 0 || y < 0 || x >= CANVAS_W || y >= CANVAS_H) return Ink::White;
  const int i = y * CANVAS_W + x;
  return (Ink)((px[i >> 2] >> ((i & 3) * 2)) & 3);
}

void Canvas::hline(int x, int y, int w, Ink c) {
  for (int i = 0; i < w; i++) pixel(x + i, y, c);
}

void Canvas::vline(int x, int y, int h, Ink c) {
  for (int i = 0; i < h; i++) pixel(x, y + i, c);
}

void Canvas::fill_rect(int x, int y, int w, int h, Ink c) {
  for (int j = 0; j < h; j++) hline(x, y + j, w, c);
}

void Canvas::rect(int x, int y, int w, int h, Ink c) {
  hline(x, y, w, c);
  hline(x, y + h - 1, w, c);
  vline(x, y, h, c);
  vline(x + w - 1, y, h, c);
}

void Canvas::line(int x0, int y0, int x1, int y1, Ink c) {
  const bool steep = abs(y1 - y0) > abs(x1 - x0);
  int t;
  if (steep) {
    t = x0; x0 = y0; y0 = t;
    t = x1; x1 = y1; y1 = t;
  }
  if (x0 > x1) {
    t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
  }
  const int dx = x1 - x0, dy = abs(y1 - y0);
  int err = dx / 2;
  const int ystep = y0 < y1 ? 1 : -1;
  for (; x0 <= x1; x0++) {
    if (steep) pixel(y0, x0, c);
    else pixel(x0, y0, c);
    err -= dy;
    if (err < 0) {
      y0 += ystep;
      err += dx;
    }
  }
}

void Canvas::fill_circle(int x0, int y0, int r, Ink c) {
  vline(x0, y0 - r, 2 * r + 1, c);
  int f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r, px_ = x, py = y;
  const int delta = 1;
  while (x < y) {
    if (f >= 0) {
      y--;
      ddy += 2;
      f += ddy;
    }
    x++;
    ddx += 2;
    f += ddx;
    if (x < y + 1) {
      vline(x0 + x, y0 - y, 2 * y + delta, c);
      vline(x0 - x, y0 - y, 2 * y + delta, c);
    }
    if (y != py) {
      vline(x0 + py, y0 - px_, 2 * px_ + delta, c);
      vline(x0 - py, y0 - px_, 2 * px_ + delta, c);
      py = y;
    }
    px_ = x;
  }
}

namespace {

const GFXglyph *glyph(const GFXfont *f, char ch) {
  const uint8_t c = (uint8_t)ch;
  if (c < f->first || c > f->last) return nullptr;
  return &f->glyph[c - f->first];
}

}  // namespace

int Canvas::text(int x, int y, const char *s, const GFXfont *f, Ink c) {
  for (; *s; s++) {
    const GFXglyph *g = glyph(f, *s);
    if (!g) continue;     // not in this subset: Adafruit skips it too
    const uint8_t *bm = f->bitmap + g->bitmapOffset;
    uint8_t bits = 0, bit = 0;
    for (int yy = 0; yy < g->height; yy++) {
      for (int xx = 0; xx < g->width; xx++) {
        if (!(bit++ & 7)) bits = *bm++;
        if (bits & 0x80) pixel(x + g->xOffset + xx, y + g->yOffset + yy, c);
        bits <<= 1;
      }
    }
    x += g->xAdvance;
  }
  return x;
}

int Canvas::text_width(const char *s, const GFXfont *f) {
  // Adafruit's charBounds(): every glyph widens the box by its inked
  // extent, including zero-width ones, and w is max - min + 1.
  int x = 0, minx = 0x7FFF, maxx = -1;
  for (; *s; s++) {
    const GFXglyph *g = glyph(f, *s);
    if (!g) continue;
    const int x1 = x + g->xOffset, x2 = x1 + g->width - 1;
    if (x1 < minx) minx = x1;
    if (x2 > maxx) maxx = x2;
    x += g->xAdvance;
  }
  return maxx >= minx ? maxx - minx + 1 : 0;
}

uint32_t Canvas::hash(void) const {
  uint32_t h = 2166136261u;     // FNV-1a
  for (size_t i = 0; i < sizeof(px); i++) {
    h ^= px[i];
    h *= 16777619u;
  }
  return h;
}
