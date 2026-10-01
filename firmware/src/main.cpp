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
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bus.h"
#include "health.h"
#include "power.h"
#include "rails.h"
#include "rtcclock.h"
#include "temp.h"

namespace {

// ---- heartbeats ------------------------------------------------------
//
// Each supervised task bumps its own counter every pass; the supervisor
// only reads them. A counter that stops is a stuck task, which is a
// different fault from a device that will not answer, and the two want
// different responses -- so they are counted separately rather than
// collapsed into one "something is wrong".
enum class Job : uint8_t { Sensors = 0, Power, Console, Count };

struct Beat {
  volatile uint32_t count;
  uint32_t last_seen;
  uint32_t stall_ms;      // how long one pass may legitimately take
  const char *name;
};

Beat g_beats[(int)Job::Count] = {
    {0, 0, 10000, "sensors"},
    {0, 0, 30000, "power"},
    {0, 0, 5000, "console"},
};

inline void beat(Job j) { g_beats[(int)j].count++; }

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// The latest reading, and whether it is one. These are separate on
// purpose: a stale value with a flag beside it is honest, a stale
// value on its own is a lie that looks like data.
float g_temp_c = 0;
bool g_temp_valid = false;
TempStatus g_temp_status = TempStatus::NoData;
PowerStatus g_power = {};

// ---- sensors ---------------------------------------------------------

void task_sensors(void *) {
  for (;;) {
    beat(Job::Sensors);
    const uint32_t t = now_ms();

    // Each device is asked only when the health registry says it is
    // worth asking. A failed part is retried on a slow schedule, so it
    // costs one transaction a minute rather than one per pass -- and
    // more to the point, it does not slow down the parts that work.
    if (health_should_try(Dev::Accel, t)) {
      uint8_t who = 0;
      i2c_read_reg(Dev::Accel, ADDR_LIS2DW12, 0x0F, &who, 1);
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
    vTaskDelay(pdMS_TO_TICKS(5000));
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
        if (!strcmp(line, "health")) print_health();
        else if (!strcmp(line, "tasks")) print_tasks();
        else if (*line) printf("  unknown: %s   (health, tasks)\n", line);
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
  rtc_begin();
  power_init();

  printf("\n\nmCOLD Foam V.1   reset %d   heap %u B\n", (int)esp_reset_reason(),
         (unsigned)esp_get_free_heap_size());
  printf("type: health, tasks\n");
  fflush(stdout);

  // Stacks are generous for now and will be trimmed to measured high
  // water marks once these tasks do their real work. Guessing them
  // small this early buys nothing and costs an overflow that presents
  // as a random crash somewhere else entirely.
  xTaskCreatePinnedToCore(task_sensors, "sensors", 4096, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(task_power, "power", 4096, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(task_console, "console", 4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(task_supervisor, "super", 3072, nullptr, 6, nullptr, 0);

  // app_main returns and the tasks it created carry on. Nothing is left
  // here to become the place where work quietly accumulates.
}
