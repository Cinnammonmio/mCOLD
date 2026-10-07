#include "rpc.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "rxlog.h"
#include "auth.h"
#include "config.h"
#include "flashlog.h"
#include "gnss.h"
#include "health.h"
#include "logrow.h"
#include "settings.h"
#include "record.h"
#include "temp.h"
#include "timekeep.h"
#include "net.h"
#include "trip.h"
#include "uplink.h"

namespace {

char g_sn[SN_LEN] = "";
PowerStatus g_pwr = {};
bool g_have_pwr = false;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t g_mx = nullptr;   // one request at a time, any transport
esp_timer_handle_t g_reboot = nullptr;

// Responses to the last few changing requests, by id: a retry gets the
// first answer back instead of a second action.
struct Cached {
  uint32_t id;
  char *resp;
};
const int CACHE = 8;
Cached g_cache[CACHE];
int g_cache_next = 0;

// Runs through deep sleep (timekeep.h), so times kept across one compare.
uint32_t now_ms(void) { return mono_ms(); }

// ---- building responses ------------------------------------------------

cJSON *ok(uint32_t id) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "id", id);
  cJSON_AddTrueToObject(o, "ok");
  return o;
}

cJSON *fail(uint32_t id, const char *code, const char *msg = nullptr) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "id", id);
  cJSON_AddFalseToObject(o, "ok");
  cJSON_AddStringToObject(o, "err", code);
  if (msg) cJSON_AddStringToObject(o, "msg", msg);
  return o;
}

// A number the device does not have is null, never 0.
void num_or_null(cJSON *o, const char *k, bool have, double v) {
  if (have) cJSON_AddNumberToObject(o, k, v);
  else cJSON_AddNullToObject(o, k);
}

double r2(double v) { return round(v * 100.0) / 100.0; }

const char *quality_name(TimeSource q) {
  switch (q) {
    case TimeSource::Rtc:  return "rtc";
    case TimeSource::Gnss: return "gnss";
    case TimeSource::Host: return "host";
    case TimeSource::Ntp:  return "ntp";
    default:               return "none";
  }
}

const char *alarm_code(int a) {
  switch (a) {
    case AL_TEMP_HIGH: return "TEMP_HIGH";
    case AL_TEMP_LOW:  return "TEMP_LOW";
    case AL_PROBE:     return "NO_TEMP";
    case AL_DOOR:      return "DOOR";
    case AL_BATTERY:   return "BATTERY_LOW";
  }
  return "?";
}

void add_alarms(cJSON *o, const char *k, uint16_t bits) {
  cJSON *a = cJSON_AddArrayToObject(o, k);
  for (int i = 0; i < AL_COUNT; i++) {
    if (bits & (1u << i)) cJSON_AddItemToArray(a, cJSON_CreateString(alarm_code(i)));
  }
}

const char *trip_err_code(TripErr e) {
  switch (e) {
    case TripErr::AlreadyActive: return "ALREADY_ACTIVE";
    case TripErr::NotActive:     return "NOT_ACTIVE";
    case TripErr::BadParams:     return "BAD_ARGS";
    case TripErr::NoLog:         return "NO_LOG";
    case TripErr::LogFull:       return "LOG_FULL";
    case TripErr::BatteryLow:    return "BATTERY_LOW";
    default:                     return "FLASH";
  }
}

bool get_num(const cJSON *req, const char *k, double *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, k);
  if (!cJSON_IsNumber(v)) return false;
  *out = v->valuedouble;
  return true;
}

// ---- commands ----------------------------------------------------------

void hex(const uint8_t *b, size_t n, char *out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = H[b[i] >> 4];
    out[2 * i + 1] = H[b[i] & 15];
  }
  out[2 * n] = 0;
}

cJSON *c_info(uint32_t id, const cJSON *, RpcSession *s) {
  cJSON *o = ok(id);
  cJSON_AddNumberToObject(o, "proto", RPC_PROTO);
  cJSON_AddStringToObject(o, "sn", g_sn);
  cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
  cJSON_AddStringToObject(o, "hw", "Foam V.1");
  cJSON_AddStringToObject(o, "idf", esp_app_get_description()->idf_ver);
  cJSON_AddNumberToObject(o, "boot", time_boot_count());
  cJSON_AddNumberToObject(o, "uptime_s", now_ms() / 1000);
  // This connection's challenge for AUTH. Fresh per connection, so a
  // proof overheard on one is worthless on the next.
  if (s && s->has_nonce) {
    char n[33];
    hex(s->nonce, 16, n);
    cJSON_AddStringToObject(o, "nonce", n);
  }
  cJSON_AddBoolToObject(o, "authorized", s && s->authorized);
  return o;
}

cJSON *c_auth(uint32_t id, const cJSON *req, RpcSession *s) {
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(req, "proof");
  if (!s || !s->has_nonce) {
    return fail(id, "BAD_REQUEST", "this link needs no AUTH");
  }
  if (!cJSON_IsString(p) || !auth_check(s->nonce, p->valuestring)) {
    // No hint which part was wrong. The key may simply be spent: it is
    // replaced after every session and at every reset.
    return fail(id, "NOT_AUTHORIZED", "tap the box again and retry");
  }
  s->authorized = true;
  return ok(id);
}

// A trip as the app and the server name it (decided 2026-10-07): trip_id,
// a UUID the box made; trip_date, YYYYMMDD, local; trip_number, the day's
// running number. The box's own counter does not leave it.
void add_trip_ident(cJSON *o, const char *uuid, uint32_t date, uint16_t number) {
  if (uuid && *uuid) {
    char d[10];
    snprintf(d, sizeof(d), "%08lu", (unsigned long)date);
    cJSON_AddStringToObject(o, "trip_id", uuid);
    cJSON_AddStringToObject(o, "trip_date", d);
    cJSON_AddNumberToObject(o, "trip_number", number);
  } else {
    cJSON_AddNullToObject(o, "trip_id");
    cJSON_AddNullToObject(o, "trip_date");
    cJSON_AddNullToObject(o, "trip_number");
  }
}

void add_trip_ident(cJSON *o, const RowHeader &h) {
  char u[37] = "";
  if (h.has_id) uuid_str(h.uuid, u);
  add_trip_ident(o, u, h.date, h.number);
}

// The request's trip_id, as the box's own id and its header. False, with
// the answer to send, if there is none or the box does not have it.
bool req_trip(uint32_t id, const cJSON *req, uint32_t *internal, RowHeader *h, cJSON **err) {
  const cJSON *j = req ? cJSON_GetObjectItemCaseSensitive(req, "trip_id") : nullptr;
  if (!cJSON_IsString(j)) {
    *err = fail(id, "BAD_ARGS", "trip_id (a UUID)");
    return false;
  }
  *internal = trip_find(j->valuestring);
  if (!*internal || !trip_info(*internal, h)) {
    *err = fail(id, "BAD_ARGS", "no such trip in the log");
    return false;
  }
  return true;
}

cJSON *c_status(uint32_t id, const cJSON *, RpcSession *) {
  cJSON *o = ok(id);

  TimeStamp t;
  time_now(&t);
  cJSON *tm = cJSON_AddObjectToObject(o, "time");
  num_or_null(tm, "utc", t.quality != TimeSource::None, (double)(t.utc_ms / 1000));
  cJSON_AddStringToObject(tm, "quality", quality_name(t.quality));
  cJSON_AddNumberToObject(tm, "boot", t.boot);

  TripStatus s;
  trip_status(&s);
  cJSON *te = cJSON_AddObjectToObject(o, "temp");
  cJSON_AddBoolToObject(te, "ok", s.temp_ok);
  num_or_null(te, "c", s.temp_ok, r2(s.temp_c));

  cJSON *tr = cJSON_AddObjectToObject(o, "trip");
  cJSON_AddBoolToObject(tr, "active", s.active);
  add_trip_ident(tr, s.trip_id, s.trip_date, s.trip_number);
  cJSON_AddNumberToObject(tr, "samples", s.samples);
  num_or_null(tr, "min", s.have_temp, s.min_c100 / 100.0);
  num_or_null(tr, "max", s.have_temp, s.max_c100 / 100.0);
  add_alarms(tr, "alarms", s.alarms_active);
  cJSON_AddBoolToObject(tr, "acked", s.acked);
  // What happened while nobody could see: a server that was out of reach
  // learns from these that an alarm came and went (decided 2026-10-05).
  cJSON_AddNumberToObject(tr, "alarms_raised", s.alarms_raised);
  if (s.last_alarm != 0xFF) {
    cJSON *la = cJSON_AddObjectToObject(tr, "last_alarm");
    cJSON_AddStringToObject(la, "type", row_alarm_name(s.last_alarm));
    num_or_null(la, "utc", s.last_alarm_utc != 0, s.last_alarm_utc);
  } else {
    cJSON_AddNullToObject(tr, "last_alarm");
  }

  PowerStatus p;
  bool have;
  portENTER_CRITICAL(&g_mux);
  p = g_pwr;
  have = g_have_pwr;
  portEXIT_CRITICAL(&g_mux);
  cJSON *pw = cJSON_AddObjectToObject(o, "power");
  num_or_null(pw, "soc", have && p.cell_valid, round(p.soc_percent));
  num_or_null(pw, "mv", have && p.cell_valid, round(p.cell_volts * 1000));
  num_or_null(pw, "ma", have && p.current_valid, round(p.battery_ma));
  const char *ch = "none";
  if (have && p.charger_valid) {
    switch (p.charge) {
      case ChargeState::PreCharge:  ch = "pre"; break;
      case ChargeState::FastCharge: ch = "fast"; break;
      case ChargeState::Done:       ch = "done"; break;
      default:                      ch = "none"; break;
    }
  }
  cJSON_AddStringToObject(pw, "charge", ch);
  cJSON_AddBoolToObject(pw, "external", have && p.charger_valid && p.power_good);

  GnssFix f;
  const bool any = gnss_last_fix(&f);
  cJSON *g = cJSON_AddObjectToObject(o, "gnss");
  cJSON_AddBoolToObject(g, "fix", any && f.valid && now_ms() - f.at_ms < 30 * 60000);
  num_or_null(g, "lat", any, any ? f.lat_deg : 0);
  num_or_null(g, "lon", any, any ? f.lon_deg : 0);
  num_or_null(g, "age_s", any, any ? (now_ms() - f.at_ms) / 1000 : 0);

  LogStats ls;
  flashlog_stats(&ls);
  cJSON *st = cJSON_AddObjectToObject(o, "storage");
  cJSON_AddNumberToObject(st, "used_pct",
                          ls.sectors ? (ls.used * 100 + ls.sectors - 1) / ls.sectors : 0);

  // Which firmware, so the server knows who has taken an update.
  cJSON *fw = cJSON_AddObjectToObject(o, "fw");
  cJSON_AddStringToObject(fw, "ver", esp_app_get_description()->version);
  const esp_partition_t *rp = esp_ota_get_running_partition();
  cJSON_AddStringToObject(fw, "slot", rp ? rp->label : "?");

  NetStatus n;
  net_status(&n);
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char ms[18];
  snprintf(ms, sizeof(ms), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  cJSON *ne = cJSON_AddObjectToObject(o, "net");
  cJSON_AddBoolToObject(ne, "connected", n.connected);
  if (n.connected) {
    cJSON_AddStringToObject(ne, "ssid", n.ssid);
    cJSON_AddNumberToObject(ne, "rssi", n.rssi);
    cJSON_AddStringToObject(ne, "ip", n.ip);
  }
  cJSON_AddStringToObject(ne, "mac", ms);
  return o;
}

cJSON *c_get_config(uint32_t id, const cJSON *, RpcSession *) {
  cJSON *o = ok(id);
  cJSON *c = cJSON_AddObjectToObject(o, "config");
  for (int i = 0; i < config_count(); i++) {
    const char *k;
    int32_t v;
    if (config_at(i, &k, &v, nullptr, nullptr)) cJSON_AddNumberToObject(c, k, v);
  }
  cJSON_AddNumberToObject(o, "schema", config_schema());
  return o;
}

cJSON *c_set_config(uint32_t id, const cJSON *req, RpcSession *ses) {
  const cJSON *k = cJSON_GetObjectItemCaseSensitive(req, "key");
  double v;
  if (!cJSON_IsString(k) || !get_num(req, "value", &v) || v != floor(v)) {
    return fail(id, "BAD_ARGS", "key (string) and value (integer)");
  }
  // The app gets the keys the server gets (settings.cpp); the rest are the
  // console's.
  if (!(ses && ses->console) && !settings_remote_key(k->valuestring)) {
    return fail(id, "BAD_ARGS", "console only");
  }
  TripStatus ts;
  trip_status(&ts);
  if (ts.active && settings_trip_locked(k->valuestring)) {
    return fail(id, "ALREADY_ACTIVE", "a trip is running: change it after the trip");
  }
  if (!config_set(k->valuestring, (int32_t)v)) {
    return fail(id, "BAD_ARGS", "unknown key or out of range");
  }
  settings_touch();
  return ok(id);
}

cJSON *c_set_time(uint32_t id, const cJSON *req, RpcSession *) {
  double utc;
  // Not before 2024, not after 2100: a phone with a broken clock must not
  // be able to stamp a trip with 1970.
  if (!get_num(req, "utc", &utc) || utc < 1704067200.0 || utc > 4102444800.0) {
    return fail(id, "BAD_ARGS", "utc: Unix seconds");
  }
  TimeStamp before;
  time_now(&before);
  const time_t s = (time_t)utc;
  struct tm tm;
  gmtime_r(&s, &tm);
  // The phone's clock does not overrule satellite time set this boot.
  const bool applied = time_set(&tm, TimeSource::Host, false);
  if (applied) {
    trip_note_time_set(TimeSource::Host, before.quality == TimeSource::None
                                             ? 0 : (uint32_t)(before.utc_ms / 1000));
  }
  cJSON *o = ok(id);
  cJSON_AddBoolToObject(o, "applied", applied);
  TimeStamp now;
  time_now(&now);
  cJSON_AddStringToObject(o, "quality", quality_name(now.quality));
  return o;
}

cJSON *c_start(uint32_t id, const cJSON *req, RpcSession *) {
  // A START retried after the device reset: the stored id says which
  // trip it already started.
  nvs_handle_t h;
  uint32_t last_id = 0, last_trip = 0;
  if (nvs_open("rpc", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, "start_id", &last_id);
    nvs_get_u32(h, "start_trip", &last_trip);
    nvs_close(h);
  }
  TripStatus s;
  trip_status(&s);
  if (id == last_id && s.active && s.id == last_trip) {
    cJSON *o = ok(id);
    add_trip_ident(o, s.trip_id, s.trip_date, s.trip_number);
    return o;
  }

  double lo, hi, hyst = 0.5, dwell = 300;
  if (!get_num(req, "low", &lo) || !get_num(req, "high", &hi)) {
    return fail(id, "BAD_ARGS", "low and high, in C");
  }
  get_num(req, "hyst", &hyst);
  get_num(req, "dwell_s", &dwell);
  if (hyst < 0 || hyst > 10 || dwell < 0 || dwell > 3600) {
    return fail(id, "BAD_ARGS", "hyst 0..10 C, dwell_s 0..3600");
  }
  const TripParams p = {(int16_t)lround(lo * 10), (int16_t)lround(hi * 10),
                        (uint16_t)lround(hyst * 10), (uint16_t)dwell, 0};
  uint32_t trip = 0;
  const TripErr e = trip_start(p, &trip);
  if (e != TripErr::Ok) return fail(id, trip_err_code(e), trip_err_name(e));
  if (nvs_open("rpc", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, "start_id", id);
    nvs_set_u32(h, "start_trip", trip);
    nvs_commit(h);
    nvs_close(h);
  }
  cJSON *o = ok(id);
  TripStatus now;
  trip_status(&now);
  add_trip_ident(o, now.trip_id, now.trip_date, now.trip_number);
  return o;
}

cJSON *c_stop(uint32_t id, const cJSON *, RpcSession *) {
  TripStatus s;
  trip_status(&s);
  const TripErr e = trip_stop(2);   // reason 2: the app
  if (e != TripErr::Ok) return fail(id, trip_err_code(e), trip_err_name(e));
  cJSON *o = ok(id);
  add_trip_ident(o, s.trip_id, s.trip_date, s.trip_number);
  return o;
}

cJSON *c_ack(uint32_t id, const cJSON *, RpcSession *) {
  trip_ack_alarms();
  TripStatus s;
  trip_status(&s);
  cJSON *o = ok(id);
  add_alarms(o, "alarms", s.alarms_active);
  return o;
}

cJSON *c_list_trips(uint32_t id, const cJSON *, RpcSession *) {
  uint32_t t[64];
  const int n = flashlog_trips(t, 64);
  cJSON *o = ok(id);
  cJSON *a = cJSON_AddArrayToObject(o, "trips");
  for (int i = 0; i < n && i < 64; i++) {
    if (t[i] > TRIP_ID_REAL_MAX) continue;   // bench records are not trips
    RowHeader h;
    if (!trip_info(t[i], &h)) continue;      // from before trip_id: the app cannot name it
    cJSON *e = cJSON_CreateObject();
    add_trip_ident(e, h);
    uint32_t last;
    num_or_null(e, "last_seq", flashlog_last_seq(t[i], &last), last);
    // Delivered: the server has every row (ACKs, or the app said so).
    cJSON_AddBoolToObject(e, "sent", uplink_fully_acked(t[i]));
    cJSON_AddItemToArray(a, e);
  }
  return o;
}

// The settings document (settings.h), the same one the server sends over
// MQTT: the request's own config / wifi / mqtt / ota_base.
cJSON *c_apply_config(uint32_t id, const cJSON *req, RpcSession *) {
  cJSON *o = ok(id);
  settings_apply(req, o);
  return o;
}

cJSON *c_get_network(uint32_t id, const cJSON *, RpcSession *) {
  cJSON *o = ok(id);
  settings_network(o);
  return o;
}

// The SETTINGS event's content, asked for (the network part needs AUTH).
cJSON *c_get_settings(uint32_t id, const cJSON *, RpcSession *ses) {
  cJSON *o = ok(id);
  settings_snapshot(o, ses && ses->authorized);
  return o;
}

// The app has the whole trip and has handed it to the server itself
// ("stop and send", decided 2026-10-05): the box need not upload it.
cJSON *c_mark_delivered(uint32_t id, const cJSON *req, RpcSession *) {
  uint32_t trip;
  RowHeader h;
  cJSON *err = nullptr;
  if (!req_trip(id, req, &trip, &h, &err)) return err;
  TripStatus s;
  trip_status(&s);
  if (s.active && s.id == trip) return fail(id, "ALREADY_ACTIVE", "the trip is still running: stop it first");
  uint32_t last;
  if (!uplink_mark_delivered(trip, &last)) {
    return fail(id, "BAD_ARGS", "no such trip in the log");
  }
  cJSON *o = ok(id);
  add_trip_ident(o, h);
  cJSON_AddNumberToObject(o, "rows", last + 1);
  return o;
}

struct Summary {
  bool header, stopped, have_temp;
  int16_t lo, hi, min_c100, max_c100;
  uint16_t hyst, dwell;
  uint32_t samples, alarms, start_utc, stop_utc;
  uint8_t start_q, stop_q;
};

bool summary_visit(const LogRecord &r, void *ctx) {
  Summary *m = (Summary *)ctx;
  LogRow row;
  RowHeader h;
  if (!row_decode(r.type, r.payload, r.len, &row, &h, nullptr)) return true;
  switch (row.event) {
    case RE_TRIP_START:
      if (!h.valid) break;
      m->lo = h.tempmin_c10;
      m->hi = h.tempmax_c10;
      m->hyst = h.hyst_c10;
      m->dwell = h.dwell_s;
      m->header = true;
      m->start_utc = row.utc;
      m->start_q = row.time_q;
      break;
    case RE_SAMPLE:
      m->samples++;
      if (row.temp_c100 != I16_NONE) {
        if (!m->have_temp || row.temp_c100 < m->min_c100) m->min_c100 = row.temp_c100;
        if (!m->have_temp || row.temp_c100 > m->max_c100) m->max_c100 = row.temp_c100;
        m->have_temp = true;
      }
      break;
    case RE_ALARM_HIGH:
    case RE_ALARM_LOW:
    case RE_ALARM_PROBE:
    case RE_BATTERY_LOW:
      m->alarms++;
      break;
    case RE_TRIP_STOP:
      m->stopped = true;
      m->stop_utc = row.utc;
      m->stop_q = row.time_q;
      break;
  }
  return true;
}

cJSON *c_summary(uint32_t id, const cJSON *req, RpcSession *) {
  uint32_t trip;
  RowHeader h;
  cJSON *err = nullptr;
  if (!req_trip(id, req, &trip, &h, &err)) return err;
  Summary m = {};
  flashlog_read(trip, 0, summary_visit, &m, nullptr);
  if (!m.header) return fail(id, "BAD_ARGS", "no such trip in the log");
  cJSON *o = ok(id);
  add_trip_ident(o, h);
  cJSON_AddNumberToObject(o, "low", m.lo / 10.0);
  cJSON_AddNumberToObject(o, "high", m.hi / 10.0);
  cJSON_AddNumberToObject(o, "hyst", m.hyst / 10.0);
  cJSON_AddNumberToObject(o, "dwell_s", m.dwell);
  num_or_null(o, "start_utc", m.start_q != 0, m.start_utc);
  cJSON_AddNumberToObject(o, "samples", m.samples);
  num_or_null(o, "min", m.have_temp, m.min_c100 / 100.0);
  num_or_null(o, "max", m.have_temp, m.max_c100 / 100.0);
  cJSON_AddNumberToObject(o, "alarms", m.alarms);
  cJSON_AddBoolToObject(o, "stopped", m.stopped);
  num_or_null(o, "stop_utc", m.stopped && m.stop_q != 0, m.stop_utc);
  return o;
}

struct Chunk {
  cJSON *arr;
  int want, got;
  bool more;
  uint32_t next;
};

bool chunk_visit(const LogRecord &r, void *ctx) {
  Chunk *c = (Chunk *)ctx;
  if (c->got == c->want) {     // one past the end: there is more
    c->more = true;
    c->next = r.seq;
    return false;
  }
  unsigned char b64[160];
  size_t olen = 0;
  mbedtls_base64_encode(b64, sizeof(b64), &olen, r.payload, r.len);
  b64[olen] = 0;
  cJSON *e = cJSON_CreateObject();
  cJSON_AddNumberToObject(e, "seq", r.seq);
  cJSON_AddNumberToObject(e, "type", r.type);
  cJSON_AddStringToObject(e, "data", (const char *)b64);
  cJSON_AddItemToArray(c->arr, e);
  c->got++;
  return true;
}

cJSON *c_read_log(uint32_t id, const cJSON *req, RpcSession *) {
  uint32_t trip;
  RowHeader h;
  cJSON *err = nullptr;
  if (!req_trip(id, req, &trip, &h, &err)) return err;
  double from = 0, max = 8;
  get_num(req, "from", &from);
  get_num(req, "max", &max);
  if (from < 0 || max < 1 || max > 16) return fail(id, "BAD_ARGS", "from >= 0, max 1..16");
  cJSON *o = ok(id);
  Chunk c = {cJSON_AddArrayToObject(o, "records"), (int)max, 0, false, 0};
  flashlog_read(trip, (uint32_t)from, chunk_visit, &c, nullptr);
  num_or_null(o, "next", c.more, c.next);
  return o;
}

cJSON *c_storage(uint32_t id, const cJSON *, RpcSession *) {
  LogStats s;
  flashlog_stats(&s);
  cJSON *o = ok(id);
  cJSON_AddNumberToObject(o, "sectors", s.sectors);
  cJSON_AddNumberToObject(o, "used", s.used);
  cJSON_AddNumberToObject(o, "free", s.free + s.dirty);
  cJSON_AddNumberToObject(o, "trips", s.trips);
  cJSON_AddNumberToObject(o, "days_left",
                          floor((double)(s.free + s.dirty) * LOG_SLOTS *
                                config().sample_period_s / 86400.0));
  return o;
}

cJSON *c_self_test(uint32_t id, const cJSON *, RpcSession *) {
  cJSON *o = ok(id);
  cJSON *h = cJSON_AddObjectToObject(o, "health");
  for (int i = 0; i < (int)Dev::Count; i++) {
    cJSON_AddStringToObject(h, health_name((Dev)i),
                            health_state_name(health_state((Dev)i)));
  }
  return o;
}

void do_reboot(void *) { esp_restart(); }

cJSON *c_reboot(uint32_t id, const cJSON *, RpcSession *) {
  // Answered first; the restart follows once the answer has had time to
  // go out.
  if (!g_reboot) {
    esp_timer_create_args_t a = {};
    a.callback = do_reboot;
    a.name = "reboot";
    esp_timer_create(&a, &g_reboot);
  }
  esp_timer_start_once(g_reboot, 800 * 1000);
  return ok(id);
}

cJSON *c_sync_status(uint32_t id, const cJSON *, RpcSession *) {
  NetStatus n;
  net_status(&n);
  UplinkStatus u;
  uplink_status(&u);
  cJSON *o = ok(id);
  cJSON *w = cJSON_AddObjectToObject(o, "wifi");
  cJSON_AddBoolToObject(w, "configured", n.configured);
  cJSON_AddBoolToObject(w, "connected", n.connected);
  if (n.connected) cJSON_AddStringToObject(w, "ssid", n.ssid);
  // The networks it knows, by name only: passwords never leave the box.
  cJSON *k = cJSON_AddArrayToObject(w, "known");
  for (int i = 0; i < n.known; i++) {
    char s[33];
    if (net_known(i, s, sizeof(s))) cJSON_AddItemToArray(k, cJSON_CreateString(s));
  }
  num_or_null(w, "rssi", n.connected, n.rssi);
  cJSON *m = cJSON_AddObjectToObject(o, "server");
  cJSON_AddBoolToObject(m, "configured", u.configured);
  // Connected to the broker; whether the server has the data is
  // "pending", which only its ACKs move.
  cJSON_AddBoolToObject(m, "broker", u.connected);
  cJSON_AddNumberToObject(m, "pending", u.records_pending);
  num_or_null(m, "last_ack_s", u.last_ack_ms != 0, (now_ms() - u.last_ack_ms) / 1000);
  return o;
}

cJSON *c_sync_now(uint32_t id, const cJSON *, RpcSession *) {
  uplink_kick();
  return ok(id);
}

cJSON *c_set_wifi(uint32_t id, const cJSON *req, RpcSession *) {
  // The same entry as the settings document's: ssid, pass, and dhcp or
  // ip/gateway/subnet/dns.
  const char *why = nullptr;
  if (!settings_wifi_entry(req, &why)) return fail(id, "BAD_ARGS", why);
  settings_touch();
  cJSON *o = ok(id);
  cJSON_AddNumberToObject(o, "known", net_count());
  return o;
}

cJSON *c_del_wifi(uint32_t id, const cJSON *req, RpcSession *) {
  const cJSON *s = cJSON_GetObjectItemCaseSensitive(req, "ssid");
  const cJSON *sl = cJSON_GetObjectItemCaseSensitive(req, "slot");
  if (cJSON_IsNumber(sl)) {
    if (!net_slot_clear((int)sl->valuedouble - 1)) return fail(id, "BAD_ARGS", "slot 1..5, one in use");
  } else if (!cJSON_IsString(s) || !net_remove(s->valuestring)) {
    return fail(id, "BAD_ARGS", "slot (1..5) or ssid: a known network");
  }
  settings_touch();
  return ok(id);
}

cJSON *c_later(uint32_t id, const cJSON *, RpcSession *) {
  return fail(id, "NOT_SUPPORTED", "arrives with the USB drive (P6)");
}

struct Cmd {
  const char *name;
  bool changes;     // needs an authorized link; idempotent by id
  cJSON *(*fn)(uint32_t, const cJSON *, RpcSession *);
};

const Cmd CMDS[] = {
    {"GET_INFO", false, c_info},
    {"AUTH", false, c_auth},
    {"GET_STATUS", false, c_status},
    {"GET_CONFIG", false, c_get_config},
    {"SET_CONFIG", true, c_set_config},
    {"APPLY_CONFIG", true, c_apply_config},
    {"GET_NETWORK", true, c_get_network},
    {"GET_SETTINGS", false, c_get_settings},
    {"SET_TIME", true, c_set_time},
    {"START_TRIP", true, c_start},
    {"STOP_TRIP", true, c_stop},
    {"ACK_ALARM", true, c_ack},
    {"LIST_TRIPS", false, c_list_trips},
    {"MARK_DELIVERED", true, c_mark_delivered},
    {"GET_TRIP_SUMMARY", false, c_summary},
    {"READ_LOG_CHUNK", false, c_read_log},
    {"GET_STORAGE_STATUS", false, c_storage},
    {"SELF_TEST", false, c_self_test},
    {"REBOOT", true, c_reboot},
    {"GET_SYNC_STATUS", false, c_sync_status},
    {"SYNC_NOW", true, c_sync_now},
    {"SET_WIFI", true, c_set_wifi},
    {"DEL_WIFI", true, c_del_wifi},
    {"GET_USB_SNAPSHOT_STATUS", false, c_later},
};

char *print(cJSON *o) {
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  return s ? s : strdup("{\"id\":0,\"ok\":false,\"err\":\"NO_MEMORY\"}");
}

}  // namespace

void rpc_init(const char *sn) {
  snprintf(g_sn, sizeof(g_sn), "%s", sn ? sn : "");
  g_mx = xSemaphoreCreateMutex();
  memset(g_cache, 0, sizeof(g_cache));
}

void rpc_note_power(const PowerStatus &ps) {
  portENTER_CRITICAL(&g_mux);
  g_pwr = ps;
  g_have_pwr = true;
  portEXIT_CRITICAL(&g_mux);
}

static char *handle_one(const char *req, size_t n, RpcSession *s) {
  const bool authorized = s && s->authorized;
  cJSON *r = cJSON_ParseWithLength(req, n);
  const cJSON *jid = r ? cJSON_GetObjectItemCaseSensitive(r, "id") : nullptr;
  const cJSON *jcmd = r ? cJSON_GetObjectItemCaseSensitive(r, "cmd") : nullptr;
  if (!r || !cJSON_IsNumber(jid) || jid->valuedouble < 1 ||
      jid->valuedouble > 4294967295.0 || !cJSON_IsString(jcmd)) {
    cJSON_Delete(r);
    return print(fail(0, "BAD_REQUEST", "a JSON object with id (1..2^32-1) and cmd"));
  }
  const uint32_t id = (uint32_t)jid->valuedouble;
  double proto;
  if (get_num(r, "proto", &proto) && proto > RPC_PROTO) {
    cJSON_Delete(r);
    return print(fail(id, "UNSUPPORTED_PROTO"));
  }

  const Cmd *c = nullptr;
  for (const Cmd &k : CMDS) {
    if (!strcmp(k.name, jcmd->valuestring)) c = &k;
  }
  if (!c) {
    // Built before the request is freed: the name points into it.
    cJSON *e = fail(id, "UNKNOWN_CMD", jcmd->valuestring);
    cJSON_Delete(r);
    return print(e);
  }
  if (c->changes && !authorized) {
    cJSON_Delete(r);
    return print(fail(id, "NOT_AUTHORIZED", "tap the box, then AUTH"));
  }

  xSemaphoreTake(g_mx, portMAX_DELAY);
  if (c->changes) {
    for (const Cached &k : g_cache) {
      if (k.resp && k.id == id) {          // a retry: the first answer again
        char *again = strdup(k.resp);
        xSemaphoreGive(g_mx);
        cJSON_Delete(r);
        return again;
      }
    }
  }
  char *resp = print(c->fn(id, r, s));
  if (c->changes) {
    Cached &slot = g_cache[g_cache_next];
    free(slot.resp);
    slot.id = id;
    slot.resp = strdup(resp);
    g_cache_next = (g_cache_next + 1) % CACHE;
  }
  xSemaphoreGive(g_mx);
  cJSON_Delete(r);
  return resp;
}

char *rpc_handle(const char *req, size_t n, RpcSession *s) {
  char *resp = handle_one(req, n, s);
  rxlog_rpc(req, n, resp, s);    // rx_show: what came in, on the glass
  return resp;
}
