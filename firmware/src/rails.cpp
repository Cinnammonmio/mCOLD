#include "rails.h"

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "board.h"

namespace {

struct RailDef {
  gpio_num_t pin;
  bool active_high;
  uint32_t settle_ms;
  const char *name;
};

// Settle times are the ones the bring-up measured the hard way. The LED
// chain was the expensive one: at five milliseconds the pixels had no
// supply when the first frame arrived, never latched it, and sat dark
// while the firmware believed they were lit.
const RailDef DEFS[(int)Rail::Count] = {
    {(gpio_num_t)PIN_EPD_PWR_EN, true, 60, "epd"},
    {(gpio_num_t)PIN_SD_PWR_EN, true, 60, "sd_gnss"},
    {(gpio_num_t)PIN_LED_PWR_EN, false, 50, "led"},
};

int g_users[(int)Rail::Count];
SemaphoreHandle_t g_lock;

void drive(Rail r, bool on) {
  const RailDef &d = DEFS[(int)r];
  gpio_set_level(d.pin, (on == d.active_high) ? 1 : 0);
}

}  // namespace

void rails_init(void) {
  g_lock = xSemaphoreCreateMutex();
  for (int i = 0; i < (int)Rail::Count; i++) {
    g_users[i] = 0;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << DEFS[i].pin;
    cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&cfg);
    drive((Rail)i, false);
  }
}

void rail_acquire(Rail r) {
  if (r >= Rail::Count) return;
  bool just_on = false;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
    just_on = (g_users[(int)r]++ == 0);
    if (just_on) drive(r, true);
    xSemaphoreGive(g_lock);
  }
  // Waited outside the lock: holding it through a settle delay would
  // block every other rail for no reason.
  if (just_on) vTaskDelay(pdMS_TO_TICKS(DEFS[(int)r].settle_ms));
}

void rail_release(Rail r) {
  if (r >= Rail::Count) return;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
  if (g_users[(int)r] > 0 && --g_users[(int)r] == 0) drive(r, false);
  xSemaphoreGive(g_lock);
}

bool rail_is_on(Rail r) {
  return r < Rail::Count && g_users[(int)r] > 0;
}

int rail_users(Rail r) {
  return r < Rail::Count ? g_users[(int)r] : 0;
}

const char *rail_name(Rail r) {
  return r < Rail::Count ? DEFS[(int)r].name : "?";
}

int rails_all_off(void) {
  int leaked = 0;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) return -1;
  for (int i = 0; i < (int)Rail::Count; i++) {
    leaked += g_users[i];
    g_users[i] = 0;
    drive((Rail)i, false);
  }
  xSemaphoreGive(g_lock);
  return leaked;
}
