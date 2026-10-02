#include "trip.h"

#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "features.h"
#include "flashlog.h"
#include "gnss.h"
#include "health.h"
#include "record.h"
#include "timekeep.h"

namespace {

const char *NS = "trip";

SemaphoreHandle_t g_mx = nullptr;
char g_sn[16] = "";

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
  uint16_t door_opens;
  uint32_t door_open_ms;
  uint32_t motion;
};
Totals g_t = {};

// Latest inputs.
TempStatus g_temp_st = TempStatus::NoData;
float g_temp_c = 0;
uint32_t g_temp_at = 0;
uint32_t g_probe_bad_since = 0;      // 0: probe fine (or never seen bad)
bool g_probe_fault_logged = false;
PowerStatus g_pwr = {};
bool g_have_pwr = false;
uint16_t g_motion_since_sample = 0;
uint32_t g_motion_event_at = 0;      // last EV_MOTION written
uint16_t g_motion_burst = 0;
uint32_t g_door_opened_at = 0;       // 0: door not open

// Alarm engine.
uint16_t g_alarms = 0;
bool g_acked = false;
uint32_t g_high_since = 0, g_low_since = 0;
uint32_t g_lost_trips = 0;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

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

void stamp(Writer &w) {
  TimeStamp t;
  time_now(&t);
  w.u32(t.quality == TimeSource::None ? 0 : (uint32_t)(t.utc_ms / 1000));
  w.u8((uint8_t)t.quality);
  w.u16((uint16_t)t.boot);
  w.u32(t.tick_ms);
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
  bool noted;          // the EV_LOSS record is in the log
  bool lost;           // false: the server had it all, nothing was lost
};

bool write_loss(const Loss &v) {
  uint8_t buf[LOG_PAYLOAD_MAX];
  Writer w(buf, sizeof(buf));
  stamp(w);
  w.u8(EV_LOSS);
  w.u32(v.trip);
  w.u32(v.records);
  w.u8(1);   // FLASH_RETENTION_NO_SD: there is no SD path yet
  return flashlog_append(g_id, REC_EVENT, buf, w.n, nullptr) == LogErr::Ok;
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
TripErr put(uint8_t type, const Writer &w, bool starting = false) {
  if (w.overflow) return TripErr::Flash;
  LogErr e = flashlog_append(g_id, type, w.p, w.n, nullptr);
  Loss v;
  if (e == LogErr::Full && evict_one(!starting, &v)) {
    e = flashlog_append(g_id, type, w.p, w.n, nullptr);
    if (e == LogErr::Ok && v.lost && !v.noted) write_loss(v);
  }
  switch (e) {
    case LogErr::Ok:   return TripErr::Ok;
    case LogErr::Full: return TripErr::LogFull;
    default:           return TripErr::Flash;
  }
}

void event(uint8_t code, void (*more)(Writer &, const void *), const void *arg) {
  if (!g_active) return;
  uint8_t buf[LOG_PAYLOAD_MAX];
  Writer w(buf, sizeof(buf));
  stamp(w);
  w.u8(code);
  if (more) more(w, arg);
  put(REC_EVENT, w);
}

void event_u8(uint8_t code, uint8_t v) {
  event(code, [](Writer &w, const void *a) { w.u8(*(const uint8_t *)a); }, &v);
}

// Arguments carried into the event writers below.
struct AlarmArg { uint8_t a; int16_t v; };
struct MotionArg { uint8_t src; uint16_t n; };
struct TimeArg { uint8_t s; uint32_t b; };

// ---- alarms ----------------------------------------------------------

void raise_alarm(uint8_t a, int16_t value) {
  if (g_alarms & (1u << a)) return;
  g_alarms |= (uint16_t)(1u << a);
  g_acked = false;
  g_t.alarms_raised++;
  const AlarmArg x = {a, value};
  event(EV_ALARM_RAISE, [](Writer &w, const void *p) {
    const auto *x = (const AlarmArg *)p;
    w.u8(x->a);
    w.i16(x->v);
  }, &x);
  printf("[trip] ALARM %s\n", alarm_name(a));
}

void clear_alarm(uint8_t a) {
  if (!(g_alarms & (1u << a))) return;
  g_alarms &= (uint16_t)~(1u << a);
  event_u8(EV_ALARM_CLEAR, a);
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
  uint16_t door_opens_last;
  uint16_t alarms;     // active at the last record, replayed from events
  bool acked;
};

bool rebuild_visit(const LogRecord &r, void *ctx) {
  Rebuild *b = (Rebuild *)ctx;
  Reader rd(r.payload, r.len);
  rd.n = STAMP_LEN;
  switch (r.type) {
    case REC_TRIP_START: {
      rd.u8();       // header format
      rd.u32();      // trip id
      b->p.low_c10 = rd.i16();
      b->p.high_c10 = rd.i16();
      b->p.hyst_c10 = rd.u16();
      b->p.dwell_s = rd.u16();
      b->p.door_alarm_s = rd.u16();
      b->have_header = true;
      break;
    }
    case REC_SAMPLE: {
      b->t.samples++;
      const uint8_t st = rd.u8();
      rd.u16();
      const int16_t c = rd.i16();
      if (st == 0 && c != I16_NONE) {
        if (!b->t.have_temp || c < b->t.min_c100) b->t.min_c100 = c;
        if (!b->t.have_temp || c > b->t.max_c100) b->t.max_c100 = c;
        b->t.have_temp = true;
      }
      rd.u8();
      b->door_opens_last = rd.u16();
      b->t.motion += rd.u16();
      break;
    }
    case REC_EVENT: {
      const uint8_t code = rd.u8();
      // Alarm state is replayed, not reset: an alarm that was raised and
      // acknowledged before the reset is still raised and still
      // acknowledged after it. Re-raising it would alert someone a second
      // time for the same excursion and count it twice.
      if (code == EV_ALARM_RAISE) {
        b->t.alarms_raised++;
        const uint8_t a = rd.u8();
        if (a < AL_COUNT) b->alarms |= (uint16_t)(1u << a);
        b->acked = false;
      } else if (code == EV_ALARM_CLEAR) {
        const uint8_t a = rd.u8();
        if (a < AL_COUNT) b->alarms &= (uint16_t)~(1u << a);
      } else if (code == EV_ALARM_ACK) {
        b->acked = true;
      }
      if (code == EV_DOOR_OPEN) b->t.door_opens++;
      if (code == EV_DOOR_CLOSE) b->t.door_open_ms += rd.u32();
      break;
    }
  }
  return true;
}

void reset_inputs(void) {
  g_alarms = 0;
  g_acked = false;
  g_high_since = g_low_since = 0;
  g_motion_since_sample = 0;
  g_motion_event_at = 0;
  g_motion_burst = 0;
  g_probe_bad_since = 0;
  g_probe_fault_logged = false;
  g_door_opened_at = door_state() == DoorState::Open ? now_ms() : 0;
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
  event_u8(EV_RESUMED, (uint8_t)esp_reset_reason());
  printf("[trip] resumed trip %08lX after a reset (%lu samples so far)\n",
         (unsigned long)id, (unsigned long)g_t.samples);
}

TripErr trip_start(const TripParams &p, uint32_t *id_out) {
  if (!g_mx) return TripErr::NoLog;
  Lock l;
  if (g_active) return TripErr::AlreadyActive;
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
  if (!MCOLD_DOOR) g_p.door_alarm_s = 0;   // no door, no door alarm
  g_t = {};
  reset_inputs();

  uint8_t buf[LOG_PAYLOAD_MAX];
  Writer w(buf, sizeof(buf));
  stamp(w);
  w.u8(1);
  w.u32(id);
  w.i16(p.low_c10);
  w.i16(p.high_c10);
  w.u16(p.hyst_c10);
  w.u16(p.dwell_s);
  w.u16(g_p.door_alarm_s);
  w.u16((uint16_t)config().sample_period_s);
  w.u16((uint16_t)config_schema());
  w.i32(config().cal_offset_c100);
  w.i32(config().cal_gain_ppm);
  w.u32((uint32_t)config().cal_version);
  w.str(esp_app_get_description()->version, 16);
  w.str(g_sn, 12);

  // Header first, then the NVS flag that says a trip is running. A crash
  // between the two leaves a header with no trip running, which reads as
  // a trip that was started and never used -- not as one to resume.
  const TripErr e = put(REC_TRIP_START, w, true);
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

  // A door still open at the stop closes its account here.
  if (g_door_opened_at) g_t.door_open_ms += now_ms() - g_door_opened_at;

  uint8_t buf[LOG_PAYLOAD_MAX];
  Writer w(buf, sizeof(buf));
  stamp(w);
  w.u8(reason);
  w.u32(g_t.samples);
  w.i16(g_t.have_temp ? g_t.min_c100 : I16_NONE);
  w.i16(g_t.have_temp ? g_t.max_c100 : I16_NONE);
  w.u16(g_t.alarms_raised);
  w.u16(g_t.door_opens);
  w.u32(g_t.door_open_ms / 1000);
  w.u32(g_t.motion);
  const TripErr e = put(REC_TRIP_STOP, w);
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
  const uint16_t a = g_alarms;
  event(EV_ALARM_ACK, [](Writer &w, const void *p) {
    w.u16(*(const uint16_t *)p);
  }, &a);
}

void trip_note_temp(TempStatus st, float c) {
  if (!g_mx) return;
  Lock l;
  const uint32_t t = now_ms();
  if (st == TempStatus::Busy) return;    // no reading taken: no news
  g_temp_st = st;
  g_temp_c = c;
  g_temp_at = t;
  if (!g_active) return;

  if (st == TempStatus::Ok) {
    if (g_probe_fault_logged) event(EV_PROBE_OK, nullptr, nullptr);
    g_probe_bad_since = 0;
    g_probe_fault_logged = false;
    clear_alarm(AL_PROBE);
    evaluate_temp(calibrated(c), t);
  } else {
    if (!g_probe_bad_since) g_probe_bad_since = t ? t : 1;
    if (!g_probe_fault_logged) {
      event_u8(EV_PROBE_FAULT, (uint8_t)st);
      g_probe_fault_logged = true;
    }
  }
}

void trip_note_motion(const AccelEvent &ev) {
  if (!g_mx) return;
  Lock l;
  if (!g_active) return;
  if (g_motion_since_sample < 0xFFFF) g_motion_since_sample++;
  g_t.motion++;
  g_motion_burst++;
  const uint32_t t = now_ms();
  if (g_motion_event_at && t - g_motion_event_at < TRIP_MOTION_EVENT_MS) return;
  g_motion_event_at = t ? t : 1;
  const MotionArg x = {ev.raw_src, g_motion_burst};
  g_motion_burst = 0;
  event(EV_MOTION, [](Writer &w, const void *p) {
    const auto *x = (const MotionArg *)p;
    w.u8(x->src);
    w.u16(x->n);
  }, &x);
}

void trip_note_power(const PowerStatus &ps) {
  if (!g_mx) return;
  Lock l;
  g_pwr = ps;
  g_have_pwr = true;
  if (!g_active || !ps.cell_valid) return;
  if (ps.soc_percent < TRIP_BATT_LOW_PCT) raise_alarm(AL_BATTERY, (int16_t)ps.soc_percent);
  else if (ps.soc_percent >= TRIP_BATT_OK_PCT) clear_alarm(AL_BATTERY);
}

void trip_note_door(DoorState now, uint32_t lasted_ms) {
  if (!g_mx) return;
  Lock l;
  if (now == DoorState::Open) {
    g_door_opened_at = now_ms();
    if (!g_door_opened_at) g_door_opened_at = 1;
    if (!g_active) return;
    g_t.door_opens++;
    event(EV_DOOR_OPEN, nullptr, nullptr);
  } else if (now == DoorState::Closed) {
    g_door_opened_at = 0;
    if (!g_active) return;
    g_t.door_open_ms += lasted_ms;
    event(EV_DOOR_CLOSE, [](Writer &w, const void *p) {
      w.u32(*(const uint32_t *)p);
    }, &lasted_ms);
    clear_alarm(AL_DOOR);
  }
}

void trip_note_time_set(TimeSource src, uint32_t before) {
  if (!g_mx) return;
  Lock l;
  const TimeArg x = {(uint8_t)src, before};
  event(EV_TIME_SET, [](Writer &w, const void *p) {
    const auto *x = (const TimeArg *)p;
    w.u8(x->s);
    w.u32(x->b);
  }, &x);
}

void trip_tick(void) {
  if (!g_mx) return;
  Lock l;
  if (!g_active) return;
  const uint32_t t = now_ms();
  if (MCOLD_DOOR && g_p.door_alarm_s && g_door_opened_at &&
      t - g_door_opened_at >= (uint32_t)g_p.door_alarm_s * 1000) {
    raise_alarm(AL_DOOR, (int16_t)((t - g_door_opened_at) / 1000 > 32767
                                 ? 32767 : (t - g_door_opened_at) / 1000));
  }
  if (g_probe_bad_since && t - g_probe_bad_since >= TRIP_PROBE_ALARM_MS) {
    raise_alarm(AL_PROBE, (int16_t)g_temp_st);
  }
}

void trip_sample(void) {
  if (!g_mx) return;
  Lock l;
  if (!g_active) return;
  const uint32_t t = now_ms();

  uint8_t buf[LOG_PAYLOAD_MAX];
  Writer w(buf, sizeof(buf));
  stamp(w);

  // A reading more than a minute old is not this sample's reading.
  const bool fresh = g_temp_at && t - g_temp_at <= 60000;
  const TempStatus st = fresh ? g_temp_st : TempStatus::NoData;
  w.u8((uint8_t)st);
  if (st == TempStatus::Ok) {
    const float cal = calibrated(g_temp_c);
    const int16_t c100 = (int16_t)lroundf(cal * 100.0f);
    w.u16((uint16_t)lroundf(g_temp_c * 4.0f));
    w.i16(c100);
    if (!g_t.have_temp || c100 < g_t.min_c100) g_t.min_c100 = c100;
    if (!g_t.have_temp || c100 > g_t.max_c100) g_t.max_c100 = c100;
    g_t.have_temp = true;
  } else {
    w.u16(U16_NONE);
    w.i16(I16_NONE);
  }

  w.u8((uint8_t)door_state());
  w.u16(g_t.door_opens);
  w.u16(g_motion_since_sample);

  GnssFix f;
  const bool any = gnss_last_fix(&f);
  w.u8((uint8_t)((any && f.valid ? 1 : 0) | (any ? 2 : 0)));
  w.i32(any ? (int32_t)llround(f.lat_deg * 1e7) : 0);
  w.i32(any ? (int32_t)llround(f.lon_deg * 1e7) : 0);
  const uint32_t age = any && f.at_ms ? (t - f.at_ms) / 1000 : 0xFFFF;
  w.u16(age > 0xFFFE ? U16_NONE : (uint16_t)age);
  w.u8(any ? f.sats : 0);
  w.u8(any && f.hdop > 0 && f.hdop < 25 ? (uint8_t)lroundf(f.hdop * 10) : 0xFF);

  w.u16(g_have_pwr && g_pwr.cell_valid ? (uint16_t)lroundf(g_pwr.cell_volts * 1000) : 0);
  w.u8(g_have_pwr && g_pwr.cell_valid ? (uint8_t)lroundf(g_pwr.soc_percent) : 0xFF);
  w.i16(g_have_pwr && g_pwr.current_valid ? (int16_t)lroundf(g_pwr.battery_ma)
                                          : I16_NONE);
  w.u8(g_have_pwr && g_pwr.charger_valid ? (uint8_t)g_pwr.charge : 0);
  w.u16(g_alarms);

  uint16_t unhealthy = 0;
  for (int i = 0; i < (int)Dev::Count && i < 16; i++) {
    const DevState s = health_state((Dev)i);
    if (s == DevState::Degraded || s == DevState::Failed) unhealthy |= (uint16_t)(1u << i);
  }
  w.u16(unhealthy);

  if (put(REC_SAMPLE, w) == TripErr::Ok) {
    g_t.samples++;
    g_motion_since_sample = 0;
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
  out->door_opens = g_t.door_opens;
  out->motion_events = g_t.motion;
  out->have_temp = g_t.have_temp;
  out->temp_read_since_boot = g_temp_at != 0;
  out->temp_ok = g_temp_st == TempStatus::Ok && g_temp_at &&
                 now_ms() - g_temp_at <= 60000;
  out->temp_c = out->temp_ok ? calibrated(g_temp_c) : 0;
  out->out_of_band = g_active && out->temp_ok &&
                     (out->temp_c * 10 > g_p.high_c10 || out->temp_c * 10 < g_p.low_c10);
  out->min_c100 = g_t.min_c100;
  out->max_c100 = g_t.max_c100;
  out->lost_trips = g_lost_trips;
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
  }
  return "?";
}

const char *alarm_name(uint8_t a) {
  switch (a) {
    case AL_TEMP_HIGH: return "temperature high";
    case AL_TEMP_LOW:  return "temperature low";
    case AL_PROBE:     return "no temperature";
    case AL_DOOR:      return "door open too long";
    case AL_BATTERY:   return "battery low";
  }
  return "?";
}

void trip_set_retention(const TripRetention &r) { g_ret = r; }
