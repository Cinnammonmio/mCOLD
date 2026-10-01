#include "buzzer.h"

#include <driver/ledc.h>
#include <esp_timer.h>

#include "board.h"
#include "health.h"

namespace {

const ledc_mode_t MODE = LEDC_LOW_SPEED_MODE;
const ledc_timer_t TIMER = LEDC_TIMER_0;
const ledc_channel_t CHAN = LEDC_CHANNEL_0;
const ledc_timer_bit_t RES = LEDC_TIMER_10_BIT;
const uint32_t HALF = 1u << (10 - 1);     // 50% duty

esp_timer_handle_t g_timer = nullptr;
bool g_ready = false;
volatile bool g_muted = false;
volatile bool g_sounding = false;
volatile uint32_t g_stopped_ms = 0;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void stop_cb(void *) { buzzer_stop(); }

}  // namespace

bool buzzer_init(void) {
  ledc_timer_config_t tc = {};
  tc.speed_mode = MODE;
  tc.duty_resolution = RES;
  tc.timer_num = TIMER;
  tc.freq_hz = BUZZER_FREQ_HZ;
  tc.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&tc) != ESP_OK) {
    health_fail(Dev::Buzzer, -700);
    return false;
  }

  // Duty 0 from the start: the channel exists, the transistor is off.
  ledc_channel_config_t cc = {};
  cc.gpio_num = PIN_BUZZER;
  cc.speed_mode = MODE;
  cc.channel = CHAN;
  cc.timer_sel = TIMER;
  cc.duty = 0;
  cc.hpoint = 0;
  if (ledc_channel_config(&cc) != ESP_OK) {
    health_fail(Dev::Buzzer, -701);
    return false;
  }

  esp_timer_create_args_t ta = {};
  ta.callback = stop_cb;
  ta.name = "buzzer";
  if (esp_timer_create(&ta, &g_timer) != ESP_OK) return false;
  g_ready = true;
  return true;
}

void buzzer_beep(uint32_t ms) {
  if (!g_ready || g_muted || ms == 0) return;
  if (ms > BUZZER_MAX_MS) ms = BUZZER_MAX_MS;
  esp_timer_stop(g_timer);
  // There is no feedback from a buzzer, so "Ok" here only means the
  // peripheral accepted the command. Whether it made a sound is a
  // question for a person standing next to it.
  if (ledc_set_duty(MODE, CHAN, HALF) == ESP_OK &&
      ledc_update_duty(MODE, CHAN) == ESP_OK) {
    health_ok(Dev::Buzzer);
  } else {
    health_fail(Dev::Buzzer, -702);
  }
  g_sounding = true;
  esp_timer_start_once(g_timer, (uint64_t)ms * 1000);
}

void buzzer_stop(void) {
  if (!g_ready) return;
  // Duty 0 rather than ledc_stop(): the channel keeps the pin, and the
  // pin is held low, so Q5 is off and stays off.
  ledc_set_duty(MODE, CHAN, 0);
  ledc_update_duty(MODE, CHAN);
  if (g_sounding) {
    g_sounding = false;
    g_stopped_ms = now_ms();
  }
}

bool buzzer_recent(uint32_t tail_ms) {
  if (g_sounding) return true;
  return g_stopped_ms && now_ms() - g_stopped_ms < tail_ms;
}

void buzzer_mute(bool on) {
  g_muted = on;
  if (on) buzzer_stop();
}

bool buzzer_is_muted(void) { return g_muted; }
