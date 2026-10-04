#include "display.h"

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "config.h"
#include "epd.h"
#include "flashlog.h"
#include "gnss.h"
#include "record.h"
#include "screens.h"
#include "timekeep.h"
#include "net.h"
#include "pm.h"
#include "trip.h"
#include "uplink.h"

namespace {

const uint32_t PASS_MS = 5000;
const uint32_t FIRST_DRAW_MS = 6000;      // after boot: let readings arrive
const uint32_t SUMMARY_MS = 3600000;      // a closed trip's summary, this long
const uint32_t GNSS_FRESH_MS = 30 * 60000;
const uint32_t CLOUD_FRESH_MS = 10 * 60000;
// Icon and USB changes redraw at once, but not more often than this: a
// Wi-Fi link at the edge of range can drop and return every few seconds,
// and every redraw is seconds of panel current.
const uint32_t ICON_REDRAW_GAP_MS = 30000;
const uint16_t CARGO_ALARMS =
    (1u << AL_TEMP_HIGH) | (1u << AL_TEMP_LOW) | (1u << AL_DOOR);

// In PSRAM: 7.6 KB that only the CPU touches, and internal RAM is short.
EXT_RAM_BSS_ATTR Canvas g_draw;
SemaphoreHandle_t g_epd = nullptr;   // one refresh at a time
char g_sn[SN_LEN] = "MCOLD";
// Checked by eye on 2026-10-02: 3 is upright on this board (bring-up
// used 1, which shows the frame upside down; the bench also used 3).
volatile int g_rot = 3;
volatile bool g_force = false;
volatile bool g_hold = false;

PowerStatus g_pwr = {};
bool g_have_pwr = false;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// What is on the glass now, and the state that put it there. Kept
// through deep sleep -- the panel keeps its picture with no power, and
// a box that forgot what it showed would redraw on every wake, seconds
// of panel current every five minutes for nothing.
RTC_DATA_ATTR uint32_t g_shown_hash = 0;
RTC_DATA_ATTR uint32_t g_shown_at = 0;        // 0: nothing drawn since boot
RTC_DATA_ATTR bool g_shown_active = false;
RTC_DATA_ATTR uint16_t g_shown_alarms = 0;
RTC_DATA_ATTR bool g_shown_acked = false;
RTC_DATA_ATTR uint8_t g_shown_icons = 0xFF;   // footer-left icons + USB power, as bits
RTC_DATA_ATTR uint32_t g_icons_drawn_at = 0;

// The trip that just ended, for its summary page.
RTC_DATA_ATTR TripStatus g_closed = {};
RTC_DATA_ATTR uint32_t g_closed_at = 0;

// Runs through deep sleep (timekeep.h), so times kept across one compare.
uint32_t now_ms(void) { return mono_ms(); }

void clock_str(char *out, size_t n) {
  TimeStamp t;
  time_now(&t);
  if (t.quality == TimeSource::None) {
    // No time is dashes, never 00:00: a frame claiming a time it cannot
    // know is worse than one that admits it.
    snprintf(out, n, "--/-- --:--");
    return;
  }
  const time_t local = (time_t)(t.utc_ms / 1000) + config().tz_offset_min * 60;
  struct tm tm;
  gmtime_r(&local, &tm);
  // Day/month and time (2026-10-04): a box looked at after a long trip
  // should say which day its picture is from.
  snprintf(out, n, "%02d/%02d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
}

Foot footer_state(const TripStatus &s, const PowerStatus &p, bool have_p) {
  Foot f = {};
  f.trip = s.active;
  NetStatus ns;
  net_status(&ns);
  // On battery Wi-Fi is up only for a moment each upload period, and the
  // picture is drawn after that moment: there the icon means "the last
  // session got through", or it would never show.
  const uint32_t recent_ms = 2 * (uint32_t)config().upload_period_s * 1000 + 60000;
  const bool wifi_recent = !pm_external_power() && ns.last_up_ms &&
                           now_ms() - ns.last_up_ms < recent_ms;
  f.wifi = ns.connected || wifi_recent;
  // The cloud means the SERVER has the data, not that the broker is up
  // (§9.1: online means a server that confirms receipt). So: broker
  // reached, and either nothing waiting or an ACK in the last 10 min.
  UplinkStatus us;
  uplink_status(&us);
  const uint32_t fresh_ms = wifi_recent ? recent_ms : CLOUD_FRESH_MS;
  f.cloud = (us.connected || (wifi_recent && us.last_session_ok)) &&
            (us.records_pending == 0 ||
             (us.last_ack_ms && now_ms() - us.last_ack_ms < fresh_ms));
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

// The footer's left-hand icons and USB power, one bit each.
const uint8_t ICON_TRIP = 1, ICON_WIFI = 2, ICON_CLOUD = 4, ICON_GNSS = 8,
              ICON_SHOCK = 16, ICON_USB = 32;

uint8_t icons_now(const TripStatus &s) {
  PowerStatus p;
  bool have;
  portENTER_CRITICAL(&g_mux);
  p = g_pwr;
  have = g_have_pwr;
  portEXIT_CRITICAL(&g_mux);
  const Foot f = footer_state(s, p, have);
  // USB power straight from the charger's PG# pin: the power task reads
  // the charger only every five seconds.
  const bool usb = gpio_get_level((gpio_num_t)PIN_PG_N) == 0;
  return (uint8_t)((f.trip ? ICON_TRIP : 0) | (f.wifi ? ICON_WIFI : 0) |
                   (f.cloud ? ICON_CLOUD : 0) | (f.gnss ? ICON_GNSS : 0) |
                   (f.shock ? ICON_SHOCK : 0) | (usb ? ICON_USB : 0));
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
  if (!pm_warm()) {
    vTaskDelay(pdMS_TO_TICKS(FIRST_DRAW_MS));
  } else {
    // After a wake: once this wake's sample and upload are done, so the
    // picture shows them -- and no later, the chip is waiting to sleep.
    for (int i = 0; i < 300 && !(pm_is_done(Duty::Trip) && pm_is_done(Duty::Uplink)); i++) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
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

    if (!g_hold) {
      // The picture without its clock decides whether anything changed:
      // the minute ticking over is not news.
      build(s, "     ");
      const uint32_t content = g_draw.hash();
      // The footer's left icons and USB power: a change in either is
      // redrawn now (decided 2026-10-02), within ICON_REDRAW_GAP_MS. A
      // cable plugged in redraws even if nothing visible changed -- the
      // person who plugged it is looking for an answer.
      const uint8_t icons = icons_now(s);
      // The gap is for a link flapping at the edge of range, which only
      // happens while Wi-Fi stays up -- on USB power. On battery each wake
      // decides once and then sleeps; a change held back here would wait
      // a whole sample period.
      const bool icons_due = icons != g_shown_icons &&
                             (!g_icons_drawn_at || !pm_external_power() ||
                              now_ms() - g_icons_drawn_at >= ICON_REDRAW_GAP_MS);
      const bool usb_changed = icons_due && ((icons ^ g_shown_icons) & ICON_USB) &&
                               g_shown_icons != 0xFF;
      const bool urgent = s.active != g_shown_active ||
                          s.alarms_active != g_shown_alarms ||
                          s.acked != g_shown_acked || icons_due;
      const bool due = !g_shown_at ||
                       now_ms() - g_shown_at >= DISPLAY_MIN_S * 1000;
      if (g_force || usb_changed || (content != g_shown_hash && (urgent || due))) {
        char clock[16];
        clock_str(clock, sizeof(clock));
        build(s, clock);
        pm_hold(Hold::Display, true);
        pm_no_light_sleep(true);    // a refresh is seconds of SPI and BUSY polling
        const bool shown = show(g_draw);
        pm_hold(Hold::Display, false);
        pm_no_light_sleep(false);
        if (shown) {
          g_shown_hash = content;
          g_shown_at = now_ms();
          g_shown_active = s.active;
          g_shown_alarms = s.alarms_active;
          g_shown_acked = s.acked;
          if (icons != g_shown_icons) g_icons_drawn_at = now_ms();
          g_shown_icons = icons;
          printf("[display] refreshed in %lu ms%s\n",
                 (unsigned long)epd_last_refresh_ms(),
                 usb_changed ? " (USB power changed)"
                 : icons_due ? " (icons changed)"
                 : urgent ? " (state change)" : "");
        } else {
          printf("[display] panel did not answer\n");
        }
        fflush(stdout);
        g_force = false;
      }
    }
    pm_done(Duty::Display);
    vTaskDelay(pdMS_TO_TICKS(PASS_MS));
  }
}

}  // namespace

void display_apply_insets(void) {
  const Config &k = config();
  scr_set_insets(k.epd_inset_t, k.epd_inset_b, k.epd_inset_l, k.epd_inset_r);
}

void display_start(const char *sn) {
  snprintf(g_sn, sizeof(g_sn), "%s", sn ? sn : "MCOLD");
  display_apply_insets();
  g_epd = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(task, "display", 4096, nullptr, 1, nullptr, 1);
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

void display_battery_off(float cell_volts) {
  if (!g_epd) return;
  g_hold = true;                // nothing else draws after this
  TripStatus s;
  trip_status(&s);
  PowerStatus p;
  bool have;
  portENTER_CRITICAL(&g_mux);
  p = g_pwr;
  have = g_have_pwr;
  portEXIT_CRITICAL(&g_mux);
  char clock[16], data[40];
  clock_str(clock, sizeof(clock));
  snprintf(data, sizeof(data), "CELL %.2f V", cell_volts);
  // Capitals: the title font is cut to ' '..'Z' (fonts_mcold.h), and a
  // lower-case letter there is simply not drawn -- "Battery empty" came
  // out as "B" on the panel (2026-10-03).
  scr_takeover(g_draw, g_sn, clock, footer_state(s, p, have), "BATTERY EMPTY",
               "Switched off. Charge to restart.", data, false, true);
  pm_hold(Hold::Display, true);
  pm_no_light_sleep(true);
  show(g_draw);
  pm_hold(Hold::Display, false);
  pm_no_light_sleep(false);
}

