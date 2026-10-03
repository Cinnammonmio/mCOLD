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
#include <driver/gpio.h>
#include <driver/usb_serial_jtag.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_pm.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "board.h"
#include "bus.h"
#include "buzzer.h"
#include "chargeled.h"
#include "config.h"
#include "flashlog.h"
#include "flashlog_test.h"
#include "door.h"
#include "features.h"
#include "record.h"
#include "trip.h"
#include "indicate.h"
#include "canvas.h"
#include "epd.h"
#include "screens.h"
#include "display.h"
#include "rpc.h"
#include "ble.h"
#include "auth.h"
#include "net.h"
#include "uplink.h"
#include "gnss.h"
#include "health.h"
#include "leds.h"
#include "nfc.h"
#include "pm.h"
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
enum class Job : uint8_t { Sensors = 0, Power, Gnss, Nfc, Trip, Console, Count };

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
    {0, 0, 5000, "trip"},
    {0, 0, 5000, "console"},
};

inline void beat(Job j) { g_beats[(int)j].count = g_beats[(int)j].count + 1; }

// Runs through deep sleep (timekeep.h), so times kept across one compare.
uint32_t now_ms(void) { return mono_ms(); }
// Since this boot or wake: esp_timer starts again at every wake.
uint32_t uptime_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

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
// Bench: charger input off for a while, so the box runs from its cell
// with the cable in. Switched back on by itself: a box left in HIZ by a
// forgotten command would never charge again.
volatile uint32_t g_hiz_until = 0;   // uptime_ms(); 0: not in HIZ

// How long after a beep motion events are still the beep's own.
const uint32_t BUZZER_BLANK_MS = 250;

// Periods. P1 values: short enough to see each driver work on the
// bench, and nothing here is yet the sampling policy of a trip (P3).
const uint32_t SENSOR_PASS_MS = 5000;
const uint32_t GNSS_SESSION_MS = 120000;   // give up on a fix after this
// Until the first fix since power-up: a module starting cold, with no
// almanac, needs minutes of open sky, more than a routine session gives.
const uint32_t GNSS_COLD_SESSION_MS = 600000;
const uint32_t GNSS_PERIOD_MS = 600000;    // between sessions
const int GNSS_FIXES_WANTED = 5;
// Console "gnss hold N": keep the module on for N minutes, for an antenna
// test by a window. A cold start can need longer than one session.
volatile uint32_t g_gnss_hold_until = 0;

// The best GNSS heard in a boot, in RTC memory: it survives a
// reset (opening the USB console is one) though not a power loss. So a
// box taken to a window on battery can be plugged back in and asked what
// it heard there.
struct GnssMemo {
  uint32_t magic;
  uint32_t boot;            // the boot it was recorded in
  uint32_t seconds;         // how long the module was on, that boot
  uint8_t in_view, heard, best_snr, sats;
  bool fix;
  double lat, lon;
};
const uint32_t GNSS_MEMO_MAGIC = 0x474E5353;   // "GNSS"
RTC_NOINIT_ATTR GnssMemo g_gnss_memo;
GnssMemo g_gnss_before = {};   // the memo as found at boot: before the reset
// On battery a GNSS session keeps the whole box awake, not just the
// module, so sessions are planned: only during a trip (or while the clock
// has no time and satellites are the way to get it), once every
// gnss_period_s, and stretched while they keep failing -- indoors there
// is nothing to find, and looking every half hour would cost more than
// all the sampling. Kept through deep sleep.
struct GnssPlan {
  uint32_t magic;
  uint32_t next_at;         // mono_ms(); 0: due now
  uint8_t misses;           // sessions in a row without a fix
};
const uint32_t GNSS_PLAN_MAGIC = 0x47504C31;   // "GPL1"
RTC_DATA_ATTR GnssPlan g_gnss_plan;
// On battery: a module that had a fix in the last four hours keeps its
// ephemeris in the backup domain (GNSS VBAT is on 3V3_MAIN) and fixes in
// seconds; a minute and a half is plenty. Without one, three minutes.
const uint32_t GNSS_HOT_MS = 4 * 3600000;
const uint32_t GNSS_BATT_SESSION_MS = 90000;
const uint32_t GNSS_BATT_COLD_SESSION_MS = 180000;
// Not one satellite heard in this long: the box is indoors (or in a
// container), and the rest of the session would only cost current. The
// night of 2026-10-02 spent ~3 mA on full-length sessions in a room.
const uint32_t GNSS_NOTHING_HEARD_MS = 60000;

// The next trip sample, through deep sleep.
struct SamplePlan {
  uint32_t magic;
  uint32_t trip;
  uint32_t next;            // mono_ms()
};
const uint32_t SAMPLE_PLAN_MAGIC = 0x53504C31;   // "SPL1"
RTC_DATA_ATTR SamplePlan g_sample_plan;

const uint32_t NFC_POLL_MS = 300;
const uint32_t FIRST_SAMPLE_WAIT_MS = 10000;   // uptime limit, see task_trip
const uint32_t CONSOLE_HOLD_MS = 120000;       // awake after a typed line

// ---- sensors ---------------------------------------------------------

void report_motion(const AccelEvent &ev, const char *when) {
  printf("[accel] %lu ms%s%s%s%s%s  (src 0x%02X, event %lu)\n",
         (unsigned long)now_ms(), when,
         ev.free_fall ? " free-fall" : " motion", ev.x ? " X" : "",
         ev.y ? " Y" : "", ev.z ? " Z" : "", ev.raw_src,
         (unsigned long)accel_event_count());
  fflush(stdout);
  // Someone is handling the box: worth showing them its status for a
  // little while, within the hourly budget.
  indicate_attention();
}

void task_sensors(void *) {
  accel_notify_task(xTaskGetCurrentTaskHandle());
  uint32_t last_pass = 0;
  bool first = true;

  for (;;) {
    // Wakes on the accelerometer's INT1 edge or after the pass period,
    // whichever is first. A motion event is handled within
    // milliseconds instead of waiting out the rest of a sleep.
    // Not before the first pass: on a wake from sleep this reading is
    // what the whole wake is waiting for.
    ulTaskNotifyTake(pdTRUE, first ? 0 : pdMS_TO_TICKS(SENSOR_PASS_MS));
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
        if (buzzer_recent(BUZZER_BLANK_MS)) {
          g_motion_blanked++;
        } else {
          report_motion(ev, "");
          trip_note_motion(ev);
        }
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
      trip_note_temp(ts, c);
    }
    pm_done(Duty::Sensors);
  }
}

// ---- gnss ------------------------------------------------------------
//
// Sessions, not a stream: the module draws tens of milliamps while it
// searches, so it is powered for a bounded window, kept only until it
// has a fix, and switched off. How often, and whether motion should
// trigger one, is trip policy (P3); these numbers only keep P1 honest
// about power and give the driver something real to do.

bool gnss_wanted(bool battery) {
  if (!battery) return true;
  TripStatus s;
  trip_status(&s);
  return s.active || !rtc_time_valid();
}

void task_gnss(void *) {
  if (!pm_warm() || g_gnss_plan.magic != GNSS_PLAN_MAGIC) g_gnss_plan = {GNSS_PLAN_MAGIC, 0, 0};
  for (;;) {
    beat(Job::Gnss);
    const bool battery = !pm_external_power();
    const bool wanted = gnss_wanted(battery);
    const uint32_t t0 = now_ms();
    const bool held = (int32_t)(g_gnss_hold_until - t0) > 0;
    const uint32_t next = g_gnss_plan.next_at;
    const bool due = held || (wanted && (!next || (int32_t)(t0 - next) >= 0));
    pm_next(Duty::Gnss, battery && wanted ? (next ? next : t0) : 0);
    if (!due || !health_should_try(Dev::Gnss, t0) || !gnss_power_on()) {
      pm_done(Duty::Gnss);
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    // The duty is the decision; the session that follows is a hold, and
    // ends on its own clock.
    pm_hold(Hold::Gnss, true);
    pm_done(Duty::Gnss);
    printf("[gnss] %lu ms session start\n", (unsigned long)uptime_ms());
    fflush(stdout);

    const uint32_t start = now_ms();
    GnssStats gs;
    gnss_stats(&gs);
    GnssFix prior;
    const bool hot = gnss_last_fix(&prior) && prior.at_ms && start - prior.at_ms < GNSS_HOT_MS;
    const uint32_t limit = battery ? (hot ? GNSS_BATT_SESSION_MS : GNSS_BATT_COLD_SESSION_MS)
                                   : (gs.fixes ? GNSS_SESSION_MS : GNSS_COLD_SESSION_MS);
    int fixes = 0;
    while (now_ms() - start < limit ||
           (int32_t)(g_gnss_hold_until - now_ms()) > 0) {
      beat(Job::Gnss);
      gnss_pump(1000);
      {
        GnssSky k;
        gnss_sky(&k);
        GnssMemo &m = g_gnss_memo;
        // The best of the whole boot, not of the latest session: a box
        // carried to a window and back must still say what it heard there.
        if (m.magic != GNSS_MEMO_MAGIC || m.boot != time_boot_count()) {
          m = {};
          m.magic = GNSS_MEMO_MAGIC;
          m.boot = time_boot_count();
        }
        m.seconds++;            // one pump per second: seconds of GNSS on
        if (k.in_view > m.in_view) m.in_view = k.in_view;
        if (k.heard > m.heard) m.heard = k.heard;
        if (k.best_snr > m.best_snr) m.best_snr = k.best_snr;
        GnssFix gf;
        if (gnss_last_fix(&gf) && gf.valid && now_ms() - gf.at_ms < 3000) {
          m.fix = true;
          m.lat = gf.lat_deg;
          m.lon = gf.lon_deg;
          if (gf.sats > m.sats) m.sats = gf.sats;
        }
      }

      if (battery && now_ms() - start >= GNSS_NOTHING_HEARD_MS &&
          (int32_t)(g_gnss_hold_until - now_ms()) <= 0) {
        GnssSky k;
        gnss_sky(&k);
        if (!k.in_view && !k.heard) {
          printf("[gnss] nothing heard in %lu s: indoors, giving up\n",
                 (unsigned long)(GNSS_NOTHING_HEARD_MS / 1000));
          break;
        }
      }

      GnssFix f;
      if (!gnss_last_fix(&f) || !f.valid || now_ms() - f.at_ms > 2000) continue;
      fixes++;

      // The RTC has no backup cell, so after any power loss its time is
      // gone. Satellite time is the most trustworthy source this box
      // has; the first status-A sentence puts the clock back.
      if (!rtc_time_valid() && f.time_valid) {
        TimeStamp before;
        time_now(&before);
        if (time_set(&f.utc, TimeSource::Gnss, false)) {
          // Recorded in the trip, so records stamped before this can be
          // re-timed against the clock that was wrong, not trusted.
          trip_note_time_set(TimeSource::Gnss,
                             before.quality == TimeSource::None
                                 ? 0 : (uint32_t)(before.utc_ms / 1000));
          printf("[gnss] clock set from satellite time\n");
          fflush(stdout);
        }
      }
      // A few consecutive fixes rather than the first: the first one
      // out of a cold start is the least accurate the module produces.
      if (fixes >= GNSS_FIXES_WANTED && (int32_t)(g_gnss_hold_until - now_ms()) <= 0) break;
    }
    gnss_power_off();
    pm_hold(Hold::Gnss, false);

    GnssFix f;
    const bool have = gnss_last_fix(&f);
    const bool got = have && f.valid && now_ms() - f.at_ms < 5000;
    g_gnss_plan.misses = got ? 0 : (uint8_t)(g_gnss_plan.misses < 4 ? g_gnss_plan.misses + 1 : 4);
    // On battery the period stretches x2, x4, x8, x16 while sessions fail
    // (with the default 30 min, up to 8 h between tries indoors).
    const uint32_t period = battery ? ((uint32_t)config().gnss_period_s * 1000) << g_gnss_plan.misses
                                    : GNSS_PERIOD_MS;
    g_gnss_plan.next_at = now_ms() + period;
    if (!g_gnss_plan.next_at) g_gnss_plan.next_at = 1;
    if (got) {
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
    pm_next(Duty::Gnss, battery ? g_gnss_plan.next_at : 0);
    pm_done(Duty::Gnss);
  }
}

// ---- nfc -------------------------------------------------------------

void task_nfc(void *) {
  bool up = false;
  uint32_t last_try = 0;
  for (;;) {
    beat(Job::Nfc);
    const uint32_t t = now_ms();
    if (!up && health_should_try(Dev::Nfc, t)) up = nfc_begin();

    if (up) {
      NfcPoll p;
      nfc_poll(&p);
      // A phone on the antenna: it is reading, or about to talk BLE.
      pm_hold(Hold::NfcField, p.field == NfcField::Present || p.field == NfcField::RfBusy);
      if (p.arrived) {
        printf("[nfc] phone on the tag (%s), tap %lu\n",
               p.field == NfcField::RfBusy ? "I2C refused" : "field",
               (unsigned long)nfc_tap_count());
        fflush(stdout);
        // "This is the box you tapped."
        indicate_cue(Cue::NfcTap);
        // The tap is how a phone asks for the box: open BLE for it.
        ble_window(BLE_TAP_WINDOW_MS);
      }
      // The key goes on the tag when it is due and nothing is reading
      // it: a phone on the antenna holds the tag's RF side and the write
      // would only be refused. Not more than every few seconds either.
      if (p.field == NfcField::Absent && auth_needs_publish() &&
          (!last_try || t - last_try > 5000)) {
        last_try = t ? t : 1;
        printf("[nfc] %s\n", auth_publish() ? "new key on the tag" : "key write refused, will retry");
        fflush(stdout);
      }
    }
    // Done once the tag has been asked and the key, if one was due,
    // written -- a key rotated while the box sleeps would wait a whole
    // sample period on the tag.
    if (!up || !auth_needs_publish() || t - last_try < 5000) pm_done(Duty::Nfc);
    // Edges latch in the tag, so a slow poll still catches a quick tap.
    vTaskDelay(pdMS_TO_TICKS(NFC_POLL_MS));
  }
}

// ---- power -----------------------------------------------------------

// Below config batt_off_mv, on battery, three readings in a row: the box
// switches itself off (decided 2026-10-03). A LiPo run down past ~3.0 V
// is damaged and swells, and even asleep the box keeps drawing; the
// charger's ship mode cuts the cell off from everything but the fuel
// gauge. Plugging USB in brings it back. Three readings, 0.7 s apart,
// so one bad read cannot switch a box off in the middle of a trip.
RTC_DATA_ATTR uint8_t g_low_reads = 0;
[[noreturn]] void battery_off(float volts);

void battery_guard(const PowerStatus &ps) {
  if (pm_external_power() || ps.power_good || !ps.cell_valid ||
      ps.cell_volts < 2.5f || ps.cell_volts > 4.5f) {
    g_low_reads = 0;
    return;
  }
  const uint16_t mv = (uint16_t)lroundf(ps.cell_volts * 1000.0f);
  if (mv >= config().batt_off_mv) {
    g_low_reads = 0;
    return;
  }
  if (++g_low_reads < 3) return;
  printf("[power] cell %u mV, below %ld: switching off; plug in USB to restart\n", mv,
         (long)config().batt_off_mv);
  battery_off(ps.cell_volts);
}

// Also the console's "poweroff", to check the whole path on the bench.
void battery_off(float volts) {
  const uint16_t mv = (uint16_t)lroundf(volts * 1000.0f);
  fflush(stdout);
  trip_note_power_off(mv);
  trip_before_sleep();
  display_battery_off(volts);
  if (!power_ship_mode()) printf("[power] charger did not answer: sleeping instead\n");
  fflush(stdout);
  // Asleep with nothing to wake it but USB power, for the ten seconds
  // until the battery switch opens -- or for good, if the charger did not
  // take the command.
  pm_sleep_until_usb();
}

void task_power(void *) {
  uint32_t last_kick = 0;
  for (;;) {
    beat(Job::Power);
    const uint32_t t = now_ms();

    PowerStatus ps;
    power_read(&ps);
    g_power = ps;
    if (ps.current_valid) pm_note_current(ps.battery_ma < 0 ? -ps.battery_ma : 0);
    chargeled_update(ps);
    trip_note_power(ps);
    display_note_power(ps);
    rpc_note_power(ps);

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
    battery_guard(ps);
    pm_done(Duty::Power);
    if (g_hiz_until && (int32_t)(uptime_ms() - g_hiz_until) >= 0) {
      g_hiz_until = 0;
      power_set_hiz(false);
      printf("[power] charger input back on (hiz time over)\n");
    }
    // Twice in the first seconds of a wake, so even a short wake records
    // the current it costs; then every five seconds.
    vTaskDelay(pdMS_TO_TICKS(uptime_ms() < 3000 ? 700 : 5000));
  }
}

// ---- console ---------------------------------------------------------

// ---- trip -------------------------------------------------------------
//
// Wakes once a second (and on a door edge, when the door feature is on):
// time-based alarms are checked, and a sample is written when one is
// due. The first sample goes in at once -- on a new trip, and after a
// resume, where it marks the far side of the gap the reset made.

void task_trip(void *) {
#if MCOLD_DOOR
  door_notify_task(xTaskGetCurrentTaskHandle());
#endif
  bool was_active = false;
  uint32_t next_sample = 0;
  {
    TripStatus st;
    trip_status(&st);
    // Back from sleep in the same trip: the schedule carries on. Not a
    // resume -- no sample is due just because the chip woke.
    if (pm_warm() && st.active && g_sample_plan.magic == SAMPLE_PLAN_MAGIC &&
        g_sample_plan.trip == st.id) {
      next_sample = g_sample_plan.next;
      was_active = true;
    }
  }
  for (;;) {
    beat(Job::Trip);
    // Every 100 ms until this wake's sample is in: the chip waits on it.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(pm_is_done(Duty::Trip) ? 1000 : 100))) {
#if MCOLD_DOOR
      DoorState s;
      uint32_t lasted = 0;
      if (door_settle(&s, &lasted)) {
        printf("[door] %s (was the other way for %lu ms)\n",
               door_state_name(s), (unsigned long)lasted);
        fflush(stdout);
        trip_note_door(s, lasted);
      }
#endif
    }
    trip_tick();

    TripStatus st;
    trip_status(&st);
    const uint32_t t = now_ms();
    if (st.active && !was_active) {
      // Just started or resumed: the first sample is due now, but not
      // before the first temperature reading since boot has come in --
      // right after a reset that is about a second away, and a sample
      // written before it would record "no data" for no reason. Ten
      // seconds is the limit: a probe that never answers must not stop
      // the sample that says so.
      if (!st.temp_read_since_boot && uptime_ms() < FIRST_SAMPLE_WAIT_MS) continue;
      next_sample = t;
    }
    was_active = st.active;
    const uint32_t period = (uint32_t)config().sample_period_s * 1000;
    bool due = st.active && (int32_t)(t - next_sample) >= 0;
    // A sample is this wake's reading: wait for it (ten seconds at most,
    // the probe that never answers still gets its "no data" sample).
    if (due && !st.temp_read_since_boot && uptime_ms() < FIRST_SAMPLE_WAIT_MS) continue;
    if (due) {
      trip_sample();
      // On the grid, not from now: the wake takes a second, and adding
      // it to every period would walk the samples later all trip long.
      next_sample += period;
      if ((int32_t)(t - next_sample) >= 0 || (int32_t)(next_sample - t) > (int32_t)period) {
        next_sample = t + period;      // fell behind (or the period changed)
      }
      due = false;
    }
    if (st.active) g_sample_plan = {SAMPLE_PLAN_MAGIC, st.id, next_sample};
    uint32_t wake = st.active ? next_sample : 0;
    const uint32_t check = trip_next_check();
    if (check && (!wake || (int32_t)(check - wake) < 0)) wake = check;
    pm_next(Duty::Trip, wake);
    pm_done(Duty::Trip);
  }
}

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
    printf("  RTC cell     %s%s (Control_3 was 0x%02X at boot)\n",
           rtc_backup_low() ? "LOW or absent" : "ok",
           rtc_ran_on_backup() ? ", carried the clock through a power-off" : "",
           rtc_control3_at_boot());
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
  } else if (p.cell_absent) {
    printf("  cell         no battery (%.3f V is the charger's output)\n", p.cell_volts);
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
#if MCOLD_DOOR
    printf("  door         %s\n", door_state_name(door_state()));
#endif
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
  for (int i = 0; i < 2; i++) {
    const GnssMemo &m = i ? g_gnss_memo : g_gnss_before;
    if (m.magic != GNSS_MEMO_MAGIC || (i == 0 && m.boot == time_boot_count())) continue;
    printf("  %s %lu s, best %u in view, %u heard, SNR %u dB-Hz",
           i ? "this boot   " : "before reset", (unsigned long)m.seconds,
           m.in_view, m.heard, m.best_snr);
    if (m.fix) printf(", FIX %.6f, %.6f, %u sats", m.lat, m.lon, m.sats);
    printf("\n");
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
  // The whole record, so what a phone will read can be checked here.
  uint8_t b[128];
  if (nfc_read_user(0, b, sizeof(b))) {
    printf("  record     ");
    for (size_t i = 0; i < sizeof(b) && b[i] != 0xFE; i++) {
      if (b[i] >= 32 && b[i] < 127) putchar((char)b[i]);
    }
    printf("\n");
  }
  printf("  key        %s\n\n", auth_published() ? "on the tag, in force"
                                                 : "not yet on the tag: AUTH will refuse");
}

// ---- trip log ----------------------------------------------------------

FlashIf g_log_flash;
bool g_log_up = false;

// Records written from the console go to this trip and only this trip,
// so bench testing can never mix into a real one. P3 allocates real
// trip ids from a counter, which will never reach this value.
const uint32_t BENCH_TRIP = 0xBE5C0001;

void log_start(void) {
  const uint32_t t0 = now_ms();
  g_log_up = flashlog_partition(&g_log_flash) && flashlog_init(&g_log_flash);
  if (g_log_up) {
    printf("[log] %lu sectors scanned in %lu ms\n",
           (unsigned long)(g_log_flash.size / LOG_SECTOR),
           (unsigned long)(now_ms() - t0));
  } else {
    printf("[log] trip_log partition missing or unreadable: NOTHING WILL BE"
           " LOGGED\n");
  }
}

void print_log(void) {
  if (!g_log_up) {
    printf("\n  trip log not available\n\n");
    return;
  }
  LogStats s;
  flashlog_stats(&s);
  printf("\n  sectors    %lu: %lu in use, %lu free, %lu dirty (reusable)\n",
         (unsigned long)s.sectors, (unsigned long)s.used,
         (unsigned long)s.free, (unsigned long)s.dirty);
  printf("  space      %lu of %lu records' worth used (%.1f%%)\n",
         (unsigned long)(s.used * LOG_SLOTS),
         (unsigned long)(s.sectors * LOG_SLOTS),
         100.0 * s.used / (s.sectors ? s.sectors : 1));
  // At the configured sample period, how long the free space lasts
  // with nothing uploaded: the offline-capacity question of §9.3.
  const double days = (double)(s.free + s.dirty) * LOG_SLOTS *
                      config().sample_period_s / 86400.0;
  printf("  offline    about %.0f days of samples at one per %ld s"
         " (samples only)\n",
         days, (long)config().sample_period_s);
  uint32_t t[8];
  const int n = flashlog_trips(t, 8);
  printf("  trips      %d", n);
  for (int i = 0; i < n && i < 8; i++) {
    uint32_t last = 0;
    const bool any = flashlog_last_seq(t[i], &last);
    printf("%s%08lX (%s%lu)", i ? ", " : "  ", (unsigned long)t[i],
           any ? "last seq " : "no records", any ? (unsigned long)last : 0UL);
  }
  printf("\n\n");
}

void log_bench_write(int n) {
  if (n < 1 || n > 2000) {
    printf("  log write N   (1..2000)\n");
    return;
  }
  const uint32_t t0 = now_ms();
  int ok = 0;
  LogErr e = LogErr::Ok;
  for (int i = 0; i < n; i++) {
    uint8_t p[32];
    uint32_t seq = 0;
    uint32_t last;
    const uint32_t next = flashlog_last_seq(BENCH_TRIP, &last) ? last + 1 : 0;
    for (int k = 0; k < (int)sizeof(p); k++) p[k] = (uint8_t)(next + k);
    e = flashlog_append(BENCH_TRIP, 0x7F, p, sizeof(p), &seq);
    if (e != LogErr::Ok) break;
    ok++;
    if (i % 50 == 49) beat(Job::Console);
  }
  printf("  %d of %d written in %lu ms%s%s\n", ok, n,
         (unsigned long)(now_ms() - t0), e == LogErr::Ok ? "" : ", stopped: ",
         e == LogErr::Ok ? "" : log_err_name(e));
}

bool bench_visit(const LogRecord &r, void *ctx) {
  uint32_t *bad = (uint32_t *)ctx;
  bool ok = r.len == 32 && r.type == 0x7F;
  for (int k = 0; k < 32 && ok; k++) ok = r.payload[k] == (uint8_t)(r.seq + k);
  if (!ok) (*bad)++;
  return true;
}

void log_bench_read(void) {
  uint32_t bad = 0, torn = 0;
  const uint32_t t0 = now_ms();
  const uint32_t n = flashlog_read(BENCH_TRIP, 0, bench_visit, &bad, &torn);
  printf("  %lu records, %lu wrong, %lu torn slots, read in %lu ms\n",
         (unsigned long)n, (unsigned long)bad, (unsigned long)torn,
         (unsigned long)(now_ms() - t0));
}

// ---- trip console ------------------------------------------------------

void print_trip(void) {
  TripStatus s;
  trip_status(&s);
  if (!s.active) {
    printf("\n  no trip running");
    if (s.id) printf("; last was %08lX", (unsigned long)s.id);
    printf("\n  deleted to make room since new: %lu\n\n",
           (unsigned long)s.lost_trips);
    return;
  }
  const TripParams &p = s.params;
  printf("\n  trip       %08lX, running\n", (unsigned long)s.id);
  printf("  alarms at  below %.1f C or above %.1f C, after %u s,"
         " clear %.1f C inside\n",
         p.low_c10 / 10.0, p.high_c10 / 10.0, p.dwell_s, p.hyst_c10 / 10.0);
#if MCOLD_DOOR
  printf("             door open over %u s%s\n", p.door_alarm_s,
         p.door_alarm_s ? "" : " (off)");
#endif
  printf("  samples    %lu, one per %ld s\n", (unsigned long)s.samples,
         (long)config().sample_period_s);
  if (s.have_temp) {
    printf("  range      %.2f .. %.2f C\n", s.min_c100 / 100.0, s.max_c100 / 100.0);
  }
#if MCOLD_DOOR
  printf("  door       opened %u times\n", s.door_opens);
#endif
  printf("  motion     %lu events\n", (unsigned long)s.motion_events);
  printf("  alarms     %u raised", s.alarms_raised);
  if (s.alarms_active) {
    printf(", active:");
    for (int a = 0; a < AL_COUNT; a++) {
      if (s.alarms_active & (1u << a)) printf(" [%s]", alarm_name((uint8_t)a));
    }
    printf("%s", s.acked ? " (acknowledged)" : "");
  }
  printf("\n\n");
}

// One record per line, decoded per record.h.
bool dump_visit(const LogRecord &r, void *) {
  Reader rd(r.payload, r.len);
  const uint32_t utc = rd.u32();
  const uint8_t q = rd.u8();
  const uint16_t boot = rd.u16();
  const uint32_t tick = rd.u32();
  char when[24] = "no time";
  if (q) {
    const time_t s = (time_t)utc;
    struct tm tm;
    gmtime_r(&s, &tm);
    strftime(when, sizeof(when), "%m-%d %H:%M:%S", &tm);
  }
  printf("  %5lu  %-14s b%u+%lus  ", (unsigned long)r.seq, when, boot,
         (unsigned long)(tick / 1000));
  switch (r.type) {
    case REC_TRIP_START: {
      rd.u8();
      rd.u32();
      const int16_t lo = rd.i16(), hi = rd.i16();
      printf("START   alarms %.1f..%.1f C", lo / 10.0, hi / 10.0);
      break;
    }
    case REC_SAMPLE: {
      const uint8_t st = rd.u8();
      rd.u16();
      const int16_t c = rd.i16();
      rd.u8();       // door state: not fitted while MCOLD_DOOR is off
      rd.u16();      // door openings
      const uint16_t motion = rd.u16();
      rd.n = 30;
      const uint16_t age = rd.u16();
      rd.n = 36;
      const uint8_t soc = rd.u8();
      rd.n = 40;
      const uint16_t alarms = rd.u16();
      if (st == 0 && c != I16_NONE) printf("SAMPLE  %6.2f C", c / 100.0);
      else printf("SAMPLE  temp -- (%s)", temp_status_name((TempStatus)st));
      printf("  motion %u  fix %s  soc %s%u  alarms %04X", motion,
             age == U16_NONE ? "none" : "aged", soc == 0xFF ? "-" : "",
             soc == 0xFF ? 0 : soc, alarms);
      break;
    }
    case REC_EVENT: {
      const uint8_t code = rd.u8();
      static const char *const NAMES[] = {
          "?", "RESUMED", "DOOR OPEN", "DOOR CLOSE", "MOTION", "PROBE FAULT",
          "PROBE OK", "ALARM", "ALARM CLEAR", "ALARM ACK", "TIME SET", "LOSS",
          "POWER OFF"};
      printf("EVENT   %s", code < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[code] : "?");
      if (code == EV_POWER_OFF) {
        printf(" (battery low, cell %u mV)", rd.u16());
      } else if (code == EV_ALARM_RAISE || code == EV_ALARM_CLEAR) {
        printf(" %s", alarm_name(rd.u8()));
      } else if (code == EV_DOOR_CLOSE) {
        printf(" after %lu ms", (unsigned long)rd.u32());
      } else if (code == EV_RESUMED) {
        printf(" (reset reason %u)", rd.u8());
      }
      break;
    }
    case REC_TRIP_STOP: {
      rd.u8();
      printf("STOP    %lu samples", (unsigned long)rd.u32());
      break;
    }
    default:
      printf("type %02X, %u bytes", r.type, r.len);
  }
  printf("\n");
  return true;
}

void trip_dump(int n) {
  const uint32_t id = trip_last_id();
  uint32_t last;
  if (!id || !flashlog_last_seq(id, &last)) {
    printf("  no trip records\n");
    return;
  }
  if (n < 1) n = 15;
  const uint32_t from = last + 1 > (uint32_t)n ? last + 1 - (uint32_t)n : 0;
  printf("\n  trip %08lX, records %lu..%lu\n", (unsigned long)id,
         (unsigned long)from, (unsigned long)last);
  flashlog_read(id, from, dump_visit, nullptr, nullptr);
  printf("\n");
}

// Settings that take effect the moment they are stored, from the console
// or the app alike; the rest are read where they are used.
void config_changed(const char *key, int32_t v) {
  if (!strcmp(key, "accel_wake_ths") && g_accel_up) {
    accel_set_threshold((uint8_t)v);
  } else if (!strcmp(key, "led_bright_pct")) {
    leds_set_brightness((int)v);
  } else if (!strcmp(key, "led_front_pct")) {
    leds_set_front_brightness((int)v);
  }
}

// "SSID PASS", or "\"SSID WITH SPACES\" PASS". False if there is no SSID.
bool wifi_args(const char *s, char *ssid, size_t sn, char *pass, size_t pn) {
  while (*s == ' ') s++;
  size_t i = 0;
  if (*s == '"') {
    s++;
    while (*s && *s != '"' && i + 1 < sn) ssid[i++] = *s++;
    if (*s == '"') s++;
  } else {
    while (*s && *s != ' ' && i + 1 < sn) ssid[i++] = *s++;
  }
  ssid[i] = 0;
  while (*s == ' ') s++;
  snprintf(pass, pn, "%s", s);
  return i > 0;
}

void print_sync(void) {
  NetStatus n;
  net_status(&n);
  printf("\n  Wi-Fi      ");
  if (!n.configured) printf("no networks (wifi add SSID PASS)\n");
  else if (n.connected) printf("%s, %s, %d dBm\n", n.ssid, n.ip, n.rssi);
  else printf("not connected (%lu attempts)\n", (unsigned long)n.reconnects);
  printf("  known      ");
  for (int i = 0; i < n.known; i++) {
    char s[33];
    if (net_known(i, s, sizeof(s))) printf("%s%s", i ? ", " : "", s);
  }
  printf("%s\n", n.known ? "" : "none");
  UplinkStatus u;
  uplink_status(&u);
  printf("  broker     ");
  if (!u.configured) printf("not set up (mqtt set HOST PORT USER PASS)\n");
  else printf("%s:%u, %s\n", u.host, u.port, u.connected ? "connected" : "not connected");
  printf("  upload     %lu batches sent, %lu ACKs from the server", (unsigned long)u.batches_sent,
         (unsigned long)u.acks);
  if (u.acks_rejected) printf(", %lu rejected", (unsigned long)u.acks_rejected);
  printf("\n  pending    %lu records not yet confirmed stored\n", (unsigned long)u.records_pending);
  if (u.last_ack_ms) {
    printf("  last ACK   %lu s ago\n", (unsigned long)((now_ms() - u.last_ack_ms) / 1000));
  }
  printf("\n");
}

void print_ble(void) {
  BleStatus b;
  ble_status(&b);
  printf("\n  BLE        %s, %s\n", b.enabled ? "enabled" : "off",
         b.connected ? "connected" : b.advertising ? "advertising" : "quiet");
  if (b.connected) {
    printf("  session    %s, MTU %u\n",
           b.authorized ? "authorized (AUTH with the tag's key)" : "read-only, no AUTH yet",
           b.mtu);
  }
  printf("  requests   %lu over BLE since boot\n\n", (unsigned long)b.requests);
}

// The protocol from the console: whoever has the cable has the box, so
// the request runs as authorized.
void rpc_command(const char *json) {
  RpcSession console = {};
  console.authorized = true;
  char *resp = rpc_handle(json, strlen(json), &console);
  printf("  %s\n", resp);
  free(resp);
}

// ---- display bench ------------------------------------------------------

EXT_RAM_BSS_ATTR Canvas g_canvas;   // demo pages; in PSRAM, as the live one is

void screen_command(const char *args) {
  if (!*args || !strcmp(args, "live")) {
    display_refresh();
    printf("  live display, redrawing now\n");
    return;
  }
  if (!strncmp(args, "rot ", 4)) {
    display_set_rotation(atoi(args + 4));
    printf("  rotation %d (screen live to redraw)\n", display_rotation());
    return;
  }
  const int p = atoi(args);
  if (p < 0 || p >= SCR_DEMO_PAGES || (*args < '0' || *args > '9')) {
    printf("  screen | screen N (0..%d, design demo) | screen rot 1|3\n",
           SCR_DEMO_PAGES - 1);
    return;
  }
  // Demo pages hold the live display off until 'screen' or 'screen live'.
  display_hold(true);
  scr_demo(g_canvas, p);
  const uint32_t t0 = now_ms();
  const bool ok = display_show(g_canvas);
  printf("  %s: %s, BUSY %lu ms, %lu ms in all  (screen live to go back)\n",
         scr_demo_name(p), ok ? "shown" : "PANEL DID NOT ANSWER",
         (unsigned long)epd_last_refresh_ms(), (unsigned long)(now_ms() - t0));
}

void trip_command(const char *args) {
  if (!*args) {
    print_trip();
  } else if (!strncmp(args, "start", 5)) {
    // trip start [LOW HIGH [HYST DWELL]], in C and seconds; 2..8 C by
    // default. In the product these come from the app at START (P5).
    float lo = 2, hi = 8, hyst = 0.5f;
    int dwell = 300;
    // sscanf gives EOF (-1) for no arguments at all: that means defaults.
    const int k = sscanf(args + 5, "%f %f %f %d", &lo, &hi, &hyst, &dwell);
    if (k == 1) {
      printf("  trip start [LOW HIGH [HYST DWELL_S]]   e.g. trip start 2 8\n");
      return;
    }
    TripParams p = {(int16_t)lroundf(lo * 10), (int16_t)lroundf(hi * 10),
                    (uint16_t)lroundf(hyst * 10), (uint16_t)dwell, 0};
    uint32_t id = 0;
    const TripErr e = trip_start(p, &id);
    if (e == TripErr::Ok) {
      printf("  trip %08lX started: alarms below %.1f or above %.1f C\n",
             (unsigned long)id, lo, hi);
    } else {
      printf("  not started: %s\n", trip_err_name(e));
    }
  } else if (!strcmp(args, "stop")) {
    const TripErr e = trip_stop(1);
    printf("  %s\n", e == TripErr::Ok ? "trip stopped" : trip_err_name(e));
  } else if (!strcmp(args, "ack")) {
    trip_ack_alarms();
    printf("  alarms acknowledged (they stay in the log)\n");
  } else if (!strncmp(args, "dump", 4)) {
    trip_dump(atoi(args + 4));
  } else {
    printf("  trip | trip start LOW HIGH | trip stop | trip ack | trip dump [N]\n");
  }
}

// The very last word, after the rails are off and the pins held: the
// accelerometer latches an "event" from that switching itself (seen as
// motion wakes 0 s after going to sleep, 2026-10-03), so whatever is
// latched now is ours and is dropped, not counted. INT1 is low after.
void before_sleep_quiet(void) {
  if (g_accel_up && accel_int_level()) {
    AccelEvent ev;
    accel_take_event(&ev);
    g_motion_blanked++;
  }
}

// The last word before deep sleep, from the pm task.
void before_sleep(void) {
  // Motion that came in while awake and was not yet read is real: count it.
  if (g_accel_up && accel_int_level()) {
    AccelEvent ev;
    if (accel_take_event(&ev) && !buzzer_recent(BUZZER_BLANK_MS)) trip_note_motion(ev);
  }
  trip_before_sleep();
  net_stop();
  power_sleep(config().sleep_meas != 0);
}

// Bench: battery current, sampled for a while, with min/mean/max.
void amps(int seconds) {
  if (seconds < 1) seconds = 1;
  if (seconds > 60) seconds = 60;
  float lo = 1e9f, hi = -1e9f, sum = 0;
  int n = 0;
  for (uint32_t end = uptime_ms() + (uint32_t)seconds * 1000; (int32_t)(uptime_ms() - end) < 0;) {
    float ma;
    if (power_battery_ma(&ma)) {
      ma = -ma;                  // out of the cell, as the box draws it
      if (ma < lo) lo = ma;
      if (ma > hi) hi = ma;
      sum += ma;
      n++;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    beat(Job::Console);
  }
  if (n) {
    printf("  drawn from the cell over %d s: mean %.1f mA, min %.1f, max %.1f (%d readings)\n",
           seconds, sum / n, lo, hi, n);
  } else {
    printf("  current monitor did not answer\n");
  }
}


void print_help(void) {
  printf("\n  health          every device, its state, and the readings\n");
  printf("  tasks           heartbeats and heap\n");
  printf("  version         firmware version and image hash\n");
  printf("  accel           XYZ now, and the motion event count\n");
  printf("  gnss            receiver statistics and last fix\n");
  printf("  nfc             tag identity, taps, first bytes of memory\n");
  printf("  led I R G B     light pixel I (0-3) for a second\n");
  printf("  ledtest [S] [N] side red, left green, middle blue, right white;\n");
  printf("                  S seconds each (4), N rounds (1)\n");
  printf("  beep MS         sound the buzzer\n");
  printf("  config          every setting, its range and default\n");
  printf("  config set K V  change one setting (stored in NVS)\n");
  printf("  config reset    every setting back to its default\n");
  printf("  reboot          restart the firmware\n");
  printf("  log             trip log: sectors, trips, space\n");
  printf("  logtest         power-cut tests against a RAM image\n");
  printf("  log write N     append N records to the bench trip\n");
  printf("  log read        check the bench trip's records\n");
  printf("  log erase       delete the bench trip\n");
  printf("  trip            the running trip: alarms, samples, counts\n");
  printf("  trip start [L H [HYST DWELL]]  start a trip; alarms below L /\n");
  printf("                  above H C (default 2..8), clear HYST inside,\n");
  printf("                  raise after DWELL s out of range\n");
  printf("  trip stop       end it, with a summary record\n");
  printf("  trip ack        acknowledge alarms (history is kept)\n");
  printf("  trip dump [N]   the last N records, decoded\n");
  printf("  screen          redraw the e-paper now (live view)\n");
  printf("  screen N        design demo page N (0-13); 'screen' to go back\n");
  printf("  screen rot 1|3  the two landscape orientations\n");
  printf("  rpc {json}      a protocol request (PROTOCOL.md), as authorized\n");
  printf("  ble [on|off]    BLE state; 'on' advertises for 60 s\n");
  printf("  sleep           power manager: why awake, next wake, recent wakes\n");
  printf("  sleep clear     start the wake record again\n");
  printf("  sleep test S    deep-sleep S seconds now, timer wake only (even on USB)\n");
  printf("  amps [S]        battery current for S seconds: mean, min, max\n");
  printf("  hiz [S] | off   bench: charger input off S s (300), box runs on its cell\n");
  printf("  poweroff        bench: the battery-empty switch-off; replug USB to restart\n\n");
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
  else if (!strncmp(line, "gnss hold", 9)) {
    const int m = atoi(line + 9);
    const int mins = m > 0 && m <= 60 ? m : 10;
    g_gnss_hold_until = now_ms() + (uint32_t)mins * 60000;
    printf("  GNSS kept on for %d min (gnss to see the sky)\n", mins);
  } else if (!strncmp(line, "gnss raw", 8)) {
    const int s = atoi(line + 8);
    gnss_echo((uint32_t)(s > 0 && s <= 120 ? s : 5) * 1000);
    printf("  raw NMEA for %d s%s\n", s > 0 && s <= 120 ? s : 5,
           gnss_is_on() ? "" : " (module is off between sessions: nothing will come)");
  }
  else if (!strcmp(line, "nfc")) print_nfc();
  else if (!strcmp(line, "help")) print_help();
  else if (!strcmp(line, "log")) print_log();
  else if (!strncmp(line, "rpc ", 4)) rpc_command(line + 4);
  else if (!strcmp(line, "ble")) print_ble();
  else if (!strcmp(line, "sleep")) pm_print();
  else if (!strcmp(line, "poweroff")) {
    // Bench: the battery-empty path, whatever the cell says. The box
    // stays off until USB power is plugged in (unplug and plug back).
    printf("  switching off as if the battery were empty\n");
    battery_off(g_power.cell_valid ? g_power.cell_volts : 0.0f);
  }
  else if (!strncmp(line, "pin ", 4)) {
    // Bench: one pin taken over as a plain GPIO, for finding what a pin
    // left high feeds. Whatever driver owned it does not get it back until
    // a reboot.
    int n = -1;
    char how[4] = "";
    if (sscanf(line + 4, "%d %3s", &n, how) == 2 && n >= 0 && n <= 48 &&
        GPIO_IS_VALID_GPIO((gpio_num_t)n)) {
      gpio_config_t c = {};
      c.pin_bit_mask = 1ULL << n;
      c.mode = strcmp(how, "in") ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
      gpio_config(&c);
      if (c.mode == GPIO_MODE_OUTPUT) gpio_set_level((gpio_num_t)n, atoi(how) ? 1 : 0);
      printf("  GPIO%d %s\n", n, c.mode == GPIO_MODE_INPUT ? "input, no pull" : atoi(how) ? "high" : "low");
    } else {
      printf("  pin N 0|1|in\n");
    }
  }
  else if (!strncmp(line, "cpu ", 4)) {
    // Bench: CPU clock and automatic light sleep while awake, to measure
    // what each costs. Not kept: the next boot is back to the default.
    int mx = 160, mn = 160, ls = 0;
    sscanf(line + 4, "%d %d %d", &mx, &mn, &ls);
    esp_pm_config_t c = {};
    c.max_freq_mhz = mx;
    c.min_freq_mhz = mn;
    c.light_sleep_enable = ls != 0;
    const esp_err_t e = esp_pm_configure(&c);
    printf("  cpu max %d MHz, min %d MHz, light sleep %s: %s\n", mx, mn, ls ? "on" : "off",
           esp_err_to_name(e));
  }
  else if (!strncmp(line, "amps", 4)) amps(atoi(line + 4) > 0 ? atoi(line + 4) : 5);
  else if (!strcmp(line, "hiz off")) {
    g_hiz_until = 0;
    printf("  %s\n", power_set_hiz(false) ? "charger input back on" : "charger did not answer");
  } else if (!strncmp(line, "hiz", 3)) {
    const int s = atoi(line + 3);
    const int secs = s > 0 && s <= 1800 ? s : 300;
    if (power_set_hiz(true)) {
      g_hiz_until = uptime_ms() + (uint32_t)secs * 1000;
      printf("  charger input off for %d s: running on the cell (hiz off to end)\n", secs);
    } else {
      printf("  charger did not answer\n");
    }
  }
  else if (!strcmp(line, "sleep clear")) {
    pm_trace_clear();
    printf("  wake record cleared\n");
  } else if (!strncmp(line, "sleep test", 10)) {
    const int s = atoi(line + 10);
    printf("  sleeping %d s; the console drops and comes back after\n", s > 0 ? s : 10);
    fflush(stdout);
    before_sleep();
    pm_sleep_test((uint32_t)(s > 0 ? s : 10));
  }
  else if (!strcmp(line, "wifi") || !strcmp(line, "sync")) print_sync();
  else if (!strncmp(line, "ack ", 4)) {
    // Bench only: what the server will send, before the server does.
    uplink_inject_ack(line + 4);
    print_sync();
  }
  else if (!strncmp(line, "wifi add ", 9) || !strncmp(line, "wifi set ", 9)) {
    char ssid[40] = "", pass[72] = "";
    if (!wifi_args(line + 9, ssid, sizeof(ssid), pass, sizeof(pass))) {
      printf("  wifi add SSID PASS   (\"quotes\" around an SSID with spaces)\n");
    } else if (net_add(ssid, pass)) {
      printf("  %s stored (%d known); joins the strongest in range\n", ssid, net_count());
    } else {
      printf("  refused: SSID 1-32, password 8-63 or none, at most %d networks\n", NET_MAX);
    }
  } else if (!strncmp(line, "wifi del ", 9)) {
    char ssid[40] = "", pass[2];
    wifi_args(line + 9, ssid, sizeof(ssid), pass, sizeof(pass));
    printf("  %s\n", net_remove(ssid) ? "removed" : "not a known network");
  } else if (!strncmp(line, "mqtt set ", 9)) {
    char host[64] = "", user[40] = "", pass[72] = "";
    int port = 0;
    sscanf(line + 9, "%63s %d %39s %71s", host, &port, user, pass);
    printf("  %s\n", port > 0 && port < 65536 && uplink_set_server(host, (uint16_t)port, user, pass)
                         ? "stored; connecting" : "mqtt set HOST PORT [USER PASS]");
  }
  else if (!strcmp(line, "ble on")) {
    ble_enable(true);
    ble_window(BLE_TAP_WINDOW_MS);
    printf("  BLE on, advertising for %lu s\n", (unsigned long)(BLE_TAP_WINDOW_MS / 1000));
  } else if (!strcmp(line, "ble off")) {
    ble_enable(false);
    printf("  BLE off\n");
  }
  else if (!strcmp(line, "screen")) screen_command("");
  else if (!strncmp(line, "screen ", 7)) screen_command(line + 7);
  else if (!strcmp(line, "trip")) trip_command("");
  else if (!strncmp(line, "trip ", 5)) trip_command(line + 5);
  else if (!strcmp(line, "logtest")) {
    printf("\n");
    const int f = flashlog_selftest();
    printf("\n  %s\n\n", f == 0 ? "all passed" : "FAILURES above");
  } else if (!strncmp(line, "log write ", 10)) log_bench_write(atoi(line + 10));
  else if (!strcmp(line, "log read")) log_bench_read();
  else if (!strcmp(line, "log erase")) {
    printf("  %s\n", log_err_name(flashlog_erase_trip(BENCH_TRIP)));
  } else if (!strcmp(line, "reboot")) {
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
  char line[256];   // room for an rpc request typed or pasted in one line
  int n = 0;
  char prev = 0;
  uint8_t c;
  for (;;) {
    beat(Job::Console);
    while (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(20)) == 1) {
      pm_hold_until(mono_ms() + CONSOLE_HOLD_MS);
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
  // Before any pin is touched: why the chip woke, and the pins held
  // through the sleep let go of at the levels they were held at.
  pm_init();
  // Time for a host to open the console after a reset. A wake from sleep
  // has none waiting (the box sleeps on battery only) and no reason to wait.
  if (!pm_warm()) vTaskDelay(pdMS_TO_TICKS(300));

  // Before any session can overwrite it: what GNSS heard before the reset.
  if (g_gnss_memo.magic == GNSS_MEMO_MAGIC) g_gnss_before = g_gnss_memo;
  health_init();
  rails_init();
  bus_init();
  leds_init();
  buzzer_init();
  epd_init();
  chargeled_start();
  config_init();
#if MCOLD_DOOR
  door_init(MCOLD_DOOR_CLOSED_LEVEL);
#endif
  rtc_begin();
  time_init();
  log_start();
  power_init();
  {
    float sm;     // the sleep just ended, if it was measured (config sleep_meas)
    if (power_sleep_mean(&sm)) pm_note_sleep_current(sm);
  }
  {
    char sn[16];
    device_sn(sn, sizeof(sn));
    trip_init(sn);     // resumes a trip a reset interrupted
    display_start(sn); // like indicate: reads state, owns none
    rpc_init(sn);
    auth_init(sn);     // a new key; the NFC task puts it on the tag
    ble_start(sn);     // after NVS (bonds) and rpc (what it carries)
    net_start();       // Wi-Fi, if credentials are set
    uplink_start(sn);  // MQTT once Wi-Fi is up
    // A full log gives up what the server already has first.
    trip_set_retention({uplink_fully_acked, uplink_forget});
    config_on_change(config_changed);
  }
  indicate_start();    // boot sweep, then status; reads state, owns no state

  // Armed here, not by a console command: opening and closing the
  // serial port resets the board, so anything a command switches on is
  // gone before anyone can tap the box. Bring-up lost three motion
  // tests to that before it was understood.
  AccelEvent pending;
  g_accel_up = accel_begin((uint8_t)config().accel_wake_ths, &pending);

  const esp_app_desc_t *app = esp_app_get_description();
  printf("\n\nmCOLD Foam V.1   firmware %s   reset %d   %s (wake %lu)   heap %u B\n",
         app->version, (int)esp_reset_reason(), pm_wake_name(pm_wake()),
         (unsigned long)pm_wakes(), (unsigned)esp_get_free_heap_size());
  if (pending.wake || pending.free_fall) {
    report_motion(pending, pm_wake() == Wake::Motion ? " woke the box:" : " before boot:");
    // Latched while the chip slept (that is what woke it) or before a
    // reset: motion all the same, and the trip counts it.
    trip_note_motion(pending);
  }
  printf("type: help\n");
  fflush(stdout);

  // Stacks are generous for now and will be trimmed to measured high
  // water marks once these tasks do their real work. Guessing them
  // small this early buys nothing and costs an overflow that presents
  // as a random crash somewhere else entirely.
  // A task that cannot get its stack is never created, and nothing else
  // says so -- the console simply never answers. So every one is checked.
  struct Spawn { TaskFunction_t fn; const char *name; uint32_t stack; UBaseType_t prio; int core; };
  static const Spawn TASKS[] = {
      {task_sensors, "sensors", 4096, 5, 1}, {task_power, "power", 4096, 4, 1},
      {task_gnss, "gnss", 4096, 3, 1},       {task_nfc, "nfc", 3072, 3, 0},
      {task_trip, "trip", 4096, 4, 1},       {task_console, "console", 6144, 2, 0},
      {task_supervisor, "super", 3072, 6, 0},
  };
  for (const Spawn &t : TASKS) {
    if (xTaskCreatePinnedToCore(t.fn, t.name, t.stack, nullptr, t.prio, nullptr, t.core) != pdPASS) {
      printf("TASK %s NOT CREATED: no %lu bytes of internal RAM for its stack\n", t.name,
             (unsigned long)t.stack);
    }
  }
  printf("internal RAM free %u B (largest block %u B)\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  fflush(stdout);

  // Last: it may put the chip to sleep as soon as every task has done
  // its first pass.
  pm_on_sleep(before_sleep);
  pm_on_quiet(before_sleep_quiet);
  pm_start();

  // app_main returns and the tasks it created carry on. Nothing is left
  // here to become the place where work quietly accumulates.
}
