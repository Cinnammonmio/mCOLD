#include "chargeled.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

#include "board.h"
#include "buzzer.h"
#include "leds.h"

namespace {

// REG09: CHRG_FAULT is bits 5:4, BAT_FAULT bit 3. The watchdog and NTC
// bits are not charge faults and are reported elsewhere.
const uint8_t REG09_CHRG_FAULT = 0x30;
const uint8_t REG09_BAT_FAULT = 0x08;

const uint32_t FRAME_MS = 40;
const uint32_t BREATHE_MS = 2000;

// Amber is red plus green; the right mix depends on the diffuser and is
// to be tuned once the case exists.
const uint8_t AMBER_R = 255, AMBER_G = 150;

volatile ChargeLed g_mode = ChargeLed::Off;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool input_present(void) {
  return gpio_get_level((gpio_num_t)PIN_PG_N) == 0;   // PG# low = power good
}

// Never fully dark while breathing. A pixel at zero lets the LED rail
// go, and taking it back costs a 50 ms settle -- a visible hitch at the
// bottom of every breath.
float breathe(uint32_t t) {
  const float ph = (float)(t % BREATHE_MS) / BREATHE_MS;
  return 0.12f + 0.88f * 0.5f * (1.0f - cosf(2.0f * (float)M_PI * ph));
}

void render(ChargeLed m, uint32_t t) {
  float k = 0;
  uint8_t r = 0, g = 0, b = 0;
  switch (m) {
    case ChargeLed::Off:
      break;
    // Charging is good news, in the calm family (2026-10-04): blue while
    // it fills, cyan near the top and when full. Amber and red stay for
    // the two cases that want a person to look.
    case ChargeLed::Charging:
      k = breathe(t); g = 40; b = 255;
      break;
    case ChargeLed::Topping:
      k = breathe(t); g = 200; b = 255;
      break;
    case ChargeLed::Full:
      k = 1; g = 200; b = 255;
      break;
    case ChargeLed::NotCharging:
      k = (t % 2000) < 120 ? 1 : 0; r = AMBER_R; g = AMBER_G;
      break;
    case ChargeLed::Fault:
      k = (t % 1000) < 120 ? 1 : 0; r = 255;
      break;
  }
  leds_set(LED_SIDE, (uint8_t)(r * k), (uint8_t)(g * k), (uint8_t)(b * k));
}

void task(void *) {
  ChargeLed shown = ChargeLed::Off;
  bool stale = false;
  for (;;) {
    if (leds_pulse_active(LED_SIDE)) {
      // A pulse has the side pixel (a test, say). Stand aside, and
      // redraw whatever is due once it ends.
      stale = true;
      vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
      continue;
    }
    const ChargeLed m = input_present() ? g_mode : ChargeLed::Off;
    if (stale || m != shown || m != ChargeLed::Off) {
      stale = false;
      render(m, now_ms());
      leds_show();
      shown = m;
    }
    // Off and idle: nothing to animate, so look less often.
    vTaskDelay(pdMS_TO_TICKS(m == ChargeLed::Off ? 200 : FRAME_MS));
  }
}

}  // namespace

void chargeled_start(void) {
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << PIN_PG_N;
  c.mode = GPIO_MODE_INPUT;
  // PG# is open drain. The pull-up only conducts while external power
  // holds the pin low, so it never costs the battery anything.
  c.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&c);
  xTaskCreatePinnedToCore(task, "chgled", 3072, nullptr, 2, nullptr, 0);
}

void chargeled_update(const PowerStatus &ps) {
  ChargeLed m;
  if (!ps.charger_valid || !ps.power_good) {
    // No word from the charger is not a charge fault: show nothing
    // rather than a red light the charger never asked for.
    m = ChargeLed::Off;
  } else if (ps.fault_reg & (REG09_CHRG_FAULT | REG09_BAT_FAULT)) {
    m = ChargeLed::Fault;
  } else {
    switch (ps.charge) {
      case ChargeState::PreCharge:
      case ChargeState::FastCharge:
        m = (ps.cell_valid && ps.soc_percent >= CHARGELED_TAPER_SOC)
                ? ChargeLed::Topping
                : ChargeLed::Charging;
        break;
      case ChargeState::Done:
        m = ChargeLed::Full;
        break;
      default:
        m = ChargeLed::NotCharging;
        break;
    }
  }
  // The design asks for one beep when a fault appears, not one per read.
  if (m == ChargeLed::Fault && g_mode != ChargeLed::Fault) buzzer_beep(150);
  g_mode = m;
}

ChargeLed chargeled_mode(void) { return g_mode; }

const char *chargeled_name(ChargeLed m) {
  switch (m) {
    case ChargeLed::Off:         return "off";
    case ChargeLed::Charging:    return "charging (breathe blue)";
    case ChargeLed::Topping:     return "topping up (breathe cyan)";
    case ChargeLed::Full:        return "full (steady cyan)";
    case ChargeLed::NotCharging: return "input, not charging (yellow blink)";
    case ChargeLed::Fault:       return "charge fault (red blink)";
  }
  return "?";
}
