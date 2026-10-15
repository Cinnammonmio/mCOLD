#include "epd.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bus.h"
#include "config.h"
#include "health.h"
#include "rails.h"

const PanelDef PANEL_213_MONO = {
    "2.13 mono SSD1680 (GxEPD2_213_B74)", 128, 122, 250, true, 10000, 1,
};
const PanelDef PANEL_213_3INK = {
    "2.13 three-ink SSD1680 (Waveshare 2.13 B V4)", 128, 122, 250, true, 40000, 3,
};
const PanelDef PANEL_213B_V3 = {
    "2.13 three-ink UC8151 (Waveshare 2.13 B V3, 104 x 212)", 104, 104, 212, false, 40000, 3,
};
const PanelDef PANEL_213_4INK = {
    "2.13 four-ink GDEY0213F51 (GxEPD2_213c_GDEY0213F51)", 128, 122, 250, false, 40000, 4,
};

namespace {

const PanelDef *g_p = nullptr;      // the panel being driven

// One frame in the controller's own layout. Mono: ram_w/8 bytes a row, a
// set bit white. Four-ink: ram_w/4 bytes a row, two bits a pixel, first
// pixel in the top bits: 00 black, 01 white, 10 yellow, 11 red. Static, so
// it is in internal RAM and DMA can read it.
uint8_t g_ram[128 / 4 * 250];
uint32_t g_last_ms = 0;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool busy(void) {
  return gpio_get_level((gpio_num_t)PIN_EPD_BUSY) == (g_p->busy_high ? 1 : 0);
}

// Waits for BUSY to clear. Returns the time it took, or -1 on timeout.
int32_t wait_ready(uint32_t timeout_ms) {
  const uint32_t t0 = now_ms();
  while (busy()) {
    if (now_ms() - t0 > timeout_ms) return -1;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return (int32_t)(now_ms() - t0);
}

// A command and its data, under the bus lock. DC low for the command
// byte, high for the data; CS low around both.
bool cmd(uint8_t c, const uint8_t *data = nullptr, size_t n = 0) {
  if (!spi3_take(500)) return false;
  gpio_set_level((gpio_num_t)PIN_EPD_CS, 0);
  gpio_set_level((gpio_num_t)PIN_EPD_DC, 0);
  bool ok = spi3_epd_write(&c, 1) == BusErr::Ok;
  gpio_set_level((gpio_num_t)PIN_EPD_DC, 1);
  if (ok && n) ok = spi3_epd_write(data, n) == BusErr::Ok;
  gpio_set_level((gpio_num_t)PIN_EPD_CS, 1);
  spi3_give();
  return ok;
}

bool cmd1(uint8_t c, uint8_t d) { return cmd(c, &d, 1); }

// The four-ink controller's framing, as GxEPD2 sends it: the command byte
// in a CS pulse of its own, then each parameter byte in its own, and only
// a long block (the image) in one. The mono panel takes command and data
// in one CS low; this one did not power up that way (2026-10-15).
bool cs_write(bool data, const uint8_t *p, size_t n) {
  if (!spi3_take(500)) return false;
  gpio_set_level((gpio_num_t)PIN_EPD_DC, data ? 1 : 0);
  gpio_set_level((gpio_num_t)PIN_EPD_CS, 0);
  const bool ok = spi3_epd_write(p, n) == BusErr::Ok;
  gpio_set_level((gpio_num_t)PIN_EPD_CS, 1);
  spi3_give();
  return ok;
}

bool c4(uint8_t c, const uint8_t *data = nullptr, size_t n = 0) {
  bool ok = cs_write(false, &c, 1);
  if (n > 64) return ok && cs_write(true, data, n);
  for (size_t i = 0; ok && i < n; i++) ok = cs_write(true, &data[i], 1);
  return ok;
}

bool c41(uint8_t c, uint8_t d) { return c4(c, &d, 1); }

// The canvas pixel that lands on panel pixel (px, py). GxEPD2's
// rotations, so the bench layout lands where it was designed:
//   1: panel x = visible_w-1 - canvas y,  panel y = canvas x
//   3: panel x = canvas y,                 panel y = h-1 - canvas x
Ink at(const Canvas &c, int rotation, int px, int py) {
  if (rotation == 3) return c.get(g_p->h - 1 - py, px);
  return c.get(py, g_p->visible_w - 1 - px);
}

void to_native_mono(const Canvas &c, int rotation) {
  const int row = g_p->ram_w / 8;
  memset(g_ram, 0xFF, (size_t)row * g_p->h);
  for (int py = 0; py < g_p->h; py++) {
    for (int px = 0; px < g_p->visible_w; px++) {
      // One ink: accent is printed in black.
      if (at(c, rotation, px, py) != Ink::White) {
        g_ram[py * row + px / 8] &= (uint8_t)~(0x80 >> (px & 7));
      }
    }
  }
}

// Two planes of ram_w/8 bytes a row: black (a set bit white) in the first
// half of g_ram, red (a clear bit red) in the second. A red pixel is white
// in the black plane.
const size_t PLANE3 = 128 / 8 * 250;
void to_native_3ink(const Canvas &c, int rotation) {
  const int row = g_p->ram_w / 8;
  memset(g_ram, 0xFF, 2 * PLANE3);
  uint8_t *red = g_ram + PLANE3;
  for (int py = 0; py < g_p->h; py++) {
    for (int px = 0; px < g_p->visible_w; px++) {
      const Ink k = at(c, rotation, px, py);
      if (k == Ink::White) continue;
      uint8_t *plane = k == Ink::Accent ? red : g_ram;
      plane[py * row + px / 8] &= (uint8_t)~(0x80 >> (px & 7));
    }
  }
}

// (B) V3: 104 x 212, two planes of 13 bytes a row (black: a set bit white;
// red: a clear bit red), the canvas's middle 212 x 104.
const size_t PLANE_V3 = 104 / 8 * 212;
void to_native_v3(const Canvas &c, int rotation) {
  const int row = 104 / 8, ox = (CANVAS_W - 212) / 2, oy = (CANVAS_H - 104) / 2;
  memset(g_ram, 0xFF, 2 * PLANE_V3);
  uint8_t *red = g_ram + PLANE_V3;
  for (int py = 0; py < 212; py++) {
    for (int px = 0; px < 104; px++) {
      const int cx = ox + (rotation == 3 ? 211 - py : py);
      const int cy = oy + (rotation == 3 ? px : 103 - px);
      const Ink k = c.get(cx, cy);
      if (k == Ink::White) continue;
      uint8_t *plane = k == Ink::Accent ? red : g_ram;
      plane[py * row + px / 8] &= (uint8_t)~(0x80 >> (px & 7));
    }
  }
}

void to_native_4ink(const Canvas &c, int rotation) {
  const int row = g_p->ram_w / 4;
  memset(g_ram, 0x55, (size_t)row * g_p->h);   // all white
  for (int py = 0; py < g_p->h; py++) {
    for (int px = 0; px < g_p->visible_w; px++) {
      const Ink k = at(c, rotation, px, py);
      if (k == Ink::White) continue;
      // Accent is the red ink: what the screens mark in it is an alarm.
      const uint8_t v = k == Ink::Accent ? 0x3 : 0x0;
      const int sh = 6 - 2 * (px & 3);
      uint8_t &b = g_ram[py * row + px / 4];
      b = (uint8_t)((b & ~(0x3 << sh)) | (v << sh));
    }
  }
}

void release_pins(void) {
  // RST has a pull-up to the panel rail. Driven high while that rail is
  // off, it would feed the panel through the resistor; left as an input
  // it follows the rail down.
  gpio_set_direction((gpio_num_t)PIN_EPD_RST, GPIO_MODE_INPUT);
}

void hw_reset(uint32_t low_ms) {
  gpio_set_direction((gpio_num_t)PIN_EPD_RST, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(low_ms));
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
}

// Which panel, from config epd_panel: 1 mono, 2 four-ink, 0 find out.
// Finding out: 250 ms after a reset (the four-ink panel's own reset takes
// under 200), its BUSY rests high -- "ready" in its polarity -- where the
// mono panel's rests low. Asked again at every refresh: panels get swapped
// on the bench.
const PanelDef *pick(void) {
  const int32_t want = config().epd_panel;
  if (want == 1) return &PANEL_213_MONO;
  if (want == 2) return &PANEL_213_4INK;
  if (want == 3) return &PANEL_213_3INK;
  if (want == 4) return &PANEL_213B_V3;
  hw_reset(10);
  vTaskDelay(pdMS_TO_TICKS(250));
  return gpio_get_level((gpio_num_t)PIN_EPD_BUSY) ? &PANEL_213_4INK : &PANEL_213_MONO;
}

bool ram_window_mono(void) {
  // Whole RAM, x and y both counting up, pointer back to the origin.
  const uint8_t x[2] = {0, (uint8_t)((g_p->ram_w - 1) / 8)};
  const uint8_t y[4] = {0, 0, (uint8_t)((g_p->h - 1) & 0xFF), (uint8_t)((g_p->h - 1) >> 8)};
  const uint8_t y0[2] = {0, 0};
  return cmd1(0x11, 0x03) && cmd(0x44, x, 2) && cmd(0x45, y, 4) &&
         cmd1(0x4E, 0) && cmd(0x4F, y0, 2);
}

// GxEPD2_213_B74, as bring-up proved it on this board. -1: no answer.
int32_t show_mono(void) {
  hw_reset(20);
  vTaskDelay(pdMS_TO_TICKS(20));
  bool ok = wait_ready(1000) >= 0 && cmd(0x12);   // software reset
  vTaskDelay(pdMS_TO_TICKS(10));
  ok = ok && wait_ready(1000) >= 0;

  static const uint8_t drv[3] = {0xF9, 0x00, 0x00};   // 250 gate lines
  static const uint8_t upd[2] = {0x00, 0x80};
  ok = ok && cmd(0x01, drv, 3) && cmd1(0x3C, 0x05) && cmd(0x21, upd, 2) &&
       cmd1(0x18, 0x80) && ram_window_mono();

  // The same image into "current" and "previous": a full refresh then
  // has nothing to difference against, which is the clean way to start
  // after a power cycle.
  const size_t n = (size_t)g_p->ram_w / 8 * g_p->h;
  ok = ok && cmd(0x24, g_ram, n) && ram_window_mono() && cmd(0x26, g_ram, n);

  // Full update: clock, analogue, temperature, display, power down.
  int32_t took = -1;
  if (ok && cmd1(0x22, 0xF7) && cmd(0x20)) took = wait_ready(g_p->refresh_max_ms);

  // Analogue off, then deep sleep. The image stays on the glass.
  if (took >= 0) {
    cmd1(0x22, 0x83);
    cmd(0x20);
    wait_ready(1000);
    cmd1(0x10, 0x01);
  }
  return took;
}

// Waveshare's EPD_2in13b_V4.c: init, both planes, refresh, deep sleep.
// -1: no answer; the step that failed is said on the console.
int32_t show_3ink(void) {
  hw_reset(2);
  vTaskDelay(pdMS_TO_TICKS(20));
  bool ok = wait_ready(1000) >= 0 && cmd(0x12);   // software reset
  ok = ok && wait_ready(1000) >= 0;
  if (!ok) {
    printf("[display] three-ink: no answer after reset\n");
    return -1;
  }
  static const uint8_t drv[3] = {0xF9, 0x00, 0x00};   // 250 gate lines
  static const uint8_t upd[2] = {0x80, 0x80};         // red RAM inverted, as Waveshare
  ok = cmd(0x01, drv, 3) && cmd1(0x11, 0x03) && ram_window_mono() && cmd1(0x3C, 0x05) &&
       cmd1(0x18, 0x80) && cmd(0x21, upd, 2) && wait_ready(1000) >= 0;
  ok = ok && cmd(0x24, g_ram, PLANE3) && cmd(0x26, g_ram + PLANE3, PLANE3);
  int32_t took = -1;
  if (ok && cmd(0x20)) {             // update with the panel's own sequence
    vTaskDelay(pdMS_TO_TICKS(10));
    took = wait_ready(g_p->refresh_max_ms);
  }
  if (!ok) printf("[display] three-ink: init or image not sent\n");
  else if (took < 0) printf("[display] three-ink: refresh did not finish\n");
  cmd1(0x10, 0x01);                  // deep sleep; the image stays on the glass
  vTaskDelay(pdMS_TO_TICKS(100));
  return took;
}

// Waveshare's EPD_2in13b_V3.c. -1: no answer; the failed step is said.
int32_t show_v3(void) {
  gpio_set_direction((gpio_num_t)PIN_EPD_RST, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(2));
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
  if (!c4(0x04) || wait_ready(5000) < 0) {             // power on
    printf("[display] (B) V3: power-on never finished\n");
    return -1;
  }
  static const uint8_t psr[2] = {0x0F, 0x89};          // LUT from OTP, booster timing
  static const uint8_t tres[3] = {0x68, 0x00, 0xD4};   // 104 x 212
  bool ok = c4(0x00, psr, 2) && c4(0x61, tres, 3) && c41(0x50, 0x77) &&
            c4(0x10, g_ram, PLANE_V3) && c4(0x13, g_ram + PLANE_V3, PLANE_V3);
  int32_t took = -1;
  if (ok && c4(0x12)) {                                // display refresh
    vTaskDelay(pdMS_TO_TICKS(100));
    took = wait_ready(g_p->refresh_max_ms);
  }
  if (!ok) printf("[display] (B) V3: setup or image not sent\n");
  else if (took < 0) printf("[display] (B) V3: refresh did not finish\n");
  c41(0x50, 0xF7);
  c4(0x02);                                            // power off
  wait_ready(2000);
  c41(0x07, 0xA5);                                     // deep sleep
  return took;
}

// The four-ink panel, a full refresh: the sequence of Waveshare's
// EPD_2in13g.c and GxEPD2_213c_GDEY0213F51, which agree command for
// command. -1: no answer; the step that failed is said on the console.
int32_t show_4ink(void) {
  // Waveshare's reset: 200 ms high, 2 ms low, 200 ms high.
  gpio_set_direction((gpio_num_t)PIN_EPD_RST, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(2));
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
  if (wait_ready(1000) < 0) {
    printf("[display] four-ink: no answer after reset\n");
    return -1;
  }

  static const uint8_t psr[2] = {0x0F, 0x29};
  static const uint8_t pwr[2] = {0x07, 0x00};
  static const uint8_t pofs[3] = {0x10, 0x54, 0x44};
  static const uint8_t btst[7] = {0x05, 0x00, 0x3F, 0x0A, 0x25, 0x12, 0x1A};
  static const uint8_t tcon[2] = {0x02, 0x02};
  static const uint8_t tres[4] = {0x00, 128, 0x00, 250};      // 128 sources, 250 gates
  static const uint8_t area[9] = {0, 0, 0, 127, 0, 0, 0, 249, 0x00};   // all of it, not partial
  // Not 0x01 (power setting) nor 0x06 (boost soft start): with either of
  // them, alone, the boost on this board never comes up (epd probe init,
  // box 005, 2026-10-16) -- its boost is not Waveshare's reference one
  // (requirements 4.7). The controller's own defaults bring it up in
  // 130 ms. With epd_4ink_full 1 they are sent as the reference does.
  const bool full = config().epd_4ink_full != 0;
  bool ok = c41(0x4D, 0x78) && c4(0x00, psr, 2) && (!full || c4(0x01, pwr, 2)) &&
            c4(0x03, pofs, 3) && (!full || c4(0x06, btst, 7)) && c41(0x50, 0x37) &&
            c4(0x60, tcon, 2) && c4(0x61, tres, 4) &&
            c41(0xE7, 0x1C) && c41(0xE3, 0x22) && c41(0xB4, 0xD0) && c41(0xB5, 0x03) &&
            c41(0xE9, 0x01) && c41(0x30, 0x08);
  // Power on: the controller runs its boost up to the panel voltages and
  // holds BUSY low until they are there (about 160 ms). GxEPD2 allows 10 s.
  // If it never lets go, the boost never got there: on box 002, the first
  // four-ink panel fitted (2026-10-15), it did not in 10 s.
  if (!ok || !c4(0x04) || wait_ready(10000) < 0) {
    printf("[display] four-ink: %s\n", ok ? "power-on never finished (BUSY low 10 s): the boost" : "init not sent");
    return -1;
  }
  ok = c4(0x83, area, 9) && c4(0x10, g_ram, (size_t)g_p->ram_w / 4 * g_p->h);

  // Full refresh: border white, then display refresh.
  int32_t took = -1;
  if (ok && c4(0x83, area, 9) && c41(0x50, 0x37) && c41(0x12, 0x00)) {
    vTaskDelay(pdMS_TO_TICKS(1));
    took = wait_ready(g_p->refresh_max_ms);
  }
  if (took < 0) printf("[display] four-ink: refresh did not finish\n");
  // Power off (Waveshare: at least 100 ms before deep sleep), then deep
  // sleep; the image stays on the glass.
  c41(0x02, 0x00);
  wait_ready(1000);
  vTaskDelay(pdMS_TO_TICKS(100));
  c41(0x07, 0xA5);
  return took;
}

}  // namespace

void epd_init(void) {
  gpio_config_t in = {};
  in.pin_bit_mask = 1ULL << PIN_EPD_BUSY;
  in.mode = GPIO_MODE_INPUT;
  gpio_config(&in);
  release_pins();
}

const PanelDef *epd_panel(void) { return g_p; }

bool epd_show(const Canvas &c, int rotation) {
  rail_acquire(Rail::Epd);
  const PanelDef *was = g_p;
  g_p = pick();
  if (g_p != was) printf("[display] panel: %s\n", g_p->name);

  int32_t took;
  if (g_p->inks == 4) {
    to_native_4ink(c, rotation);
    took = show_4ink();
  } else if (g_p == &PANEL_213B_V3) {
    to_native_v3(c, rotation);
    took = show_v3();
  } else if (g_p->inks == 3) {
    to_native_3ink(c, rotation);
    took = show_3ink();
  } else {
    to_native_mono(c, rotation);
    took = show_mono();
  }
  g_last_ms = took >= 0 ? (uint32_t)took : 0;
  release_pins();
  rail_release(Rail::Epd);

  if (took < 0) {
    health_fail(Dev::Display, -800);
    return false;
  }
  health_ok(Dev::Display);
  return true;
}

uint32_t epd_last_refresh_ms(void) { return g_last_ms; }

namespace {
// BUSY for `ms`, sampled every 2 ms: its level at the start and each change.
void trace(const char *what, uint32_t ms) {
  const uint32_t t0 = now_ms();
  int lvl = gpio_get_level((gpio_num_t)PIN_EPD_BUSY);
  printf("  %-26s BUSY %d at 0 ms", what, lvl);
  int changes = 0;
  while (now_ms() - t0 < ms) {
    const int v = gpio_get_level((gpio_num_t)PIN_EPD_BUSY);
    if (v != lvl) {
      lvl = v;
      if (++changes <= 8) printf(", %d at %lu", v, (unsigned long)(now_ms() - t0));
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  printf(changes > 8 ? ", ... (%d changes)\n" : "\n", changes);
}
}  // namespace

// Bench: each command of the four-ink init alone after a reset, then a
// power on -- which of them keeps the boost from coming up on this board.
void epd_probe_init(void) {
  struct Step { uint8_t c; uint8_t n; uint8_t d[7]; };
  static const Step steps[] = {
      {0x00, 0, {}},  // none: the defaults
      {0x4D, 1, {0x78}},
      {0x00, 2, {0x0F, 0x29}},
      {0x01, 2, {0x07, 0x00}},
      {0x03, 3, {0x10, 0x54, 0x44}},
      {0x06, 7, {0x05, 0x00, 0x3F, 0x0A, 0x25, 0x12, 0x1A}},
      {0x50, 1, {0x37}},
      {0x60, 2, {0x02, 0x02}},
      {0x61, 4, {0x00, 128, 0x00, 250}},
      {0xE7, 1, {0x1C}},
      {0xE3, 1, {0x22}},
      {0xB4, 1, {0xD0}},
      {0xB5, 1, {0x03}},
      {0xE9, 1, {0x01}},
      {0x30, 1, {0x08}},
  };
  rail_acquire(Rail::Epd);
  vTaskDelay(pdMS_TO_TICKS(50));
  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
    const Step &st = steps[i];
    gpio_set_direction((gpio_num_t)PIN_EPD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (i) c4(st.c, st.d, st.n);
    c4(0x04);
    const uint32_t t0 = now_ms();
    while (gpio_get_level((gpio_num_t)PIN_EPD_BUSY) == 0 && now_ms() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(2));
    const uint32_t ms = now_ms() - t0;
    if (i) printf("  0x%02X (%u bytes) then power on: ", st.c, st.n);
    else printf("  defaults, power on: ");
    printf(ms >= 3000 ? "NEVER (3 s)\n" : "%lu ms\n", (unsigned long)ms);
    c41(0x02, 0x00);
    vTaskDelay(pdMS_TO_TICKS(150));
  }
  c41(0x07, 0xA5);
  release_pins();
  rail_release(Rail::Epd);
}

void epd_probe(void) {
  rail_acquire(Rail::Epd);
  vTaskDelay(pdMS_TO_TICKS(50));
  trace("rail on", 300);
  gpio_set_direction((gpio_num_t)PIN_EPD_RST, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 0);
  trace("RST low", 20);
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
  trace("RST high (hardware reset)", 500);
  // SSD1680 (mono, three-ink): software reset holds BUSY high a moment.
  cmd(0x12);
  trace("0x12 software reset", 500);
  // A UC8xxx-type (four-ink): power on holds BUSY low until the boost is up.
  cmd(0x04);
  trace("0x04 power on", 1500);
  // A refresh of whatever is in RAM: the four-ink controller wants a
  // parameter (0x12 0x00), the (B) V3 none. BUSY low for seconds = it ran.
  c4(0x12);
  trace("0x12 refresh, no parameter", 3000);
  c41(0x12, 0x00);
  trace("0x12 0x00 refresh", 3000);
  cmd1(0x02, 0x00);
  cmd1(0x10, 0x01);                  // deep sleep in either family's terms
  release_pins();
  rail_release(Rail::Epd);
  printf("  SSD1680 answers 0x12 with BUSY high for a few ms; a panel that never moves BUSY is not hearing us\n");
}
