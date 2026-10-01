// mCOLD Foam V.1 -- product firmware.
//
// Every piece of work belongs to a task created here on purpose, with
// its own stack, its own priority and its own failure handling.
//
// That shape exists for one reason. A cold-chain box that stops logging
// temperature because the GNSS module went quiet has failed at its job;
// a box that logs temperature for a week and reports "no position" has
// done it. So no task waits on another, no task holds a bus across a
// long operation, and no device failure propagates past the task that
// owns it.
//
// The supervisor watches heartbeats and reports what is wrong. It does
// not restart anything: a box that reboots loses its state and its
// time, and a reboot loop costs far more than a sensor that limps.
#include <driver/usb_serial_jtag.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "board.h"
#include "bus.h"
#include "buzzer.h"
#include "chargeled.h"
#include "config.h"
#include "gnss.h"
#include "health.h"
#include "leds.h"
#include "nfc.h"
#include "power.h"
#include "rails.h"
#include "rtcclock.h"
#include "temp.h"
#include "timekeep.h"

namespace {

// ---- heartbeats ------------------------------------------------------
//
// Each supervised task bumps its own counter every pass; the supervisor
// only reads them. A counter that stops is a stuck task, which is a
// different fault from a device that will not answer, and the two want
// different responses -- so they are counted separately rather than
// collapsed into one "something is wrong".
enum class Job : uint8_t { Sensors = 0, Power, Gnss, Nfc, Console, Count };

struct Beat {
  volatile uint32_t count;
  uint32_t last_seen;
  uint32_t stall_ms;      // how long one pass may legitimately take
  const char *name;
};

Beat g_beats[(int)Job::Count] = {
    {0, 0, 10000, "sensors"},
    {0, 0, 30000, "power"},
    {0, 0, 10000, "gnss"},
    {0, 0, 5000, "nfc"},
    {0, 0, 5000, "console"},
};

inline void beat(Job j) { g_beats[(int)j].count = g_beats[(int)j].count + 1; }

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// The latest reading, and whether it is one. These are separate on
// purpose: a stale value with a flag beside it is honest, a stale
// value on its own is a lie that looks like data.
float g_temp_c = 0;
bool g_temp_valid = false;
TempStatus g_temp_status = TempStatus::NoData;
PowerStatus g_power = {};
bool g_accel_up = false;
AccelSample g_accel = {};
bool g_accel_valid = false;
uint32_t g_motion_blanked = 0;

// How long after a beep motion events are still the beep's own.
const uint32_t BUZZER_BLANK_MS = 250;

// Periods. P1 values: short enough to see each driver work on the
// bench, and nothing here is yet the sampling policy of a trip (P3).
const uint32_t SENSOR_PASS_MS = 5000;
const uint32_t GNSS_SESSION_MS = 120000;   // give up on a fix after this
const uint32_t GNSS_PERIOD_MS = 600000;    // between sessions
const int GNSS_FIXES_WANTED = 5;
const uint32_t NFC_POLL_MS = 300;

// ---- sensors ---------------------------------------------------------

void report_motion(const AccelEvent &ev, const char *when) {
  printf("[accel] %lu ms%s%s%s%s%s  (src 0x%02X, event %lu)\n",
         (unsigned long)now_ms(), when,
         ev.free_fall ? " free-fall" : " motion", ev.x ? " X" : "",
         ev.y ? " Y" : "", ev.z ? " Z" : "", ev.raw_src,
         (unsigned long)accel_event_count());
  fflush(stdout);
  // P1 verification aid: a person tapping the board sees the answer on
  // the board, with no serial timing to get wrong. The P4 pattern layer
  // replaces this.
  leds_pulse(LED_ALIVE, 0, 255, 0, 120);
}

void task_sensors(void *) {
  accel_notify_task(xTaskGetCurrentTaskHandle());
  uint32_t last_pass = 0;
  bool first = true;

  for (;;) {
    // Wakes on the accelerometer's INT1 edge or after the pass period,
    // whichever is first. A motion event is handled within
    // milliseconds instead of waiting out the rest of a sleep.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SENSOR_PASS_MS));
    beat(Job::Sensors);
    const uint32_t t = now_ms();

    // Every wake, and every pass: a latched source with no edge to
    // announce it would otherwise sit there forever with INT1 held high.
    if (g_accel_up) {
      AccelEvent ev;
      if (accel_take_event(&ev)) {
        // The buzzer trips the detector by itself. An event during a
        // beep, or just after one, is counted but not believed. A real
        // knock in that window is lost with it, which is the lesser
        // error: the alternative logs every alarm beep as a shock.
        if (buzzer_recent(BUZZER_BLANK_MS)) g_motion_blanked++;
        else report_motion(ev, "");
      }
    }

    if (!first && t - last_pass < SENSOR_PASS_MS) continue;
    first = false;
    last_pass = t;

    // Each device is asked only when the health registry says it is
    // worth asking. A failed part is retried on a slow schedule, so it
    // costs one transaction a minute rather than one per pass -- and
    // more to the point, it does not slow down the parts that work.
    if (health_should_try(Dev::Accel, t)) {
      if (!g_accel_up) {
        AccelEvent pending;
        g_accel_up = accel_begin((uint8_t)config().accel_wake_ths, &pending);
      }
      AccelSample a;
      if (g_accel_up && accel_read(&a)) {
        g_accel = a;
        g_accel_valid = true;
      } else {
        g_accel_valid = false;
      }
    }
    if (health_should_try(Dev::Thermo, t)) {
      float c = 0;
      const TempStatus ts = temp_sample(&c);
      if (ts == TempStatus::Ok) {
        g_temp_c = c;
        g_temp_valid = true;
      } else {
        // Anything other than a conversion leaves the last value
        // alone and marks it stale. Nothing downstream is allowed to
        // see a number that was not measured.
        g_temp_valid = false;
      }
      g_temp_status = ts;
    }
  }
}

// ---- gnss ------------------------------------------------------------
//
// Sessions, not a stream: the module draws tens of milliamps while it
// searches, so it is powered for a bounded window, kept only until it
// has a fix, and switched off. How often, and whether motion should
// trigger one, is trip policy (P3); these numbers only keep P1 honest
// about power and give the driver something real to do.

void task_gnss(void *) {
  for (;;) {
    beat(Job::Gnss);
    if (!health_should_try(Dev::Gnss, now_ms()) || !gnss_power_on()) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    printf("[gnss] %lu ms session start\n", (unsigned long)now_ms());
    fflush(stdout);

    const uint32_t start = now_ms();
    int fixes = 0;
    while (now_ms() - start < GNSS_SESSION_MS) {
      beat(Job::Gnss);
      gnss_pump(1000);

      GnssFix f;
      if (!gnss_last_fix(&f) || !f.valid || now_ms() - f.at_ms > 2000) continue;
      fixes++;

      // The RTC has no backup cell, so after any power loss its time is
      // gone. Satellite time is the most trustworthy source this box
      // has; the first status-A sentence puts the clock back.
      if (!rtc_time_valid() && f.time_valid) {
        if (time_set(&f.utc, TimeSource::Gnss, false)) {
          printf("[gnss] clock set from satellite time\n");
          fflush(stdout);
        }
      }
      // A few consecutive fixes rather than the first: the first one
      // out of a cold start is the least accurate the module produces.
      if (fixes >= GNSS_FIXES_WANTED) break;
    }
    gnss_power_off();

    GnssFix f;
    const bool have = gnss_last_fix(&f);
    if (have && f.valid && now_ms() - f.at_ms < 5000) {
      printf("[gnss] fix %.6f, %.6f  %u sats  hdop %.1f\n", f.lat_deg,
             f.lon_deg, f.sats, f.hdop);
    } else {
      GnssSky k;
      gnss_sky(&k);
      printf("[gnss] no fix this session (%s): %u satellites in view, %u"
             " heard, best SNR %u dB-Hz\n",
             health_state(Dev::Gnss) == DevState::Ok ? "module talking"
                                                     : "module silent",
             k.in_view, k.heard, k.best_snr);
    }
    fflush(stdout);

    for (uint32_t waited = 0; waited < GNSS_PERIOD_MS; waited += 5000) {
      beat(Job::Gnss);
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

// ---- nfc -------------------------------------------------------------

void task_nfc(void *) {
  bool up = false;
  for (;;) {
    beat(Job::Nfc);
    const uint32_t t = now_ms();
    if (!up && health_should_try(Dev::Nfc, t)) up = nfc_begin();

    if (up) {
      NfcPoll p;
      nfc_poll(&p);
      if (p.arrived) {
        printf("[nfc] phone on the tag (%s), tap %lu\n",
               p.field == NfcField::RfBusy ? "I2C refused" : "field",
               (unsigned long)nfc_tap_count());
        fflush(stdout);
        // P1 verification aid, as with motion. BLE comes up here in P5.
        leds_pulse(LED_DEVICE, 0, 0, 255, 120);
      }
    }
    // Edges latch in the tag, so a slow poll still catches a quick tap.
    vTaskDelay(pdMS_TO_TICKS(NFC_POLL_MS));
  }
}

// ---- power -----------------------------------------------------------

void task_power(void *) {
  uint32_t last_kick = 0;
  for (;;) {
    beat(Job::Power);
    const uint32_t t = now_ms();

    PowerStatus ps;
    power_read(&ps);
    g_power = ps;
    chargeled_update(ps);

    // Kicked on its own clock, not once per read loop: the charger
    // gives about forty seconds and the sampling period may grow a
    // long way beyond that once this box starts sleeping between
    // samples.
    if (ps.charger_valid && (uint32_t)(t - last_kick) >= POWER_WATCHDOG_MS) {
      power_kick_watchdog();
      last_kick = t;
    }

    if (health_should_try(Dev::Rtc, t)) {
      struct tm tmv;
      rtc_get(&tmv);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ---- console ---------------------------------------------------------

void print_health(void) {
  printf("\n  %-9s %-9s %7s %7s %10s\n", "device", "state", "ok", "fail",
         "last ok");
  for (int i = 0; i < (int)Dev::Count; i++) {
    const Dev d = (Dev)i;
    const DevHealth *h = health_get(d);
    if (!h) continue;
    char age[16] = "never";
    if (h->last_ok_ms) {
      snprintf(age, sizeof(age), "%lus",
               (unsigned long)((now_ms() - h->last_ok_ms) / 1000));
    }
    printf("  %-9s %-9s %7lu %7lu %10s\n", health_name(d),
           health_state_name(h->state), (unsigned long)h->ok_count,
           (unsigned long)h->fail_count, age);
  }
  if (g_temp_valid) {
    printf("\n  temperature  %.2f C\n", g_temp_c);
  } else {
    printf("\n  temperature  -- (%s)\n",
           temp_status_name(g_temp_status));
  }
  struct tm tmv;
  if (rtc_get(&tmv)) {
    printf("  clock        %04d-%02d-%02d %02d:%02d:%02d  source %s%s\n",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
           tmv.tm_min, tmv.tm_sec, rtc_source_name(rtc_source()),
           rtc_time_valid() ? "" : "  <- NOT USABLE for timestamps");
  } else {
    printf("  clock        no answer\n");
  }
  TimeStamp ts;
  time_now(&ts);
  printf("  order        boot %lu, %lu ms\n", (unsigned long)ts.boot,
         (unsigned long)ts.tick_ms);

  const PowerStatus &p = g_power;
  if (p.cell_valid) {
    printf("  cell         %.3f V  %.1f %%  %+.2f %%/hr\n", p.cell_volts,
           p.soc_percent, p.rate_percent_hr);
  } else {
    printf("  cell         -- (fuel gauge did not answer)\n");
  }
  if (p.current_valid) {
    printf("  battery      %+.1f mA  %s\n", p.battery_ma,
           p.battery_ma >= 0 ? "into the cell" : "out of the cell");
  } else {
    printf("  battery      -- (current monitor did not answer)\n");
  }
  if (p.charger_valid) {
    printf("  charger      %s, input %s, power-good %d\n",
           charge_state_name(p.charge), vbus_type_name(p.vbus), p.power_good);
    printf("  side light   %s\n", chargeled_name(chargeled_mode()));
    if (p.watchdog_expired) {
      printf("               WATCHDOG expired: the charger reset its own"
             " registers\n");
    }
  } else {
    printf("  charger      -- (did not answer)\n");
  }

  int ok, deg, fail, unk;
  health_summary(&ok, &deg, &fail, &unk);
  printf("\n  %d ok, %d degraded, %d failed, %d not tried\n", ok, deg, fail,
         unk);
  for (int i = 0; i < (int)Rail::Count; i++) {
    const Rail r = (Rail)i;
    printf("  rail %-8s %s  %d holder%s\n", rail_name(r),
           rail_is_on(r) ? "on " : "off", rail_users(r),
           rail_users(r) == 1 ? "" : "s");
  }
  printf("\n");
  fflush(stdout);
}

void print_tasks(void) {
  printf("\n");
  for (int i = 0; i < (int)Job::Count; i++) {
    printf("  %-9s %8lu passes\n", g_beats[i].name,
           (unsigned long)g_beats[i].count);
  }
  printf("  free heap %u B, lowest ever %u B\n",
         (unsigned)esp_get_free_heap_size(),
         (unsigned)esp_get_minimum_free_heap_size());
  printf("\n");
  fflush(stdout);
}

void print_version(void) {
  const esp_app_desc_t *app = esp_app_get_description();
  char sha[17];
  esp_app_get_elf_sha256(sha, sizeof(sha));
  printf("\n  firmware   %s\n", app->version);
  printf("  esp-idf    %s\n", app->idf_ver);
  printf("  elf sha256 %s...\n\n", sha);
}

void print_accel(void) {
  AccelSample a;
  if (g_accel_up && accel_read(&a)) {
    printf("\n  x %+8.1f  y %+8.1f  z %+8.1f mg   |a| %.0f mg\n", a.x_mg,
           a.y_mg, a.z_mg, a.magnitude_mg);
    printf("  (at rest |a| should be close to 1000 whatever the orientation)\n");
  } else {
    printf("\n  accelerometer did not answer\n");
  }
  const uint32_t last = accel_last_event_ms();
  printf("  %lu motion events (%lu during the buzzer, ignored)",
         (unsigned long)(accel_event_count() - g_motion_blanked),
         (unsigned long)g_motion_blanked);
  if (last) printf(", last %lu s ago", (unsigned long)((now_ms() - last) / 1000));
  printf("   INT1 %s\n\n", accel_int_level() ? "HIGH" : "low");
}

void print_gnss(void) {
  GnssStats st;
  gnss_stats(&st);
  printf("\n  module     %s, %lu sessions\n", gnss_is_on() ? "on" : "off",
         (unsigned long)st.sessions);
  printf("  received   %lu bytes, %lu good sentences, %lu bad checksums\n",
         (unsigned long)st.bytes, (unsigned long)st.sentences,
         (unsigned long)st.bad_checksum);
  if (st.bad_checksum > st.sentences) {
    printf("             more bad than good: suspect the baud rate\n");
  }
  GnssSky k;
  gnss_sky(&k);
  if (k.at_ms) {
    printf("  sky        %u in view, %u heard, best SNR %u dB-Hz  (%lu s ago)\n",
           k.in_view, k.heard, k.best_snr,
           (unsigned long)((now_ms() - k.at_ms) / 1000));
    printf("             a fix generally needs four or more heard above"
           " ~30 dB-Hz\n");
  }
  GnssFix f;
  if (!gnss_last_fix(&f)) {
    printf("  position   none yet\n\n");
    return;
  }
  printf("  position   %.6f, %.6f   %u sats  hdop %.1f\n", f.lat_deg, f.lon_deg,
         f.sats, f.hdop);
  printf("             %lu s old -- the last known position, not the"
         " current one\n\n",
         (unsigned long)((now_ms() - f.at_ms) / 1000));
}

void device_sn(char *out, size_t n) {
  // Bring-up's format. iOS cannot see a BLE MAC, so this name is how the
  // app finds the box after reading the tag; the MAC is only a tiebreak.
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(out, n, "MCOLD-%02X%02X", m[4], m[5]);
}

void print_nfc(void) {
  uint8_t uid[8];
  if (!nfc_uid(uid)) {
    printf("\n  tag did not answer\n\n");
    return;
  }
  printf("\n  UID        ");
  for (int i = 0; i < 8; i++) printf("%02X", uid[i]);
  printf("   IC_REF 0x%02X\n", nfc_ic_ref());
  printf("  taps       %lu since boot\n", (unsigned long)nfc_tap_count());
  uint8_t b[48];
  if (nfc_read_user(0, b, sizeof(b))) {
    printf("  memory     ");
    for (size_t i = 0; i < sizeof(b); i++) {
      printf("%c", b[i] >= 32 && b[i] < 127 ? (char)b[i] : '.');
    }
    printf("\n");
  }
  printf("\n");
}

void nfc_write_sn(void) {
  char sn[24];
  device_sn(sn, sizeof(sn));
  uint8_t rec[64];
  const size_t n = ndef_text_record(sn, rec, sizeof(rec));
  if (n && nfc_write_ndef(rec, n)) {
    printf("  wrote and verified a Text record \"%s\"\n", sn);
  } else {
    printf("  write FAILED at a block that did not verify; the tag may now"
           " hold a partial record\n");
  }
}

void print_help(void) {
  printf("\n  health          every device, its state, and the readings\n");
  printf("  tasks           heartbeats and heap\n");
  printf("  version         firmware version and image hash\n");
  printf("  accel           XYZ now, and the motion event count\n");
  printf("  gnss            receiver statistics and last fix\n");
  printf("  nfc             tag identity, taps, first bytes of memory\n");
  printf("  nfc write       write the SN as an NDEF Text record\n");
  printf("  led I R G B     light pixel I (0-3) for a second\n");
  printf("  ledtest [S] [N] side red, left green, middle blue, right white;\n");
  printf("                  S seconds each (4), N rounds (1)\n");
  printf("  beep MS         sound the buzzer\n");
  printf("  config          every setting, its range and default\n");
  printf("  config set K V  change one setting (stored in NVS)\n");
  printf("  config reset    every setting back to its default\n");
  printf("  reboot          restart the firmware\n\n");
}

// Walks the pixels in physical order, so a person watching can check
// position and colour together without knowing the chain order.
void led_test(int hold_s, int rounds) {
  if (hold_s < 1) hold_s = 1;
  if (hold_s > 10) hold_s = 10;
  if (rounds < 1) rounds = 1;
  if (rounds > 5) rounds = 5;
  struct Step { int idx; const char *where; uint8_t r, g, b; const char *colour; };
  static const Step S[] = {
      {LED_SIDE, "side", 255, 0, 0, "RED"},
      {LED_CARGO, "front left", 0, 255, 0, "GREEN"},
      {LED_ALIVE, "front middle", 0, 0, 255, "BLUE"},
      {LED_DEVICE, "front right", 255, 255, 255, "WHITE"},
  };
  for (int r = 0; r < rounds; r++) {
    for (const Step &s : S) {
      printf("  %-13s (index %d)  %s\n", s.where, s.idx, s.colour);
      fflush(stdout);
      // Held by renewing a short pulse each second, so the light is
      // still bounded by the pulse limit if this task were to stall.
      for (int k = 0; k < hold_s; k++) {
        leds_pulse(s.idx, s.r, s.g, s.b, 1200);
        vTaskDelay(pdMS_TO_TICKS(1000));
        beat(Job::Console);   // longer than the console's stall limit
      }
      vTaskDelay(pdMS_TO_TICKS(1200));   // dark gap between steps
    }
  }
}

void run_command(char *line) {
  if (!strcmp(line, "health")) print_health();
  else if (!strcmp(line, "tasks")) print_tasks();
  else if (!strcmp(line, "version")) print_version();
  else if (!strcmp(line, "accel")) print_accel();
  else if (!strcmp(line, "gnss")) print_gnss();
  else if (!strcmp(line, "nfc")) print_nfc();
  else if (!strcmp(line, "nfc write")) nfc_write_sn();
  else if (!strcmp(line, "help")) print_help();
  else if (!strcmp(line, "reboot")) {
    printf("  restarting\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
  } else if (!strcmp(line, "config")) config_print();
  else if (!strcmp(line, "config reset")) {
    config_reset();
    printf("  every setting back to its default\n");
  } else if (!strncmp(line, "config set ", 11)) {
    char key[24];
    long v;
    if (sscanf(line + 11, "%23s %ld", key, &v) != 2) {
      printf("  config set KEY VALUE\n");
    } else if (!config_set(key, (int32_t)v)) {
      printf("  refused: unknown key or out of range (see 'config')\n");
    } else {
      printf("  %s = %ld, stored\n", key, v);
      // The one setting that has a consumer already.
      if (!strcmp(key, "accel_wake_ths") && g_accel_up) {
        accel_set_threshold((uint8_t)v);
      }
    }
  } else if (!strncmp(line, "ledtest", 7)) {
    int hold = 4, rounds = 1;
    sscanf(line + 7, "%d %d", &hold, &rounds);
    led_test(hold, rounds);
  }
  else if (!strncmp(line, "led ", 4)) {
    int i, r, g, b;
    if (sscanf(line + 4, "%d %d %d %d", &i, &r, &g, &b) == 4 && i >= 0 &&
        i < LED_COUNT) {
      leds_pulse(i, (uint8_t)r, (uint8_t)g, (uint8_t)b, 1000);
    } else {
      printf("  led I R G B   (I 0..3, colours 0..255)\n");
    }
  } else if (!strncmp(line, "beep ", 5)) {
    buzzer_beep((uint32_t)atoi(line + 5));
  } else if (*line) {
    printf("  unknown: %s   (help)\n", line);
  }
}

void task_console(void *) {
  char line[64];
  int n = 0;
  char prev = 0;
  uint8_t c;
  for (;;) {
    beat(Job::Console);
    while (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(20)) == 1) {
      if (c == '\n' && prev == '\r') { prev = (char)c; continue; }
      prev = (char)c;
      if (c == '\r' || c == '\n') {
        printf("\n");
        line[n] = 0;
        n = 0;
        run_command(line);
        fflush(stdout);
      } else if (c == 8 || c == 127) {
        if (n > 0) { n--; printf("\b \b"); fflush(stdout); }
      } else if (c >= ' ' && n < (int)sizeof(line) - 1) {
        line[n++] = (char)c;
        putchar(c);
        fflush(stdout);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---- supervisor -------------------------------------------------------

void task_supervisor(void *) {
  uint32_t last[(int)Job::Count] = {0};
  for (int i = 0; i < (int)Job::Count; i++) g_beats[i].last_seen = now_ms();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    const uint32_t t = now_ms();
    for (int i = 0; i < (int)Job::Count; i++) {
      const uint32_t c = g_beats[i].count;
      if (c != last[i]) {
        last[i] = c;
        g_beats[i].last_seen = t;
        continue;
      }
      const uint32_t stuck = t - g_beats[i].last_seen;
      if (stuck > g_beats[i].stall_ms) {
        printf("[supervisor] %s has not run for %lu ms\n", g_beats[i].name,
               (unsigned long)stuck);
        fflush(stdout);
      }
    }
  }
}

}  // namespace

extern "C" void app_main(void) {
  usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&ucfg);
  vTaskDelay(pdMS_TO_TICKS(300));

  health_init();
  rails_init();
  bus_init();
  leds_init();
  buzzer_init();
  chargeled_start();
  config_init();
  rtc_begin();
  time_init();
  power_init();

  // Armed here, not by a console command: opening and closing the
  // serial port resets the board, so anything a command switches on is
  // gone before anyone can tap the box. Bring-up lost three motion
  // tests to that before it was understood.
  AccelEvent pending;
  g_accel_up = accel_begin((uint8_t)config().accel_wake_ths, &pending);

  const esp_app_desc_t *app = esp_app_get_description();
  printf("\n\nmCOLD Foam V.1   firmware %s   reset %d   heap %u B\n",
         app->version, (int)esp_reset_reason(),
         (unsigned)esp_get_free_heap_size());
  if (pending.wake || pending.free_fall) report_motion(pending, " before boot:");
  printf("type: help\n");
  fflush(stdout);

  // Stacks are generous for now and will be trimmed to measured high
  // water marks once these tasks do their real work. Guessing them
  // small this early buys nothing and costs an overflow that presents
  // as a random crash somewhere else entirely.
  xTaskCreatePinnedToCore(task_sensors, "sensors", 4096, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(task_power, "power", 4096, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(task_gnss, "gnss", 4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(task_nfc, "nfc", 3072, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(task_console, "console", 6144, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(task_supervisor, "super", 3072, nullptr, 6, nullptr, 0);

  // app_main returns and the tasks it created carry on. Nothing is left
  // here to become the place where work quietly accumulates.
}
