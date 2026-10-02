#include "epd.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "board.h"
#include "bus.h"
#include "health.h"
#include "rails.h"

const PanelDef PANEL_213_MONO = {
    "2.13 mono SSD1680 (GxEPD2_213_B74)", 128, 122, 250, true, 10000, 1,
};

namespace {

const PanelDef &P = PANEL_213_MONO;

// One frame in the controller's own layout: ram_w/8 bytes a row, h rows,
// a set bit is white. Static, so it is in internal RAM and DMA can read it.
uint8_t g_ram[128 / 8 * 250];
uint32_t g_last_ms = 0;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool busy(void) {
  return gpio_get_level((gpio_num_t)PIN_EPD_BUSY) == (P.busy_high ? 1 : 0);
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

bool ram_window(void) {
  // Whole RAM, x and y both counting up, pointer back to the origin.
  const uint8_t x[2] = {0, (uint8_t)((P.ram_w - 1) / 8)};
  const uint8_t y[4] = {0, 0, (uint8_t)((P.h - 1) & 0xFF), (uint8_t)((P.h - 1) >> 8)};
  const uint8_t y0[2] = {0, 0};
  return cmd1(0x11, 0x03) && cmd(0x44, x, 2) && cmd(0x45, y, 4) &&
         cmd1(0x4E, 0) && cmd(0x4F, y0, 2);
}

// The canvas in the controller's layout. GxEPD2's rotations, so the
// bench layout lands where it was designed:
//   1: panel x = visible_w-1 - canvas y,  panel y = canvas x
//   3: panel x = canvas y,                 panel y = h-1 - canvas x
void to_native(const Canvas &c, int rotation) {
  memset(g_ram, 0xFF, sizeof(g_ram));
  const int row = P.ram_w / 8;
  for (int py = 0; py < P.h; py++) {
    for (int px = 0; px < P.visible_w; px++) {
      int cx, cy;
      if (rotation == 3) {
        cx = P.h - 1 - py;
        cy = px;
      } else {
        cx = py;
        cy = P.visible_w - 1 - px;
      }
      // One ink panel: accent is printed in black. A four-ink driver
      // would send it to its own colour plane instead.
      if (c.get(cx, cy) != Ink::White) {
        g_ram[py * row + px / 8] &= (uint8_t)~(0x80 >> (px & 7));
      }
    }
  }
}

void release_pins(void) {
  // RST has a pull-up to the panel rail. Driven high while that rail is
  // off, it would feed the panel through the resistor; left as an input
  // it follows the rail down.
  gpio_set_direction((gpio_num_t)PIN_EPD_RST, GPIO_MODE_INPUT);
}

}  // namespace

void epd_init(void) {
  gpio_config_t in = {};
  in.pin_bit_mask = 1ULL << PIN_EPD_BUSY;
  in.mode = GPIO_MODE_INPUT;
  gpio_config(&in);
  release_pins();
}

bool epd_show(const Canvas &c, int rotation) {
  to_native(c, rotation);
  rail_acquire(Rail::Epd);

  // Reset as GxEPD2 does it on this panel: high, low for 20 ms, high.
  gpio_set_direction((gpio_num_t)PIN_EPD_RST, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level((gpio_num_t)PIN_EPD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(20));

  bool ok = wait_ready(1000) >= 0 && cmd(0x12);   // software reset
  vTaskDelay(pdMS_TO_TICKS(10));
  ok = ok && wait_ready(1000) >= 0;

  // GxEPD2_213_B74::_InitDisplay(), byte for byte.
  static const uint8_t drv[3] = {0xF9, 0x00, 0x00};   // 250 gate lines
  static const uint8_t upd[2] = {0x00, 0x80};
  ok = ok && cmd(0x01, drv, 3) && cmd1(0x3C, 0x05) && cmd(0x21, upd, 2) &&
       cmd1(0x18, 0x80) && ram_window();

  // The same image into "current" and "previous": a full refresh then
  // has nothing to difference against, which is the clean way to start
  // after a power cycle.
  ok = ok && cmd(0x24, g_ram, sizeof(g_ram)) && ram_window() &&
       cmd(0x26, g_ram, sizeof(g_ram));

  // Full update: clock, analogue, temperature, display, power down.
  int32_t took = -1;
  if (ok && cmd1(0x22, 0xF7) && cmd(0x20)) {
    took = wait_ready(P.refresh_max_ms);
  }
  g_last_ms = took >= 0 ? (uint32_t)took : 0;

  // Analogue off, then deep sleep. The image stays on the glass.
  if (took >= 0) {
    cmd1(0x22, 0x83);
    cmd(0x20);
    wait_ready(1000);
    cmd1(0x10, 0x01);
  }
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
