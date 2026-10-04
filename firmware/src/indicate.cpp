#include "indicate.h"

#include <driver/gpio.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "board.h"
#include "buzzer.h"
#include "config.h"
#include "health.h"
#include "leds.h"
#include "pm.h"
#include "record.h"
#include "timekeep.h"
#include "trip.h"

namespace {

// ---- the pattern vocabulary (docs/led-design.md) ---------------------
//
// Segments alternate on, off, on ... in milliseconds. Every pattern ends
// dark: nothing here can leave a light on.

enum class Pat : uint8_t { Tick, Blink, Double, Triple, Fast, Step };

struct PatDef {
  uint8_t n;
  uint16_t seg[9];
};

const PatDef PATS[] = {
    {1, {40}},                                         // TICK    40 ms
    {1, {120}},                                        // BLINK  120 ms
    {3, {80, 120, 80}},                                // DOUBLE 280 ms
    {5, {80, 70, 80, 70, 80}},                         // TRIPLE 380 ms
    {9, {100, 100, 100, 100, 100, 100, 100, 100, 100}},  // FAST, 5 Hz
    {1, {80}},                                         // one step of a SWEEP
};

struct Rgb {
  uint8_t r, g, b;
};
// Two families (decided 2026-10-04). Warnings keep their place and their
// colour: red on the left is the cargo, amber on the right is the box.
// Everything else -- the box saying it is fine, or that it heard you --
// is blue, violet or cyan: colours that do not read as danger.
const Rgb RED = {255, 0, 0};
const Rgb BLUE = {0, 40, 255};
// Little red in it: at the 2 % front cap a violet of 150/255 red comes
// out 3:5 red to blue and reads pink, close to the alarm red (2026-10-04).
const Rgb VIOLET = {60, 0, 255};
const Rgb CYAN = {0, 200, 255};
// Amber is red plus green; the mix depends on the diffuser, to be tuned
// once the case exists.
const Rgb AMBER = {255, 150, 0};

struct Track {
  int pixel;
  Pat pat;
  Rgb c;
  uint16_t delay_ms;
};

// The front row, left to right as a person facing the box sees it.
const int ROW[3] = {LED_CARGO, LED_ALIVE, LED_DEVICE};

// Alarms that are about the goods, and alarms that are about the box.
const uint16_t CARGO_ALARMS =
    (1u << AL_TEMP_HIGH) | (1u << AL_TEMP_LOW) | (1u << AL_DOOR);
const uint16_t DEVICE_ALARMS = (1u << AL_PROBE) | (1u << AL_BATTERY);

const uint32_t STEP_MS = 10;
const uint32_t BOOT_DELAY_MS = 3000;   // let every device be tried once first

volatile int g_cue = -1;               // pending cue; a newer one replaces it
// The hourly budget is kept through deep sleep: forgetting it at every
// wake would hand a box shaking in a truck a fresh budget every time.
RTC_DATA_ATTR uint32_t g_window_until = 0;
RTC_DATA_ATTR uint32_t g_windows[INDICATE_WINDOWS_PER_HOUR];
RTC_DATA_ATTR uint32_t g_windows_magic = 0;
// When the steady status last showed, through deep sleep: on battery it
// is shown every config led_status_s, not at every wake (2026-10-03).
RTC_DATA_ATTR uint32_t g_status_at = 0;
const uint32_t WINDOWS_MAGIC = 0x494E4431;   // "IND1"
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// Runs through deep sleep (timekeep.h), so times kept across one compare.
uint32_t now_ms(void) { return mono_ms(); }

uint32_t length(const Track &t) {
  uint32_t d = t.delay_ms;
  const PatDef &p = PATS[(int)t.pat];
  for (int i = 0; i < p.n; i++) d += p.seg[i];
  return d;
}

bool lit(const Track &t, uint32_t at) {
  if (at < t.delay_ms) return false;
  at -= t.delay_ms;
  const PatDef &p = PATS[(int)t.pat];
  for (int i = 0; i < p.n; i++) {
    if (at < p.seg[i]) return (i % 2) == 0;
    at -= p.seg[i];
  }
  return false;
}

// Plays tracks together, each on its own pixel, and leaves them dark.
bool in_window(void);

void play(const Track *tr, int n) {
  if (n <= 0) return;
  pm_hold(Hold::Indicate, true);     // a pattern cut off by sleep stays lit
  uint32_t total = 0;
  for (int i = 0; i < n; i++) {
    const uint32_t d = length(tr[i]);
    if (d > total) total = d;
  }
  for (uint32_t t = 0; t <= total; t += STEP_MS) {
    for (int i = 0; i < n; i++) {
      const bool on = lit(tr[i], t);
      leds_set(tr[i].pixel, on ? tr[i].c.r : 0, on ? tr[i].c.g : 0,
               on ? tr[i].c.b : 0);
    }
    leds_show();
    vTaskDelay(pdMS_TO_TICKS(STEP_MS));
  }
  for (int i = 0; i < n; i++) leds_set(tr[i].pixel, 0, 0, 0);
  leds_show();
  pm_hold(Hold::Indicate, in_window());
}

void play_one(int pixel, Pat p, Rgb c) {
  const Track t = {pixel, p, c, 0};
  play(&t, 1);
}

void play_row(Pat p, Rgb c) {
  Track t[3];
  for (int i = 0; i < 3; i++) t[i] = {ROW[i], p, c, 0};
  play(t, 3);
}

// SWEEP: left to right across the front, by position. (The design names
// it LED1->2->3, written when LED1 was thought to be on the left; it is on
// the right.)
void sweep(void) {
  // Blue, violet, cyan from left to right.
  static const Rgb COL[3] = {BLUE, VIOLET, CYAN};
  Track t[3];
  for (int i = 0; i < 3; i++) t[i] = {ROW[i], Pat::Step, COL[i], (uint16_t)(i * 80)};
  play(t, 3);
}

// Three beeps for an alarm. The lights are dark while it sounds.
void alarm_sound(void) {
  if (!config().buzzer_enabled) return;
  pm_hold(Hold::Indicate, true);
  for (int i = 0; i < 3; i++) {
    buzzer_beep(150);
    vTaskDelay(pdMS_TO_TICKS(300));
  }
  pm_hold(Hold::Indicate, in_window());
}

bool external_power(void) { return pm_external_power(); }

bool device_fault(const TripStatus &s) {
  if (s.active && (s.alarms_active & DEVICE_ALARMS)) return true;
  int ok, degraded, failed, unknown;
  health_summary(&ok, &degraded, &failed, &unknown);
  if (degraded || failed) return true;
  // A trip whose records cannot be stamped with a time is a box with a
  // problem, even if every sensor is fine (§4.5).
  return s.active && !time_valid();
}

// The steady status, one frame: each light that has something to say
// says it, all at once, and the frame is over in under half a second.
void play_status(const TripStatus &s) {
  Track t[3];
  int n = 0;
  if (s.active && (s.alarms_active & CARGO_ALARMS)) {
    t[n++] = {LED_CARGO, Pat::Double, RED, 0};
  }
  if (s.active) t[n++] = {LED_ALIVE, Pat::Tick, CYAN, 0};
  if (device_fault(s)) t[n++] = {LED_DEVICE, Pat::Triple, AMBER, 0};
  play(t, n);
}

void boot_cue(void) {
  sweep();
  vTaskDelay(pdMS_TO_TICKS(150));
  TripStatus s;
  trip_status(&s);
  if (device_fault(s)) play_one(LED_DEVICE, Pat::Triple, AMBER);
  else play_row(Pat::Blink, CYAN);
}

void open_window(bool counted) {
  const uint32_t t = now_ms();
  portENTER_CRITICAL(&g_mux);
  bool allowed = !counted;
  if (counted && (int32_t)(g_window_until - t) <= 0) {
    // At most N windows in any hour: a box shaking in a truck all day
    // would otherwise blink for its whole battery life.
    for (int i = 0; i < INDICATE_WINDOWS_PER_HOUR; i++) {
      if (!g_windows[i] || t - g_windows[i] >= 3600000) {
        g_windows[i] = t ? t : 1;
        allowed = true;
        break;
      }
    }
  }
  if (allowed) {
    g_window_until = t + (external_power() ? INDICATE_WINDOW_MS : INDICATE_BATTERY_WINDOW_MS);
  }
  portEXIT_CRITICAL(&g_mux);
  // Someone is looking: the box stays up to show them.
  if (allowed) pm_hold(Hold::Indicate, true);
}

bool in_window(void) { return (int32_t)(g_window_until - now_ms()) > 0; }

void task(void *) {
  TripStatus prev;
  if (!pm_warm()) {
    vTaskDelay(pdMS_TO_TICKS(BOOT_DELAY_MS));
    boot_cue();
    trip_status(&prev);
  } else {
    // A wake from sleep is not a boot and gets no boot sweep. It is the
    // moment the steady status is due (on battery that is once a sample
    // period, and the box sleeps the rest), shown once the sample that
    // woke it has been taken -- an alarm it raises shows in this frame.
    for (int i = 0; i < 50 && !pm_is_done(Duty::Trip); i++) vTaskDelay(pdMS_TO_TICKS(100));
    trip_status(&prev);
    // A cargo alarm shows at every wake; the "alive" tick only every
    // led_status_s -- each one costs the LED rail and a third of a second
    // of the chip awake.
    const bool cargo = prev.active && (prev.alarms_active & CARGO_ALARMS);
    if (cargo || external_power() || !g_status_at ||
        now_ms() - g_status_at >= (uint32_t)config().led_status_s * 1000) {
      play_status(prev);
      g_status_at = now_ms() ? now_ms() : 1;
    }
  }
  uint32_t last_status = now_ms();
  pm_done(Duty::Indicate);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));

    const int cue = g_cue;
    if (cue >= 0) {
      g_cue = -1;
      if (cue == (int)Cue::Boot) boot_cue();
      if (cue == (int)Cue::NfcTap) {
        open_window(false);          // a person is holding the box
        play_row(Pat::Blink, VIOLET);
      }
    }

    // Changes in the trip, seen from outside it.
    TripStatus s;
    trip_status(&s);
    if (s.active && !prev.active) {
      play_one(LED_ALIVE, Pat::Triple, CYAN);          // START_TRIP
    } else if (!s.active && prev.active) {
      play_one(LED_ALIVE, Pat::Blink, BLUE);           // STOP_TRIP
    }
    const uint16_t raised = s.alarms_active & (uint16_t)~prev.alarms_active;
    if (s.active && (raised & CARGO_ALARMS)) {
      play_one(LED_CARGO, Pat::Double, RED);           // at once, not next round
      if (!s.acked) alarm_sound();
    } else if (s.active && (raised & DEVICE_ALARMS)) {
      play_one(LED_DEVICE, Pat::Triple, AMBER);
    }
    if (s.acked && !prev.acked && s.alarms_active) {
      buzzer_stop();
      play_one(LED_CARGO, Pat::Blink, BLUE);           // acknowledged
    }
    prev = s;
    if (!in_window()) pm_hold(Hold::Indicate, false);

    // The steady status, as often as it can be afforded.
    const uint32_t t = now_ms();
    const uint32_t period = (external_power() || in_window())
                                ? 1000
                                : (uint32_t)config().sample_period_s * 1000;
    if (t - last_status >= period) {
      play_status(s);
      last_status = now_ms();
      g_status_at = last_status ? last_status : 1;
    }
  }
}

}  // namespace

void indicate_start(void) {
  leds_set_brightness(config().led_bright_pct);
  leds_set_front_brightness(config().led_front_pct);
  if (!pm_warm() || g_windows_magic != WINDOWS_MAGIC) {
    memset(g_windows, 0, sizeof(g_windows));
    g_window_until = 0;
    g_windows_magic = WINDOWS_MAGIC;
  }
  xTaskCreatePinnedToCore(task, "indicate", 3072, nullptr, 2, nullptr, 0);
}

void indicate_cue(Cue c) { g_cue = (int)c; }

void indicate_attention(void) { open_window(true); }
