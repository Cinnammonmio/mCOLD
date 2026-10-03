#include "leds.h"

#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "board.h"
#include "health.h"
#include "rails.h"

namespace {

// 10 MHz: one tick is 0.1 us. The bit timings are the ones the
// bring-up's NeoPixel driver used at 800 kHz, which lit both part
// types on this chain: 0.4 us high for a zero, 0.8 us for a one,
// 1.25 us a bit.
const uint32_t RES_HZ = 10000000;
const uint16_t T0H = 4, T0L = 8, T1H = 8, T1L = 4;

// Low for longer than this between frames latches the colours. The
// SK6812 wants 80 us; the 2812 variant less.
const uint32_t RESET_US = 100;

rmt_channel_handle_t g_chan = nullptr;
rmt_encoder_handle_t g_enc = nullptr;
SemaphoreHandle_t g_lock = nullptr;
esp_timer_handle_t g_pulse_timer = nullptr;

uint8_t g_rgb[LED_COUNT][3];
bool g_rail_held = false;

volatile uint8_t g_cap = LEDS_BRIGHTNESS_CAP;               // the side light
volatile uint8_t g_cap_front = LEDS_FRONT_BRIGHTNESS_CAP;   // the three on the front

uint8_t scale(int i, uint8_t v) {
  const uint8_t cap = i == LED_SIDE ? g_cap : g_cap_front;
  return (uint8_t)((v * cap + 127) / 255);
}

bool any_lit(void) {
  for (int i = 0; i < LED_COUNT; i++) {
    if (g_rgb[i][0] || g_rgb[i][1] || g_rgb[i][2]) return true;
  }
  return false;
}

bool send(void) {
  uint8_t frame[LED_COUNT * 3];
  for (int i = 0; i < LED_COUNT; i++) {
    frame[i * 3 + 0] = g_rgb[i][1];   // G
    frame[i * 3 + 1] = g_rgb[i][0];   // R
    frame[i * 3 + 2] = g_rgb[i][2];   // B
  }
  rmt_transmit_config_t tc = {};
  tc.loop_count = 0;
  tc.flags.eot_level = 0;             // idle low: never back-power a pixel
  if (rmt_transmit(g_chan, g_enc, frame, sizeof(frame), &tc) != ESP_OK) {
    return false;
  }
  const bool ok = rmt_tx_wait_all_done(g_chan, 50) == ESP_OK;
  esp_rom_delay_us(RESET_US);
  return ok;
}

// Called with the lock held.
bool show_locked(void) {
  const bool lit = any_lit();
  if (lit && !g_rail_held) {
    rail_acquire(Rail::Led);          // waits out the 50 ms settle
    g_rail_held = true;
  }
  if (!g_rail_held) return true;      // dark and unpowered: nothing to send

  const bool ok = send();
  if (ok) health_ok(Dev::Leds);
  else health_fail(Dev::Leds, -600);

  if (!lit) {
    rail_release(Rail::Led);
    g_rail_held = false;
  }
  return ok;
}

// The pixel a pulse lit, so that ending it puts out that one only. The
// side light may be showing charge status at the same time, and a tap
// on the front must not switch it off.
volatile int g_pulse_idx = -1;

void pulse_done(void *) {
  const int i = g_pulse_idx;
  g_pulse_idx = -1;
  if (i < 0) return;
  leds_set(i, 0, 0, 0);
  leds_show();
}

}  // namespace

bool leds_init(void) {
  g_lock = xSemaphoreCreateMutex();
  memset(g_rgb, 0, sizeof(g_rgb));

  rmt_tx_channel_config_t cc = {};
  cc.gpio_num = (gpio_num_t)PIN_LED_DATA;
  cc.clk_src = RMT_CLK_SRC_DEFAULT;
  cc.resolution_hz = RES_HZ;
  cc.mem_block_symbols = 64;
  cc.trans_queue_depth = 2;
  if (rmt_new_tx_channel(&cc, &g_chan) != ESP_OK) {
    health_fail(Dev::Leds, -601);
    return false;
  }

  rmt_bytes_encoder_config_t ec = {};
  ec.bit0.level0 = 1;
  ec.bit0.duration0 = T0H;
  ec.bit0.level1 = 0;
  ec.bit0.duration1 = T0L;
  ec.bit1.level0 = 1;
  ec.bit1.duration0 = T1H;
  ec.bit1.level1 = 0;
  ec.bit1.duration1 = T1L;
  ec.flags.msb_first = 1;
  if (rmt_new_bytes_encoder(&ec, &g_enc) != ESP_OK ||
      rmt_enable(g_chan) != ESP_OK) {
    health_fail(Dev::Leds, -602);
    return false;
  }

  esp_timer_create_args_t ta = {};
  ta.callback = pulse_done;
  ta.name = "led_pulse";
  esp_timer_create(&ta, &g_pulse_timer);
  return true;
}

void leds_set(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= LED_COUNT || !g_lock) return;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_rgb[i][0] = scale(i, r);
  g_rgb[i][1] = scale(i, g);
  g_rgb[i][2] = scale(i, b);
  xSemaphoreGive(g_lock);
}

void leds_clear(void) {
  if (!g_lock) return;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  memset(g_rgb, 0, sizeof(g_rgb));
  xSemaphoreGive(g_lock);
}

bool leds_show(void) {
  if (!g_chan || !g_lock) return false;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  const bool ok = show_locked();
  xSemaphoreGive(g_lock);
  return ok;
}

void leds_off(void) {
  leds_clear();
  leds_show();
}

void leds_pulse(int i, uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  if (!g_pulse_timer) return;
  if (ms > LEDS_PULSE_MAX_MS) ms = LEDS_PULSE_MAX_MS;
  esp_timer_stop(g_pulse_timer);      // a new pulse replaces the old one
  const int prev = g_pulse_idx;
  if (prev >= 0 && prev != i) leds_set(prev, 0, 0, 0);
  g_pulse_idx = i;
  leds_set(i, r, g, b);
  leds_show();
  esp_timer_start_once(g_pulse_timer, (uint64_t)ms * 1000);
}

bool leds_pulse_active(int index) { return g_pulse_idx == index; }

uint8_t cap_of(int pct) {
  if (pct < 1) pct = 1;
  if (pct > 100) pct = 100;
  return (uint8_t)((pct * 255 + 50) / 100);
}

void leds_set_brightness(int pct) { g_cap = cap_of(pct); }

void leds_set_front_brightness(int pct) { g_cap_front = cap_of(pct); }
