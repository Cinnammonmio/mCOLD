// E-paper panel driver.
//
// Two panels fit the board. The one it was designed for is the four-ink
// 2.13" GDEY0213F51 (black, white, yellow, red; GxEPD2_213c_GDEY0213F51),
// first fitted to box 002 on 2026-10-15; bench boards carried a mono 2.13"
// SSD1680 module (GxEPD2_213_B74) before that. Their command sets have
// nothing in common, and -- the trap that cost bring-up an afternoon --
// their BUSY lines are of OPPOSITE polarity: the mono panel holds it high
// while busy, the four-ink one low. That is also how they are told apart:
// after a reset the four-ink panel's BUSY rests high, the mono one's low
// (config epd_panel 0); epd_panel 1 or 2 names the panel outright. Both
// sequences are GxEPD2's, byte for byte.
//
// A third: Waveshare's 2.13" e-Paper (B) V4, black, white and red, tried
// on box 006 (2026-10-16). Also an SSD1680, BUSY high like the mono one,
// so nothing tells the two apart from outside: it is epd_panel 3, never
// found by itself. Waveshare's EPD_2in13b_V4.c sequence.
//
// And the (B) V3 before it, which is what box 005 turned out to carry: a
// UC8151-type controller (BUSY low while busy, power on with 0x04) and a
// smaller glass, 104 x 212. epd_panel 4. The screens are drawn 250 x 122,
// so it shows their middle: 19 px off each end, 9 off top and bottom.
//
// Every refresh is a full one and powers the panel down afterwards: the
// image stays on the glass with no power at all, and partial refresh is
// not something the four-ink panel can do, so the product never relies
// on it. The rail is shared with the thermocouple and reference counted.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "canvas.h"

struct PanelDef {
  const char *name;
  uint16_t ram_w;          // controller columns, panel's short side
  uint16_t visible_w;      // of those, how many reach the glass
  uint16_t h;              // the long side
  bool busy_high;          // BUSY level that means "busy"
  uint32_t refresh_max_ms; // give up waiting after this
  uint8_t inks;            // 1: black only; accent drawn black
};

extern const PanelDef PANEL_213_MONO;
extern const PanelDef PANEL_213_4INK;
extern const PanelDef PANEL_213_3INK;
extern const PanelDef PANEL_213B_V3;

// The panel the last refresh drove; nullptr before the first one.
const PanelDef *epd_panel(void);

void epd_init(void);

// Draws the canvas and refreshes the whole panel. Blocks for the
// refresh (about 3.6 s on the mono panel, 21 s on the four-ink one). `rotation` is GxEPD2's: 1 or
// 3, the two landscape orientations. Returns false if the panel never
// signalled ready -- with the wrong BUSY polarity this is what happens,
// and it looks exactly like dead hardware.
bool epd_show(const Canvas &c, int rotation);

// How long the last refresh kept BUSY asserted, ms. The mono panel takes
// about 3,600, the four-ink one about 21,000; a number far from that (0,
// say) is the first sign of a wrong driver or a wrong polarity.
uint32_t epd_last_refresh_ms(void);

// Bench (2026-10-16): what BUSY does through a reset and a software reset,
// printed, for a panel that does not answer. The rail is taken and given
// back around it.
void epd_probe(void);
void epd_probe_init(void);
