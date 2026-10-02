// E-paper panel driver.
//
// The board was designed for a four-ink 2.13" panel (GDEY0213F51) that
// is not in hand; the panel fitted today is a mono 2.13" SSD1680 module,
// what GxEPD2 calls GxEPD2_213_B74. This driver runs the mono panel with
// the command sequence GxEPD2 uses for it, which bring-up proved on this
// board. The four-ink panel needs a different sequence, and -- the trap
// that cost bring-up an afternoon -- the OPPOSITE BUSY polarity. Both
// live in the PanelDef, so the swap is one table entry, not a rewrite.
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

// The panel fitted now.
extern const PanelDef PANEL_213_MONO;

void epd_init(void);

// Draws the canvas and refreshes the whole panel. Blocks for the
// refresh (about 3.6 s on the mono panel). `rotation` is GxEPD2's: 1 or
// 3, the two landscape orientations. Returns false if the panel never
// signalled ready -- with the wrong BUSY polarity this is what happens,
// and it looks exactly like dead hardware.
bool epd_show(const Canvas &c, int rotation);

// How long the last refresh kept BUSY asserted, ms. The mono panel takes
// about 3,600; a number far from that is the first sign of a wrong
// driver or a wrong polarity.
uint32_t epd_last_refresh_ms(void);
