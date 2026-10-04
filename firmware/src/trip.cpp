#include "trip.h"

#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "features.h"
#include "flashlog.h"
#include "gnss.h"
#include "health.h"
#include "logrow.h"
#include "pm.h"
#include "record.h"
#include "timekeep.h"

namespace {

const char *NS = "trip";

SemaphoreHandle_t g_mx = nullptr;
char g_sn[SN_LEN] = "";

// ---- trip state ------------------------------------------------------

bool g_active = false;
uint32_t g_id = 0;
uint32_t g_last_id = 0;
TripParams g_p = {};

// Running totals. Rebuilt from the log on resume, so they are never the
// only copy of anything.
struct Totals {
  uint32_t samples;
  bool have_temp;
  int16_t min_c100, max_c100;
  uint16_t alarms_raised;
  uint32_t motion;
  uint8_t last_alarm;       // Alarm, 0xFF none
  uint32_t last_alarm_utc;
};

uint8_t alarm_of(uint8_t ev) {
  switch (ev) {
    case RE_ALARM_HIGH:  return AL_TEMP_HIGH;
    case RE_ALARM_LOW:   return AL_TEMP_LOW;
    case RE_ALARM_PROBE: return AL_PROBE;
    case RE_BATTERY_LOW: return AL_BATTERY;
  }
  return 0xFF;
}
Totals g_t = {};

// Latest inputs.
TempStatus g_temp_st = TempStatus::NoData;
float g_temp_c = 0;
uint32_t g_temp_at = 0;
bool g_temp_this_wake = false;       // a reading since this boot or wake
uint32_t g_probe_bad_since = 0;      // 0: probe fine (or never seen bad)
bool g_probe_fault_logged = false;
PowerStatus g_pwr = {};
bool g_have_pwr = false;
uint16_t g_motion_since_row = 0;     // the motion column: since the previous row
int8_t g_usb = -1;                   // external power at the last look; -1 unknown
uint8_t g_link = LINK_OFFLINE;       // the internet column (uplink says)

// Alarm engine.
uint16_t g_alarms = 0;
bool g_acked = false;
uint32_t g_high_since = 0, g_low_since = 0;
uint32_t g_lost_trips = 0;

// The inputs above, carried through deep sleep. The totals and the alarm
// state come back from the log on every boot, sleep or not; these are the
// things the log does not hold -- how long the probe has been above the
// line, motion since the last sample -- and losing them every five
// minutes would mean a dwell time longer than the sample period could
// never raise its alarm.
struct Kept {
  uint32_t magic;
  uint32_t id;
  TempStatus temp_st;
  float temp_c;
  uint32_t temp_at, probe_bad_since;
  bool probe_fault_logged;
  uint16_t motion_since_row;
  int8_t usb;
  uint8_t link;
  uint32_t high_since, low_since;
};
const uint32_t KEPT_MAGIC = 0x54524B32;   // "TRK2"
RTC_DATA_ATTR Kept g_kept;

// Interval clock: runs through deep sleep, so the times above stay
// meaningful across one.
uint32_t now_ms(void) { return mono_ms(); }

struct Lock {
  Lock() { xSemaphoreTake(g_mx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_mx); }
};

// ---- NVS: only the two facts needed to find the trip again -----------

uint32_t nvs_u32(const char *k, uint32_t def) {
  nvs_handle_t h;
  uint32_t v = def;
  if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, k, &v);
    nvs_close(h);
  }
  return v;
}

bool nvs_put(const char *k, uint32_t v) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_u32(h, k, v) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

// ---- writing records -------------------------------------------------

float calibrated(float c);

// The state of the box now, as a row: every row -- a sample or an event
// -- carries it, so each one reads on its own (decided 2026-10-05).
// Takes the motion count with it.
LogRow snapshot(uint8_t event, int32_t detail) {
  LogRow r = {};
  TimeStamp ts;
  time_now(&ts);
  r.utc = ts.quality == TimeSource::None ? 0 : (uint32_t)(ts.utc_ms / 1000);
  r.time_q = (uint8_t)ts.quality;
  r.boot = (uint16_t)ts.boot;
  r.tick_ms = ts.tick_ms;
  r.event = event;
  const uint32_t t = now_ms();
  // A reading more than a minute old is not this row's reading.
  const bool fresh = g_temp_at && t - g_temp_at <= 60000 && g_temp_st == TempStatus::Ok;
  r.temp_c100 = fresh ? (int16_t)lroundf(calibrated(g_temp_c) * 100.0f) : I16_NONE;
  r.alarms = (uint8_t)g_alarms;
  GnssFix f;
  if (gnss_last_fix(&f) && f.valid) {
    const uint32_t age = f.at_ms ? t - f.at_ms : UINT32_MAX;
    r.gnss = age <= (uint32_t)config().sample_period_s * 1000 ? GNSS_FIX : GNSS_LAST;
    r.lat_e7 = (int32_t)llround(f.lat_deg * 1e7);
    r.lon_e7 = (int32_t)llround(f.lon_deg * 1e7);
  }
  r.motion = g_motion_since_row;
  r.battery = g_have_pwr && g_pwr.cell_valid ? (uint8_t)lroundf(g_pwr.soc_percent) : 0xFF;
  r.internet = g_link;
  r.detail = detail;
  return r;
}

// Deletes the oldest trip that is not running, to make room. Writes the
// loss into the running trip first: if power fails between the two, the
// log says a trip was lost that is in fact still there, which is the
// safe way round.
// A trip deleted to make room, and the notice that says so.
TripRetention g_ret = {};

struct Loss {
  uint32_t trip;
  uint32_t records;
  bool noted;          // the DATA_LOST row is in the log
  bool lost;           // false: the server had it all, nothing was lost
};

bool write_loss(const Loss &v) {
  uint8_t buf[LOG_PAYLOAD_MAX];
  const LogRow r = snapshot(RE_DATA_LOST, (int32_t)v.trip);
  const size_t n = row_encode(r, nullptr, nullptr, buf, sizeof(buf));
  if (!n || flashlog_append(g_id, REC_ROW, buf, n, nullptr) != LogErr::Ok) return false;
  g_motion_since_row = 0;
  return true;
}

// Deletes the oldest trip that is not this one, to make room.
//
// §9.6 wants the loss written before the data goes, and that is the
// order whenever it is possible: if power fails in between, the log
// claims a loss that did not happen, which is the safe way round. Two
// cases cannot: the log is so full that not even the notice fits, and
// a trip starting, whose header must be its first record. Then the
// notice follows the deletion (`noted` false), and the caller writes it.
bool evict_one(bool notice_first, Loss *out) {
  uint32_t trips[16];
  const int n = flashlog_trips(trips, 16);

  // First, a trip the server already has in full (§9.6 step 1): deleting
  // it gives the space back and loses nothing, so it is no loss event.
  if (g_ret.fully_acked) {
    for (int i = 0; i < n && i < 16; i++) {
      if (trips[i] == g_id || !g_ret.fully_acked(trips[i])) continue;
      if (flashlog_erase_trip(trips[i]) != LogErr::Ok) return false;
      if (g_ret.forget) g_ret.forget(trips[i]);
      printf("[trip] log full: reclaimed trip %08lX, already on the server\n",
             (unsigned long)trips[i]);
      *out = {trips[i], 0, true, false};
      return true;
    }
  }

  // Then the oldest finished trip, whole, as a loss.
  for (int i = 0; i < n && i < 16; i++) {
    if (trips[i] == g_id) continue;
    Loss v = {trips[i], flashlog_read(trips[i], 0, nullptr, nullptr, nullptr),
              false, true};
    if (notice_first) v.noted = write_loss(v);
    if (flashlog_erase_trip(v.trip) != LogErr::Ok) return false;
    if (g_ret.forget) g_ret.forget(v.trip);
    g_lost_trips++;
    nvs_put("lost", g_lost_trips);
    printf("[trip] log full: deleted trip %08lX (%lu records) to make room\n",
           (unsigned long)v.trip, (unsigned long)v.records);
    *out = v;
    return true;
  }
  return false;
}

// `starting`: this is the trip's header, which must be record 0.
TripErr put(const LogRow &r, const RowHeader *h = nullptr, const RowSummary *sum = nullptr,
            bool starting = false) {
  uint8_t buf[LOG_PAYLOAD_MAX];
  const size_t n = row_encode(r, h, sum, buf, sizeof(buf));
  if (!n) return TripErr::Flash;
  LogErr e = flashlog_append(g_id, REC_ROW, buf, n, nullptr);
  Loss v;
  if (e == LogErr::Full && evict_one(!starting, &v)) {
    e = flashlog_append(g_id, REC_ROW, buf, n, nullptr);
    if (e == LogErr::Ok && v.lost && !v.noted) write_loss(v);
  }
  switch (e) {
    case LogErr::Ok:
      g_motion_since_row = 0;   // counted into this row
      return TripErr::Ok;
    case LogErr::Full: return TripErr::LogFull;
    default:           return TripErr::Flash;
  }
}

// An event row, during a trip.
void event(uint8_t ev, int32_t detail = 0) {
  if (!g_active) return;
  put(snapshot(ev, detail));
}

// ---- alarms ----------------------------------------------------------

uint8_t raise_event(uint8_t a) {
  switch (a) {
    case AL_TEMP_HIGH: return RE_ALARM_HIGH;
    case AL_TEMP_LOW:  return RE_ALARM_LOW;
    case AL_PROBE:     return RE_ALARM_PROBE;
    case AL_BATTERY:   return RE_BATTERY_LOW;
  }
  return 0;
}

// `value` goes in the detail column where it says something the row's
// own columns do not (the battery's percent; a temperature is in `temp`).
void raise_alarm(uint8_t a, int16_t value) {
  if (g_alarms & (1u << a)) return;
  const uint8_t ev = raise_event(a);
  if (!ev) return;          // the door alarm: no door on this product
  g_alarms |= (uint16_t)(1u << a);
  g_acked = false;
  g_t.alarms_raised++;
  TimeStamp ts;
  time_now(&ts);
  g_t.last_alarm = a;
  g_t.last_alarm_utc = ts.quality == TimeSource::None ? 0 : (uint32_t)(ts.utc_ms / 1000);
  event(ev, a == AL_BATTERY ? value : 0);
  printf("[trip] ALARM %s\n", alarm_name(a));
}

void clear_alarm(uint8_t a) {
  if (!(g_alarms & (1u << a))) return;
  g_alarms &= (uint16_t)~(1u << a);
  event(RE_ALARM_CLEAR, a);
  printf("[trip] alarm cleared: %s\n", alarm_name(a));
}

float calibrated(float c) {
  const Config &k = config();
  return c * (float)k.cal_gain_ppm / 1e6f + (float)k.cal_offset_c100 / 100.0f;
}

void evaluate_temp(float c_cal, uint32_t t) {
  const float c10 = c_cal * 10.0f;
  const int16_t v = (int16_t)lroundf(c10);

  // Above high. Raised only after dwell; cleared only once back below
  // high minus hysteresis, so a reading that hovers on the line does not
  // raise and clear every five seconds.
  if (c10 > g_p.high_c10) {
    if (!g_high_since) g_high_since = t ? t : 1;
    if (t - g_high_since >= (uint32_t)g_p.dwell_s * 1000) raise_alarm(AL_TEMP_HIGH, v);
  } else {
    g_high_since = 0;
    if (c10 <= g_p.high_c10 - g_p.hyst_c10) clear_alarm(AL_TEMP_HIGH);
  }
  if (c10 < g_p.low_c10) {
    if (!g_low_since) g_low_since = t ? t : 1;
    if (t - g_low_since >= (uint32_t)g_p.dwell_s * 1000) raise_alarm(AL_TEMP_LOW, v);
  } else {
    g_low_since = 0;
    if (c10 >= g_p.low_c10 + g_p.hyst_c10) clear_alarm(AL_TEMP_LOW);
  }
}

// ---- rebuilding totals from the log ----------------------------------

struct Rebuild {
  Totals t;
  bool have_header;
  TripParams p;
  uint16_t alarms;     // active at the last row
  bool acked;
};

bool rebuild_visit(const LogRecord &r, void *ctx) {
  Rebuild *b = (Rebuild *)ctx;
  LogRow row;
  RowHeader h;
  if (!row_decode(r.type, r.payload, r.len, &row, &h, nullptr)) return true;
  // Alarm state is replayed, not reset: an alarm that was raised and
  // acknowledged before the reset is still raised and still acknowledged
  // after it. Re-raising it would alert someone a second time for the
  // same excursion and count it twice. Every row carries the active
  // alarms, so the last row says which.
  b->alarms = row.alarms;
  b->t.motion += row.motion;
  switch (row.event) {
    case RE_TRIP_START:
      if (h.valid) {
        b->p.low_c10 = h.tempmin_c10;
        b->p.high_c10 = h.tempmax_c10;
        b->p.hyst_c10 = h.hyst_c10;
        b->p.dwell_s = h.dwell_s;
        b->p.door_alarm_s = 0;
        b->have_header = true;
      }
      break;
    case RE_SAMPLE:
      b->t.samples++;
      if (row.temp_c100 != I16_NONE) {
        const int16_t c = row.temp_c100;
        if (!b->t.have_temp || c < b->t.min_c100) b->t.min_c100 = c;
        if (!b->t.have_temp || c > b->t.max_c100) b->t.max_c100 = c;
        b->t.have_temp = true;
      }
      break;
    case RE_ALARM_HIGH:
    case RE_ALARM_LOW:
    case RE_ALARM_PROBE:
    case RE_BATTERY_LOW:
      b->t.alarms_raised++;
      b->t.last_alarm = alarm_of(row.event);
      b->t.last_alarm_utc = row.time_q ? row.utc : 0;
      b->acked = false;
      break;
    case RE_ALARM_ACK:
      b->acked = true;
      break;
  }
  return true;
}

void reset_inputs(void) {
  g_alarms = 0;
  g_acked = false;
  g_high_since = g_low_since = 0;
  g_motion_since_row = 0;
  g_probe_bad_since = 0;
  g_probe_fault_logged = false;
}

// The next trip id: one past both the stored counter and every real trip
// already in the log. The log is the check on the counter -- NVS can be
// erased (config_init does it when NVS is unusable), and a counter that
// restarted at 1 would write into a trip that already exists.
uint32_t next_id(void) {
  uint32_t id = nvs_u32("next_id", 1);
  uint32_t trips[64];
  const int n = flashlog_trips(trips, 64);
  for (int i = 0; i < n && i < 64; i++) {
    if (trips[i] <= TRIP_ID_REAL_MAX && trips[i] >= id) id = trips[i] + 1;
  }
  return id ? id : 1;
}

}  // namespace

// ---- public ----------------------------------------------------------

void trip_init(const char *sn) {
  if (!g_mx) g_mx = xSemaphoreCreateMutex();
  Lock l;
  snprintf(g_sn, sizeof(g_sn), "%s", sn ? sn : "");
  g_lost_trips = nvs_u32("lost", 0);
  g_last_id = nvs_u32("last", 0);

  const uint32_t id = nvs_u32("active", 0);
  if (!id) return;

  // A trip was running when the device last went down. Find its header
  // and carry on with it -- never start a new trip in its place (§8).
  Rebuild b = {};
  flashlog_read(id, 0, rebuild_visit, &b, nullptr);
  if (!b.have_header) {
    printf("[trip] trip %08lX was running but its header is gone; not"
           " resuming\n", (unsigned long)id);
    nvs_put("active", 0);
    return;
  }
  g_id = id;
  g_last_id = id;
  g_p = b.p;
  g_t = b.t;
  g_active = true;
  reset_inputs();
  g_alarms = b.alarms;     // after reset_inputs(), which zeroes them
  g_acked = b.acked;
  if (pm_warm() && g_kept.magic == KEPT_MAGIC && g_kept.id == id) {
    // Back from sleep: the same trip carrying on, not a resume. Nothing
    // was missed, so nothing goes in the log.
    g_temp_st = g_kept.temp_st;
    g_temp_c = g_kept.temp_c;
    g_temp_at = g_kept.temp_at;
    g_probe_bad_since = g_kept.probe_bad_since;
    g_probe_fault_logged = g_kept.probe_fault_logged;
    g_motion_since_row = g_kept.motion_since_row;
    g_usb = g_kept.usb;
    g_link = g_kept.link;
    g_high_since = g_kept.high_since;
    g_low_since = g_kept.low_since;
    return;
  }
  event(RE_POWER_ON, (int32_t)esp_reset_reason());
  printf("[trip] resumed trip %08lX after a reset (%lu samples so far)\n",
         (unsigned long)id, (unsigned long)g_t.samples);
}

void trip_note_power_off(uint16_t cell_mv) {
  if (!g_mx) return;
  Lock l;
  if (!g_active) return;
  event(RE_POWER_OFF, cell_mv);
}

void trip_before_sleep(void) {
  if (!g_mx) return;
  Lock l;
  g_kept = {KEPT_MAGIC, g_active ? g_id : 0, g_temp_st, g_temp_c, g_temp_at,
            g_probe_bad_since, g_probe_fault_logged, g_motion_since_row, g_usb, g_link,
            g_high_since, g_low_since};
}

uint32_t trip_next_check(void) {
  if (!g_mx) return 0;
  Lock l;
  if (!g_active) return 0;
  // An alarm waiting out its dwell time is due when the dwell ends, not
  // at the next sample: the sample period may be longer than the dwell.
  uint32_t at = 0;
  const uint32_t now = now_ms();
  auto earliest = [&at, now](uint32_t since, uint32_t len) {
    if (!since) return;
    const uint32_t due = since + len;
    // Already past: decided at the next reading, which the sample
    // schedule brings. A dwell that ran out while the probe gives no
    // readings would otherwise stay "due" -- in the past -- and keep the
    // chip awake waiting for a reading that is not coming.
    if ((int32_t)(due - now) <= 0) return;
    if (!at || (int32_t)(due - at) < 0) at = due ? due : 1;
  };
  if (!(g_alarms & (1u << AL_TEMP_HIGH))) earliest(g_high_since, (uint32_t)g_p.dwell_s * 1000);
  if (!(g_alarms & (1u << AL_TEMP_LOW))) earliest(g_low_since, (uint32_t)g_p.dwell_s * 1000);
  if (!(g_alarms & (1u << AL_PROBE))) earliest(g_probe_bad_since, TRIP_PROBE_ALARM_MS);
  return at;
}

TripErr trip_start(const TripParams &p, uint32_t *id_out) {
  if (!g_mx) return TripErr::NoLog;
  Lock l;
  if (g_active) return TripErr::AlreadyActive;
  // Decided 2026-10-03: a trip is not started on a battery that cannot
  // carry it. Measured, on battery: a box on a charger may start one.
  if (g_have_pwr && g_pwr.cell_valid && !g_pwr.power_good &&
      g_pwr.cell_volts * 1000.0f < (float)config().batt_trip_mv) {
    return TripErr::BatteryLow;
  }
  if (p.low_c10 >= p.high_c10 || p.low_c10 < -400 || p.high_c10 > 1000 ||
      p.hyst_c10 > 100 || p.dwell_s > 3600) {
    return TripErr::BadParams;
  }
  LogStats s;
  flashlog_stats(&s);
  if (!s.sectors) return TripErr::NoLog;

  const uint32_t id = next_id();
  g_id = id;
  g_p = p;
  g_p.door_alarm_s = 0;     // no door on this product (decided 2026-10-05)
  g_t = {};
  reset_inputs();

  RowHeader h = {};
  h.trip = id;
  h.tempmin_c10 = p.low_c10;
  h.tempmax_c10 = p.high_c10;
  h.hyst_c10 = p.hyst_c10;
  h.dwell_s = p.dwell_s;
  h.period_s = (uint16_t)config().sample_period_s;
  h.schema = (uint16_t)config_schema();
  h.cal_offset_c100 = config().cal_offset_c100;
  h.cal_gain_ppm = config().cal_gain_ppm;
  h.cal_version = (uint32_t)config().cal_version;
  snprintf(h.fw, sizeof(h.fw), "%s", esp_app_get_description()->version);
  snprintf(h.sn, sizeof(h.sn), "%s", g_sn);

  // Header first, then the NVS flag that says a trip is running. A crash
  // between the two leaves a header with no trip running, which reads as
  // a trip that was started and never used -- not as one to resume.
  const TripErr e = put(snapshot(RE_TRIP_START, 0), &h, nullptr, true);
  if (e != TripErr::Ok) return e;
  nvs_put("next_id", id + 1);
  nvs_put("last", id);
  nvs_put("active", id);
  g_active = true;
  g_last_id = id;
  if (id_out) *id_out = id;
  return TripErr::Ok;
}

TripErr trip_stop(uint8_t reason) {
  if (!g_mx) return TripErr::NotActive;
  Lock l;
  if (!g_active) return TripErr::NotActive;

  RowSummary sum = {};
  sum.reason = reason;
  sum.samples = g_t.samples;
  sum.min_c100 = g_t.have_temp ? g_t.min_c100 : I16_NONE;
  sum.max_c100 = g_t.have_temp ? g_t.max_c100 : I16_NONE;
  sum.alarms_raised = g_t.alarms_raised;
  sum.motion = g_t.motion;
  const TripErr e = put(snapshot(RE_TRIP_STOP, 0), nullptr, &sum);
  if (e != TripErr::Ok) return e;
  nvs_put("active", 0);
  g_active = false;
  g_alarms = 0;
  return TripErr::Ok;
}

void trip_ack_alarms(void) {
  if (!g_mx) return;
  Lock l;
  if (!g_active || !g_alarms) return;
  g_acked = true;
  event(RE_ALARM_ACK, g_alarms);
}

void trip_note_temp(TempStatus st, float c) {
  if (!g_mx) return;
  Lock l;
  const uint32_t t = now_ms();
  if (st == TempStatus::Busy) return;    // no reading taken: no news
  g_temp_this_wake = true;
  g_temp_st = st;
  g_temp_c = c;
  g_temp_at = t;
  if (!g_active) return;

  if (st == TempStatus::Ok) {
    if (g_probe_fault_logged) event(RE_PROBE_OK);
    g_probe_bad_since = 0;
    g_probe_fault_logged = false;
    clear_alarm(AL_PROBE);
    evaluate_temp(calibrated(c), t);
  } else {
    if (!g_probe_bad_since) g_probe_bad_since = t ? t : 1;
    if (!g_probe_fault_logged) {
      event(RE_PROBE_FAULT, (int32_t)st);
      g_probe_fault_logged = true;
    }
  }
}

void trip_note_motion(const AccelEvent &ev) {
  if (!g_mx) return;
  Lock l;
  if (!g_active) return;
  // Counted, not logged: the motion column of the next row says how many.
  // Ordinary handling is not an event (decided 2026-10-05); a SHOCK row
  // waits for a shock threshold.
  (void)ev;
  if (g_motion_since_row < 0xFFFF) g_motion_since_row++;
  g_t.motion++;
}

void trip_note_power(const PowerStatus &ps) {
  if (!g_mx) return;
  Lock l;
  g_pwr = ps;
  g_have_pwr = true;
  // Power plugged in or pulled out during a trip: a row each way. The
  // first look after a reset only learns the state.
  if (ps.charger_valid) {
    const int8_t usb = ps.power_good ? 1 : 0;
    if (g_usb >= 0 && usb != g_usb) event(usb ? RE_USB_IN : RE_USB_OUT);
    g_usb = usb;
  }
  if (!g_active || !ps.cell_valid) return;
  if (ps.soc_percent < TRIP_BATT_LOW_PCT) raise_alarm(AL_BATTERY, (int16_t)ps.soc_percent);
  else if (ps.soc_percent >= TRIP_BATT_OK_PCT) clear_alarm(AL_BATTERY);
}

// This product has no door (decided 2026-10-05): nothing is logged.
void trip_note_door(DoorState, uint32_t) {}

void trip_note_link(uint8_t link) {
  if (!g_mx) return;
  Lock l;
  g_link = link;
}

void trip_note_time_set(TimeSource src, uint32_t before) {
  if (!g_mx) return;
  Lock l;
  (void)src;
  event(RE_TIME_SET, (int32_t)before);
}

void trip_tick(void) {
  if (!g_mx) return;
  Lock l;
  if (!g_active) return;
  const uint32_t t = now_ms();
  if (g_probe_bad_since && t - g_probe_bad_since >= TRIP_PROBE_ALARM_MS) {
    raise_alarm(AL_PROBE, (int16_t)g_temp_st);
  }
}

void trip_sample(void) {
  if (!g_mx) return;
  Lock l;
  if (!g_active) return;
  const LogRow r = snapshot(RE_SAMPLE, 0);
  if (put(r) != TripErr::Ok) return;
  g_t.samples++;
  if (r.temp_c100 != I16_NONE) {
    if (!g_t.have_temp || r.temp_c100 < g_t.min_c100) g_t.min_c100 = r.temp_c100;
    if (!g_t.have_temp || r.temp_c100 > g_t.max_c100) g_t.max_c100 = r.temp_c100;
    g_t.have_temp = true;
  }
}

void trip_status(TripStatus *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!g_mx) return;
  Lock l;
  out->active = g_active;
  out->id = g_active ? g_id : g_last_id;
  out->params = g_p;
  out->samples = g_t.samples;
  out->alarms_active = g_alarms;
  out->alarms_raised = g_t.alarms_raised;
  out->acked = g_acked;
  out->door_opens = 0;
  out->motion_events = g_t.motion;
  out->have_temp = g_t.have_temp;
  out->temp_read_since_boot = g_temp_this_wake;
  out->temp_ok = g_temp_st == TempStatus::Ok && g_temp_at &&
                 now_ms() - g_temp_at <= 60000;
  out->temp_c = out->temp_ok ? calibrated(g_temp_c) : 0;
  out->out_of_band = g_active && out->temp_ok &&
                     (out->temp_c * 10 > g_p.high_c10 || out->temp_c * 10 < g_p.low_c10);
  out->min_c100 = g_t.min_c100;
  out->max_c100 = g_t.max_c100;
  out->lost_trips = g_lost_trips;
  out->last_alarm = g_t.alarms_raised ? g_t.last_alarm : 0xFF;
  out->last_alarm_utc = g_t.alarms_raised ? g_t.last_alarm_utc : 0;
}

uint32_t trip_last_id(void) { return g_active ? g_id : g_last_id; }

const char *trip_err_name(TripErr e) {
  switch (e) {
    case TripErr::Ok:            return "ok";
    case TripErr::AlreadyActive: return "a trip is already running";
    case TripErr::NotActive:     return "no trip is running";
    case TripErr::BadParams:     return "parameters out of range";
    case TripErr::NoLog:         return "trip log unavailable";
    case TripErr::LogFull:       return "log full and nothing may be deleted";
    case TripErr::Flash:         return "flash error";
    case TripErr::BatteryLow:    return "battery too low to start a trip: charge first";
  }
  return "?";
}

const char *alarm_name(uint8_t a) {
  switch (a) {
    case AL_TEMP_HIGH: return "temperature high";
    case AL_TEMP_LOW:  return "temperature low";
    case AL_PROBE:     return "no temperature";
    case AL_BATTERY:   return "battery low";
  }
  return "?";
}

void trip_set_retention(const TripRetention &r) { g_ret = r; }
