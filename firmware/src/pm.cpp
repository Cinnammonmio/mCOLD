#include "pm.h"

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "rails.h"
#include "timekeep.h"

namespace {

// Pins with a level that matters while the chip sleeps. A digital pad
// that is not held floats in deep sleep, and these are the ones where a
// float costs something: a rail enable drifting on powers the panel or
// the GNSS all night; a data or select line left high feeds an unpowered
// part through its input protection diodes.
struct Held {
  uint8_t pin;
  uint8_t level;
};
const Held HELD[] = {
    {PIN_EPD_PWR_EN, 0}, {PIN_SD_PWR_EN, 0}, {PIN_LED_PWR_EN, 1},   // rails off
    {PIN_LED_DATA, 0},   {PIN_BUZZER, 0},                           // quiet, dark
    {PIN_SPI3_SCK, 0},   {PIN_SPI3_MOSI, 0}, {PIN_EPD_CS, 0},       // panel and
    {PIN_EPD_DC, 0},     {PIN_EPD_RST, 0},   {PIN_TC_CS, 0},        // probe unpowered
    {PIN_GNSS_TX, 0},                                               // GNSS unpowered
};

const uint32_t ALL_DUTIES = (1u << (int)Duty::Count) - 1;
// A duty that has not reported by then is not going to: its task is
// stuck or was never created. Sleep anyway -- the timer brings it back
// for another try, where staying up would empty the battery in a day.
const uint32_t DUTY_LIMIT_MS = 60000;
// The same reasoning for the box as a whole: on battery, nothing but a
// person (BLE, console, a phone on the tag) keeps it up longer.
const uint32_t AWAKE_LIMIT_MS = 10 * 60000;
// Less than this to the next job and it is cheaper to wait awake than
// to boot again.
const uint32_t MIN_SLEEP_MS = 4000;
// One motion wake a minute. A box being carried would otherwise boot
// every few seconds; the accelerometer latches what happens meanwhile
// and the next wake collects it.
const uint32_t MOTION_REARM_MS = 60000;

// ---- carried through deep sleep (lost on any other reset) ---------------

struct Kept {
  uint32_t magic;
  uint32_t wakes;
  uint32_t slept_at;        // mono_ms() as the chip went down
  uint32_t planned_ms;      // the timer it set
  uint32_t motion_wake_at;  // mono_ms() of the last motion wake, 0: none
};
const uint32_t KEPT_MAGIC = 0x504D4B31;   // "PMK1"
RTC_DATA_ATTR Kept g_kept;

// ---- the record of recent wakes: survives a reset too -------------------
//
// Opening the USB console resets the board, which would wipe RTC_DATA
// and count a new boot; the whole point of this record is to be read
// after the box has been on battery for a while, so it lives in
// RTC_NOINIT, checks itself, and runs until `sleep clear`.

struct WakeRec {
  uint32_t mono_s;          // when it woke, mono_ms()/1000
  uint8_t cause;            // Wake
  uint8_t boot_ms_x10;      // ROM + bootloader time before app_main, /10
  uint16_t awake_ms;        // app_main to sleep
  int16_t ma_x10;           // battery current while awake, mean; INT16_MIN none
  uint16_t slept_s;         // how long the sleep before this wake lasted
  int16_t sleep_ua;         // that sleep's current, uA (config sleep_meas); INT16_MIN none
};
const int TRACE_N = 48;
struct Trace {
  uint32_t magic;
  uint16_t head, count;
  uint32_t wakes;
  uint64_t awake_ms, sleep_ms;
  double awake_mas;         // mA x s spent awake, from the current monitor
  uint32_t awake_ms_measured;  // the part of awake_ms that had a reading
  WakeRec r[TRACE_N];
};
const uint32_t TRACE_MAGIC = 0x50545244;  // "PTRD"
RTC_NOINIT_ATTR Trace g_trace;

Wake g_wake = Wake::Cold;
bool g_warm = false;
uint32_t g_boot_ms = 0;          // estimated time before app_main
uint32_t g_slept_ms = 0;         // the sleep that ended with this wake
uint32_t g_init_mono = 0;

volatile uint32_t g_done = 0;
volatile uint32_t g_holds = 0;
volatile uint32_t g_hold_until = 0;
volatile uint32_t g_next[(int)Duty::Count];
// When each duty first reported and each hold was last let go, in ms of
// this wake: what a long wake was waiting for, printed as it sleeps.
uint32_t g_done_at[(int)Duty::Count];
uint32_t g_hold_off_at[(int)Hold::Count];
double g_ma_sum = 0;
uint32_t g_ma_n = 0;
int16_t g_sleep_ua = INT16_MIN;

const int HOOKS_MAX = 6;
PmHook g_hooks[HOOKS_MAX];
int g_nhooks = 0;
PmHook g_quiet = nullptr;
// After the rails go off and the pins are held, before anything is
// armed: long enough for the supply and the accelerometer to settle.
const uint32_t SETTLE_MS = 40;

const char *const DUTY_NAMES[] = {"sensors", "power", "trip",   "display",
                                  "nfc",     "uplink", "gnss", "indicate"};
const char *const HOLD_NAMES[] = {"ble", "nfc field", "gnss", "uplink", "display", "indicate", "console"};

uint32_t awake_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void trace_check(void) {
  if (g_trace.magic == TRACE_MAGIC && g_trace.head < TRACE_N && g_trace.count <= TRACE_N) {
    return;
  }
  memset(&g_trace, 0, sizeof(g_trace));
  g_trace.magic = TRACE_MAGIC;
}

void trace_put(uint32_t awake) {
  trace_check();
  WakeRec &w = g_trace.r[g_trace.head];
  w.mono_s = g_init_mono / 1000;
  w.cause = (uint8_t)g_wake;
  w.boot_ms_x10 = (uint8_t)(g_boot_ms / 10 > 255 ? 255 : g_boot_ms / 10);
  w.awake_ms = (uint16_t)(awake > 65535 ? 65535 : awake);
  w.ma_x10 = g_ma_n ? (int16_t)(g_ma_sum / g_ma_n * 10) : INT16_MIN;
  w.slept_s = (uint16_t)(g_slept_ms / 1000 > 65535 ? 65535 : g_slept_ms / 1000);
  w.sleep_ua = g_sleep_ua;
  g_trace.head = (uint16_t)((g_trace.head + 1) % TRACE_N);
  if (g_trace.count < TRACE_N) g_trace.count++;
  g_trace.wakes++;
  const uint32_t whole = awake + g_boot_ms;
  g_trace.awake_ms += whole;
  g_trace.sleep_ms += g_slept_ms;
  if (g_ma_n) {
    g_trace.awake_mas += g_ma_sum / g_ma_n * whole / 1000.0;
    g_trace.awake_ms_measured += whole;
  }
}

// Leaves every held pin at the level it will keep through the sleep.
void hold_pins(void) {
  for (const Held &h : HELD) {
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << h.pin;
    c.mode = GPIO_MODE_OUTPUT;
    gpio_config(&c);
    gpio_set_level((gpio_num_t)h.pin, h.level);
    gpio_hold_en((gpio_num_t)h.pin);
  }
  gpio_deep_sleep_hold_en();
}

uint32_t next_wake(uint32_t t, int *who = nullptr);
void report(uint32_t sleep_ms, uint32_t awake, const char *motion, const char *nfc,
            const char *usb);

[[noreturn]] void enter(uint32_t sleep_ms, bool wake_on_events) {
  for (int i = 0; i < g_nhooks; i++) g_hooks[i]();
  for (int r = 0; r < (int)Rail::Count; r++) {
    if (rail_users((Rail)r)) {
      printf("[pm] rail %s still claimed %d time(s) at sleep\n", rail_name((Rail)r),
             rail_users((Rail)r));
    }
  }
  rails_all_off();
  hold_pins();
  vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
  if (g_quiet) g_quiet();

  esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000);
  const char *armed_motion = "no", *armed_nfc = "no", *armed_usb = "no";
  if (wake_on_events) {
    // INT1 is latched: high now means an event nobody has read, and
    // arming on a level that is already there wakes the chip at once.
    const uint32_t t = mono_ms();
    const bool rearm = !g_kept.motion_wake_at || t - g_kept.motion_wake_at >= MOTION_REARM_MS;
    if (rearm && gpio_get_level((gpio_num_t)PIN_ACC_INT1) == 0) {
      esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_ACC_INT1, 1);
      armed_motion = "yes";
    } else if (!rearm) {
      armed_motion = "resting";
    }
    // PG# and the tag's GPO are both active low: one EXT1 group, wake
    // on any low. Each is armed only while it is high, or the chip
    // would wake straight back up (PG# held low by a cable is the case
    // §12 warns about).
    uint64_t mask = 0;
    if (gpio_get_level((gpio_num_t)PIN_PG_N)) {
      mask |= 1ULL << PIN_PG_N;
      armed_usb = "yes";
    }
    const bool gpo_idle = gpio_get_level((gpio_num_t)PIN_NFC_GPO) != 0;
    if (gpo_idle) {
      mask |= 1ULL << PIN_NFC_GPO;
      armed_nfc = "yes";
    }
    if (mask) esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (gpo_idle) {
      // The GPO may be open drain with nothing else pulling it up; the
      // RTC pull-up only works with the RTC peripherals kept powered.
      esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
      rtc_gpio_pullup_en((gpio_num_t)PIN_NFC_GPO);
      rtc_gpio_pulldown_dis((gpio_num_t)PIN_NFC_GPO);
    }
  }

  const uint32_t awake = awake_ms();
  trace_put(awake);
  g_kept.magic = KEPT_MAGIC;
  g_kept.slept_at = mono_ms();
  g_kept.planned_ms = sleep_ms;
  report(sleep_ms, awake, armed_motion, armed_nfc, armed_usb);
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(20));   // let the console drain
  esp_deep_sleep_start();
}

uint32_t next_wake(uint32_t t, int *who) {
  uint32_t best = t + (uint32_t)config().idle_wake_s * 1000;
  if (who) *who = -1;
  for (int d = 0; d < (int)Duty::Count; d++) {
    const uint32_t n = g_next[d];
    if (n && (int32_t)(n - best) < 0) {
      best = n;
      if (who) *who = d;
    }
  }
  return best;
}

// One line on what this wake did, for anyone watching the console.
void report(uint32_t sleep_ms, uint32_t awake, const char *motion, const char *nfc,
            const char *usb) {
  int who;
  next_wake(mono_ms(), &who);
  printf("[pm] sleep %lu s (for %s) after %lu ms awake; wakes: motion %s, tap %s, usb %s\n",
         (unsigned long)(sleep_ms / 1000), who >= 0 ? DUTY_NAMES[who] : "idle",
         (unsigned long)awake, motion, nfc, usb);
  printf("[pm] done at");
  for (int d = 0; d < (int)Duty::Count; d++) {
    printf(" %s %lu", DUTY_NAMES[d], (unsigned long)g_done_at[d]);
  }
  bool any = false;
  for (int h = 0; h < (int)Hold::Count; h++) {
    if (!g_hold_off_at[h]) continue;
    printf("%s %s %lu", any ? "," : "; holds off at", HOLD_NAMES[h],
           (unsigned long)g_hold_off_at[h]);
    any = true;
  }
  printf("\n");
}

void task(void *) {
  uint32_t last_why = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (pm_external_power() || !config().sleep_en) continue;
    const uint32_t t = mono_ms();
    const uint32_t up = awake_ms();

    uint32_t missing = ALL_DUTIES & ~g_done;
    if (missing && up > DUTY_LIMIT_MS) {
      printf("[pm] still waiting on");
      for (int d = 0; d < (int)Duty::Count; d++) {
        if (missing & (1u << d)) printf(" %s", DUTY_NAMES[d]);
      }
      printf(" after %lu s: sleeping without\n", (unsigned long)(DUTY_LIMIT_MS / 1000));
      g_done = ALL_DUTIES;
      missing = 0;
    }
    if (missing) continue;

    const uint32_t people = (1u << (int)Hold::Ble) | (1u << (int)Hold::NfcField) |
                            (1u << (int)Hold::Console) | (1u << (int)Hold::Display);
    const bool held = g_holds || (int32_t)(g_hold_until - t) > 0;
    if (held) {
      // Only something a person is doing outlasts the awake limit.
      if (up < AWAKE_LIMIT_MS || (g_holds & people)) continue;
      if (t - last_why > 60000) {
        last_why = t;
        printf("[pm] awake %lu s on battery; sleeping despite holds 0x%02lX\n",
               (unsigned long)(up / 1000), (unsigned long)g_holds);
      }
      enter(60000, true);
    }

    const int32_t ms = (int32_t)(next_wake(t) - t);
    if (ms < (int32_t)MIN_SLEEP_MS) {
      if (up < AWAKE_LIMIT_MS) continue;
      enter(60000, true);
    }
    enter((uint32_t)ms, true);
  }
}

}  // namespace

void pm_init(void) {
  g_init_mono = mono_ms();
  for (int d = 0; d < (int)Duty::Count; d++) g_next[d] = 0;

  g_warm = esp_reset_reason() == ESP_RST_DEEPSLEEP && g_kept.magic == KEPT_MAGIC;
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: g_wake = Wake::Timer; break;
    case ESP_SLEEP_WAKEUP_EXT0:  g_wake = Wake::Motion; break;
    case ESP_SLEEP_WAKEUP_EXT1: {
      const uint64_t m = esp_sleep_get_ext1_wakeup_status();
      g_wake = (m & (1ULL << PIN_NFC_GPO)) ? Wake::Nfc
             : (m & (1ULL << PIN_PG_N))    ? Wake::Usb
                                           : Wake::Other;
      break;
    }
    case ESP_SLEEP_WAKEUP_UNDEFINED: g_wake = Wake::Cold; break;
    default: g_wake = Wake::Other; break;
  }
  if (!g_warm) {
    g_wake = Wake::Cold;
    memset(&g_kept, 0, sizeof(g_kept));
  } else {
    g_kept.wakes++;
    // How long the chip really slept, and how long ROM and bootloader
    // took before this code ran: both from the RTC timer, which kept
    // counting. A timer wake ends exactly at the planned time, so the
    // rest is boot.
    g_slept_ms = g_init_mono - g_kept.slept_at;
    if (g_wake == Wake::Timer && g_slept_ms > g_kept.planned_ms) {
      g_boot_ms = g_slept_ms - g_kept.planned_ms;
      if (g_boot_ms > 5000) g_boot_ms = 0;
      g_slept_ms = g_kept.planned_ms;
    }
    if (g_wake == Wake::Motion) g_kept.motion_wake_at = g_init_mono ? g_init_mono : 1;
  }

  // Pins held through the sleep: set the level they were held at as the
  // output level first, so letting go does not glitch -- the LED rail
  // enable floating for a moment lights four pixels at full brightness.
  for (const Held &h : HELD) {
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << h.pin;
    c.mode = GPIO_MODE_OUTPUT;
    gpio_config(&c);
    gpio_set_level((gpio_num_t)h.pin, h.level);
    gpio_hold_dis((gpio_num_t)h.pin);
  }
  gpio_deep_sleep_hold_dis();
  // The wake pins come back from sleep as RTC IO; the drivers expect
  // ordinary GPIO.
  const gpio_num_t wake_pins[] = {(gpio_num_t)PIN_NFC_GPO, (gpio_num_t)PIN_ACC_INT1,
                                  (gpio_num_t)PIN_PG_N};
  for (gpio_num_t p : wake_pins) {
    if (rtc_gpio_is_valid_gpio(p)) rtc_gpio_deinit(p);
  }
  gpio_config_t in = {};
  in.pin_bit_mask = 1ULL << PIN_NFC_GPO;
  in.mode = GPIO_MODE_INPUT;
  in.pull_up_en = GPIO_PULLUP_ENABLE;   // see enter(): the GPO may be open drain
  gpio_config(&in);
  in.pin_bit_mask = 1ULL << PIN_PG_N;
  in.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&in);
}

Wake pm_wake(void) { return g_wake; }
bool pm_warm(void) { return g_warm; }
uint32_t pm_wakes(void) { return g_kept.wakes; }

const char *pm_wake_name(Wake w) {
  switch (w) {
    case Wake::Cold:   return "cold boot";
    case Wake::Timer:  return "timer";
    case Wake::Motion: return "motion";
    case Wake::Nfc:    return "nfc tap";
    case Wake::Usb:    return "usb power";
    default:           return "other";
  }
}

bool pm_external_power(void) {
  return gpio_get_level((gpio_num_t)PIN_PG_N) == 0 && !config().sleep_usb;
}

void pm_done(Duty d) {
  if (d >= Duty::Count) return;
  if (!(g_done & (1u << (int)d))) g_done_at[(int)d] = awake_ms();
  g_done = g_done | (1u << (int)d);
}

bool pm_is_done(Duty d) { return d < Duty::Count && (g_done & (1u << (int)d)); }

void pm_hold(Hold h, bool on) {
  if (h >= Hold::Count) return;
  const uint32_t b = 1u << (int)h;
  // Read-modify-write from several tasks: a critical section keeps one
  // task's release from undoing another's claim.
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&mux);
  const bool was = g_holds & b;
  g_holds = on ? (g_holds | b) : (g_holds & ~b);
  portEXIT_CRITICAL(&mux);
  if (was && !on) g_hold_off_at[(int)h] = awake_ms();
}

void pm_hold_until(uint32_t until) {
  if ((int32_t)(until - g_hold_until) > 0) g_hold_until = until;
}

void pm_next(Duty d, uint32_t at) {
  if (d < Duty::Count) g_next[(int)d] = at;
}

void pm_note_current(float ma) {
  g_ma_sum += ma;
  g_ma_n++;
}

void pm_note_sleep_current(float ma) {
  const float ua = ma * 1000.0f;
  g_sleep_ua = (int16_t)(ua > 32767 ? 32767 : ua < -32767 ? -32767 : ua);
}

void pm_on_sleep(PmHook fn) {
  if (fn && g_nhooks < HOOKS_MAX) g_hooks[g_nhooks++] = fn;
}

void pm_on_quiet(PmHook fn) { g_quiet = fn; }

void pm_start(void) {
  trace_check();
  xTaskCreatePinnedToCore(task, "pm", 3072, nullptr, 1, nullptr, 0);
}

void pm_print(void) {
  const uint32_t t = mono_ms();
  printf("\n  this wake      %s, %s, %lu wakes since boot %lu\n", pm_wake_name(g_wake),
         g_warm ? "warm" : "cold", (unsigned long)g_kept.wakes,
         (unsigned long)time_boot_count());
  printf("  sleep          %s%s\n", config().sleep_en ? "allowed" : "off (config sleep_en)",
         pm_external_power() ? ", but USB power is in: staying up" : "");
  printf("  duties done   ");
  for (int d = 0; d < (int)Duty::Count; d++) {
    printf(" %s%s", DUTY_NAMES[d], (g_done & (1u << d)) ? "" : "(waiting)");
  }
  printf("\n  holds         ");
  if (!g_holds) printf(" none");
  for (int h = 0; h < (int)Hold::Count; h++) {
    if (g_holds & (1u << h)) printf(" %s", HOLD_NAMES[h]);
  }
  if ((int32_t)(g_hold_until - t) > 0) {
    printf("  (+%lu s)", (unsigned long)((g_hold_until - t) / 1000));
  }
  printf("\n  next wake      %ld s (idle %ld s)",
         (long)(int32_t)(next_wake(t) - t) / 1000, (long)config().idle_wake_s);
  for (int d = 0; d < (int)Duty::Count; d++) {
    if (g_next[d]) printf(", %s %+ld s", DUTY_NAMES[d], (long)(int32_t)(g_next[d] - t) / 1000);
  }
  printf("\n");

  trace_check();
  const Trace &r = g_trace;
  if (!r.wakes) {
    printf("  no sleeps recorded this boot\n");
    return;
  }
  const double total_s = (double)(r.awake_ms + r.sleep_ms) / 1000.0;
  printf("\n  %lu wakes over %.1f h: awake %.2f %% of the time\n", (unsigned long)r.wakes,
         total_s / 3600.0, total_s > 0 ? r.awake_ms / 10.0 / total_s : 0.0);
  if (r.awake_ms_measured) {
    const double ma_awake = r.awake_mas / (r.awake_ms_measured / 1000.0);
    printf("  awake: %.1f mA mean -> %.3f mA averaged over all time, before sleep current\n",
           ma_awake, total_s > 0 ? ma_awake * r.awake_ms / 1000.0 / total_s : 0.0);
  }
  printf("\n  %-8s %-10s %7s %7s %8s %8s %9s\n", "mono s", "cause", "boot ms", "awake", "mA",
         "slept s", "sleep uA");
  for (int i = 0; i < r.count; i++) {
    const WakeRec &w = r.r[(r.head + TRACE_N - r.count + i) % TRACE_N];
    char ma[12] = "--";
    if (w.ma_x10 != INT16_MIN) snprintf(ma, sizeof(ma), "%.1f", w.ma_x10 / 10.0);
    char su[12] = "--";
    if (w.sleep_ua != INT16_MIN) snprintf(su, sizeof(su), "%d", w.sleep_ua);
    printf("  %-8lu %-10s %7u %7u %8s %8u %9s\n", (unsigned long)w.mono_s,
           pm_wake_name((Wake)w.cause), (unsigned)w.boot_ms_x10 * 10, (unsigned)w.awake_ms,
           ma, (unsigned)w.slept_s, su);
  }
}

void pm_trace_clear(void) {
  g_trace.magic = 0;
  trace_check();
}

void pm_sleep_until_usb(void) {
  for (int i = 0; i < g_nhooks; i++) g_hooks[i]();
  rails_all_off();
  hold_pins();
  vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
  if (gpio_get_level((gpio_num_t)PIN_PG_N)) {
    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_PG_N, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  printf("[pm] asleep until USB power\n");
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(20));
  esp_deep_sleep_start();
}

void pm_sleep_test(uint32_t seconds) {
  if (seconds < 1) seconds = 1;
  enter(seconds * 1000, false);
}
