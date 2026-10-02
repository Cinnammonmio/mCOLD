#include "display.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "epd.h"
#include "flashlog.h"
#include "gnss.h"
#include "record.h"
#include "screens.h"
#include "timekeep.h"
#include "trip.h"

namespace {

const uint32_t PASS_MS = 5000;
const uint32_t FIRST_DRAW_MS = 6000;      // after boot: let readings arrive
const uint32_t SUMMARY_MS = 3600000;      // a closed trip's summary, this long
const uint32_t GNSS_FRESH_MS = 30 * 60000;
const uint16_t CARGO_ALARMS =
    (1u << AL_TEMP_HIGH) | (1u << AL_TEMP_LOW) | (1u << AL_DOOR);

Canvas g_draw;
SemaphoreHandle_t g_epd = nullptr;   // one refresh at a time
char g_sn[16] = "MCOLD";
// Checked by eye on 2026-10-02: 3 is upright on this board (bring-up
// used 1, which shows the frame upside down; the bench also used 3).
volatile int g_rot = 3;
volatile bool g_force = false;
volatile bool g_hold = false;
volatile int32_t g_passkey = -1;      // -1: no pairing in progress
TaskHandle_t g_task = nullptr;
int32_t g_shown_passkey = -1;

PowerStatus g_pwr = {};
bool g_have_pwr = false;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// What is on the glass now, and the state that put it there.
uint32_t g_shown_hash = 0;
uint32_t g_shown_at = 0;              // 0: nothing drawn since boot
bool g_shown_active = false;
uint16_t g_shown_alarms = 0;
bool g_shown_acked = false;

// The trip that just ended, for its summary page.
TripStatus g_closed = {};
uint32_t g_closed_at = 0;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void clock_str(char *out, size_t n) {
  TimeStamp t;
  time_now(&t);
  if (t.quality == TimeSource::None) {
    // No time is "--:--", never 00:00: a frame claiming a time it cannot
    // know is worse than one that admits it.
    snprintf(out, n, "--:--");
    return;
  }
  const time_t local = (time_t)(t.utc_ms / 1000) + config().tz_offset_min * 60;
  struct tm tm;
  gmtime_r(&local, &tm);
  snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

Foot footer_state(const TripStatus &s, const PowerStatus &p, bool have_p) {
  Foot f = {};
  f.trip = s.active;
  f.wifi = false;      // P6
  f.cloud = false;     // P6
  GnssFix fix;
  f.gnss = gnss_last_fix(&fix) && fix.valid && now_ms() - fix.at_ms < GNSS_FRESH_MS;
  f.shock = false;     // no shock alarm until a threshold is set
  f.charging = have_p && p.charger_valid &&
               (p.charge == ChargeState::PreCharge || p.charge == ChargeState::FastCharge);
  f.batt = (have_p && p.cell_valid) ? (int)lroundf(p.soc_percent) : -1;
  LogStats ls;
  flashlog_stats(&ls);
  f.mem = ls.sectors ? (int)((ls.used * 100 + ls.sectors - 1) / ls.sectors) : 0;
  return f;
}

const char *charge_word(ChargeState c) {
  switch (c) {
    case ChargeState::PreCharge:  return "PRE-CHARGE";
    case ChargeState::FastCharge: return "FAST CHARGE";
    case ChargeState::Done:       return "CHARGED";
    default:                      return "NOT CHARGING";
  }
}

// Picks the screen and draws it into g_draw with the given clock text.
void build(const TripStatus &s, const char *clock) {
  PowerStatus p;
  bool have_p;
  portENTER_CRITICAL(&g_mux);
  p = g_pwr;
  have_p = g_have_pwr;
  portEXIT_CRITICAL(&g_mux);
  const Foot f = footer_state(s, p, have_p);

  const int32_t pk = g_passkey;
  if (pk >= 0) {
    // B3: the code goes in the headline, split in threes like the iOS
    // prompt shows it. Whoever can read this is holding the box.
    char code[12];
    snprintf(code, sizeof(code), "%03ld %03ld", (long)(pk / 1000), (long)(pk % 1000));
    scr_takeover(g_draw, g_sn, clock, f, code, "Enter this code on the phone.",
                 "BLUETOOTH PAIRING", false, false);
    return;
  }

  char temp[12];
  if (s.temp_ok) snprintf(temp, sizeof(temp), "%.1f", s.temp_c);
  else snprintf(temp, sizeof(temp), "--");    // never 0.0 for no reading

  if (s.active) {
    if (s.alarms_active & (1u << AL_PROBE)) {
      // B6: the trip goes on, the box needs looking at. Red title, no
      // frame: the goods are not known to be in danger.
      scr_takeover(g_draw, g_sn, clock, f, "SENSOR FAULT",
                   "Temperature probe not reading.", "TRIP CONTINUES|SEE APP",
                   false, true);
      return;
    }
    char lo[12] = "--", hi[12] = "--";
    if (s.have_temp) {
      snprintf(lo, sizeof(lo), "%.1f", s.min_c100 / 100.0);
      snprintf(hi, sizeof(hi), "%.1f", s.max_c100 / 100.0);
    }
    // A2 normal, A3 out of band (red number), A4 alarm (red frame too),
    // A5 no reading (red "--").
    const bool alarm = (s.alarms_active & CARGO_ALARMS) != 0;
    scr_monitor(g_draw, g_sn, clock, f, temp, lo, hi, nullptr,
                !s.temp_ok || s.out_of_band, alarm);
    return;
  }

  if (g_closed_at && now_ms() - g_closed_at < SUMMARY_MS) {
    // C1, drawn after STOP until the box has something newer to say.
    char title[16], range[32], alarms[24], samples[16];
    snprintf(title, sizeof(title), "TRIP %04lu", (unsigned long)g_closed.id);
    if (g_closed.have_temp) {
      snprintf(range, sizeof(range), "%.1f / %.1f ~C", g_closed.min_c100 / 100.0,
               g_closed.max_c100 / 100.0);
    } else {
      snprintf(range, sizeof(range), "no reading");
    }
    if (g_closed.alarms_raised) snprintf(alarms, sizeof(alarms), "%u raised", g_closed.alarms_raised);
    else snprintf(alarms, sizeof(alarms), "none");
    snprintf(samples, sizeof(samples), "%lu", (unsigned long)g_closed.samples);
    const Row rows[] = {{"Samples", samples}, {"Temperature", range}, {"Alarms", alarms}};
    scr_detail(g_draw, g_sn, clock, f, title, "CLOSED", rows, 3);
    return;
  }

  if (have_p && p.charger_valid && p.power_good && p.cell_valid) {
    // A6: on the charger with no trip, the charge is the news.
    char cur[16], cell[16];
    if (p.current_valid) snprintf(cur, sizeof(cur), "%.2f A", p.battery_ma / 1000.0);
    else snprintf(cur, sizeof(cur), "--");
    snprintf(cell, sizeof(cell), "%.2f V", p.cell_volts);
    // The power module names sources in lower case; this font has
    // capitals only.
    char src[16];
    snprintf(src, sizeof(src), "%s", vbus_type_name(p.vbus));
    for (char *q = src; *q; q++) {
      if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    }
    const Row rows[] = {{"SOURCE", src}, {"CURRENT", cur}, {"CELL", cell}};
    scr_charge(g_draw, g_sn, clock, f, (int)lroundf(p.soc_percent),
               charge_word(p.charge), rows, 3);
    return;
  }

  // A1: no trip. The temperature is still shown -- it is still measured.
  scr_monitor(g_draw, g_sn, clock, f, temp, nullptr, nullptr, "NO ACTIVE TRIP",
              !s.temp_ok, false);
}

bool show(const Canvas &c) {
  xSemaphoreTake(g_epd, portMAX_DELAY);
  const bool ok = epd_show(c, g_rot);
  xSemaphoreGive(g_epd);
  return ok;
}

void task(void *) {
  vTaskDelay(pdMS_TO_TICKS(FIRST_DRAW_MS));
  TripStatus prev;
  trip_status(&prev);

  for (;;) {
    TripStatus s;
    trip_status(&s);
    if (prev.active && !s.active) {
      g_closed = prev;           // the last status the running trip had
      g_closed_at = now_ms();
    }
    if (s.active) g_closed_at = 0;
    prev = s;

    if (!g_hold || g_passkey >= 0) {    // a passkey beats a demo page
      // The picture without its clock decides whether anything changed:
      // the minute ticking over is not news.
      build(s, "     ");
      const uint32_t content = g_draw.hash();
      const int32_t pk = g_passkey;
      const bool urgent = s.active != g_shown_active ||
                          s.alarms_active != g_shown_alarms ||
                          s.acked != g_shown_acked || pk != g_shown_passkey;
      const bool due = !g_shown_at ||
                       now_ms() - g_shown_at >= DISPLAY_MIN_S * 1000;
      if (g_force || (content != g_shown_hash && (urgent || due))) {
        char clock[8];
        clock_str(clock, sizeof(clock));
        build(s, clock);
        if (show(g_draw)) {
          g_shown_hash = content;
          g_shown_at = now_ms();
          g_shown_active = s.active;
          g_shown_alarms = s.alarms_active;
          g_shown_acked = s.acked;
          g_shown_passkey = pk;
          printf("[display] refreshed in %lu ms%s\n",
                 (unsigned long)epd_last_refresh_ms(), urgent ? " (state change)" : "");
        } else {
          printf("[display] panel did not answer\n");
        }
        fflush(stdout);
        g_force = false;
      }
    }
    // Woken early by anything that cannot wait a whole pass -- a pairing
    // passkey, which the phone gives the user 30 seconds to type.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PASS_MS));
  }
}

}  // namespace

void display_start(const char *sn) {
  snprintf(g_sn, sizeof(g_sn), "%s", sn ? sn : "MCOLD");
  g_epd = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(task, "display", 4096, nullptr, 1, &g_task, 1);
}

void display_note_power(const PowerStatus &ps) {
  portENTER_CRITICAL(&g_mux);
  g_pwr = ps;
  g_have_pwr = true;
  portEXIT_CRITICAL(&g_mux);
}

void display_refresh(void) {
  g_hold = false;
  g_force = true;
}

void display_hold(bool on) { g_hold = on; }

void display_set_rotation(int r) {
  if (r == 1 || r == 3) g_rot = r;
}

int display_rotation(void) { return g_rot; }

bool display_show(const Canvas &c) { return g_epd ? show(c) : false; }

void display_passkey(uint32_t code) {
  g_passkey = (int32_t)(code % 1000000);
  if (g_task) xTaskNotifyGive(g_task);
}

void display_passkey_clear(void) {
  if (g_passkey < 0) return;
  g_passkey = -1;
  if (g_task) xTaskNotifyGive(g_task);
}
