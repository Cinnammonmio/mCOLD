#include "logrow.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

const char *const EVENTS[RE_COUNT] = {
    "?",          "TRIP_START",  "TRIP_STOP",  "SAMPLE",   "ALARM_HIGH",
    "ALARM_LOW",  "ALARM_PROBE", "ALARM_CLEAR", "ALARM_ACK", "SHOCK",
    "PROBE_FAULT", "PROBE_OK",   "USB_IN",     "USB_OUT",  "POWER_ON",
    "POWER_OFF",  "BATTERY_LOW", "TIME_SET",   "DATA_LOST"};

const char *const ALARMS[AL_COUNT] = {"HIGH", "LOW", "PROBE", "DOOR", "BATTERY"};

double c100(int16_t v) { return v / 100.0; }

}  // namespace

size_t row_encode(const LogRow &r, const RowHeader *h, const RowSummary *s, uint8_t *buf,
                  size_t cap) {
  Writer w(buf, cap);
  w.u32(r.utc);
  w.u8(r.time_q);
  w.u16(r.boot);
  w.u32(r.tick_ms);
  w.u8(r.event);
  w.i16(r.temp_c100);
  w.u8(r.alarms);
  w.u8(r.gnss);
  w.i32(r.lat_e7);
  w.i32(r.lon_e7);
  w.u16(r.motion);
  w.u8(r.battery);
  w.u8(r.internet);
  w.i32(r.detail);
  if (r.event == RE_TRIP_START && h) {
    w.u8(ROW_HEADER_FORMAT);
    w.u32(h->trip);
    w.i16(h->tempmin_c10);
    w.i16(h->tempmax_c10);
    w.u16(h->hyst_c10);
    w.u16(h->dwell_s);
    w.u16(h->period_s);
    w.u16(h->schema);
    w.i32(h->cal_offset_c100);
    w.i32(h->cal_gain_ppm);
    w.u32(h->cal_version);
    w.str(h->fw, 16);
    w.i32(h->temp_adj_c100);
    for (int i = 0; i < 16; i++) w.u8(h->uuid[i]);
    w.u32(h->date);
    w.u16(h->number);
  } else if (r.event == RE_TRIP_STOP && s) {
    w.u8(s->reason);
    w.u32(s->samples);
    w.i16(s->min_c100);
    w.i16(s->max_c100);
    w.u16(s->alarms_raised);
    w.u32(s->motion);
  }
  return w.overflow ? 0 : w.n;
}

bool row_decode(uint8_t type, const uint8_t *p, size_t len, LogRow *r, RowHeader *h,
                RowSummary *s) {
  if (h) h->valid = false;
  if (s) s->valid = false;
  if (type != REC_ROW || len < ROW_LEN) return false;
  Reader rd(p, len);
  r->utc = rd.u32();
  r->time_q = rd.u8();
  r->boot = rd.u16();
  r->tick_ms = rd.u32();
  r->event = rd.u8();
  r->temp_c100 = rd.i16();
  r->alarms = rd.u8();
  r->gnss = rd.u8();
  r->lat_e7 = rd.i32();
  r->lon_e7 = rd.i32();
  r->motion = rd.u16();
  r->battery = rd.u8();
  r->internet = rd.u8();
  r->detail = rd.i32();
  uint8_t fmt = 0;
  if (r->event == RE_TRIP_START && h && len >= ROW_START_LEN &&
      ((fmt = rd.u8()) == 3 || fmt == 4 || fmt == ROW_HEADER_FORMAT)) {
    h->trip = rd.u32();
    h->tempmin_c10 = rd.i16();
    h->tempmax_c10 = rd.i16();
    h->hyst_c10 = rd.u16();
    h->dwell_s = rd.u16();
    h->period_s = rd.u16();
    h->schema = rd.u16();
    h->cal_offset_c100 = rd.i32();
    h->cal_gain_ppm = rd.i32();
    h->cal_version = rd.u32();
    rd.str(h->fw, 16);
    h->fw[16] = 0;
    if (fmt < 5) {
      rd.str(h->sn, 24);
      h->sn[24] = 0;
    } else {
      h->sn[0] = 0;
    }
    h->temp_adj_c100 = fmt >= 4 ? rd.i32() : 0;
    h->has_id = fmt >= 5 && len >= ROW_START_LEN5;
    if (h->has_id) {
      for (int i = 0; i < 16; i++) h->uuid[i] = rd.u8();
      h->date = rd.u32();
      h->number = rd.u16();
    } else {
      memset(h->uuid, 0, sizeof(h->uuid));
      h->date = 0;
      h->number = 0;
    }
    h->valid = true;
  } else if (r->event == RE_TRIP_STOP && s && len >= ROW_STOP_LEN) {
    s->reason = rd.u8();
    s->samples = rd.u32();
    s->min_c100 = rd.i16();
    s->max_c100 = rd.i16();
    s->alarms_raised = rd.u16();
    s->motion = rd.u32();
    s->valid = true;
  }
  return true;
}

const char *row_event_name(uint8_t e) { return e < RE_COUNT ? EVENTS[e] : "?"; }
const char *row_alarm_name(uint8_t a) { return a < AL_COUNT ? ALARMS[a] : "?"; }

const char *row_gnss_name(uint8_t g) {
  return g == GNSS_FIX ? "fix" : g == GNSS_LAST ? "last" : "none";
}

const char *row_link_name(uint8_t l) {
  return l == LINK_ONLINE ? "online" : l == LINK_WIFI_ONLY ? "wifi_only" : "offline";
}

void row_alarm_str(uint8_t bits, char *out, size_t n) {
  size_t k = 0;
  out[0] = 0;
  for (int a = 0; a < AL_COUNT; a++) {
    if (!(bits & (1u << a))) continue;
    k += snprintf(out + k, k < n ? n - k : 0, "%s%s", k ? "|" : "", ALARMS[a]);
  }
}

void row_detail_str(const LogRow &r, char *out, size_t n) {
  out[0] = 0;
  switch (r.event) {
    case RE_ALARM_CLEAR:
      snprintf(out, n, "%s", row_alarm_name((uint8_t)r.detail));
      break;
    case RE_ALARM_ACK:
      row_alarm_str((uint8_t)r.detail, out, n);
      break;
    case RE_PROBE_FAULT:
      snprintf(out, n, "probe status %ld", (long)r.detail);
      break;
    case RE_POWER_ON:
      snprintf(out, n, "reset reason %ld", (long)r.detail);
      break;
    case RE_POWER_OFF:
      snprintf(out, n, "%ld mV", (long)r.detail);
      break;
    case RE_BATTERY_LOW:
      snprintf(out, n, "%ld %%", (long)r.detail);
      break;
    case RE_TIME_SET:
      if (r.detail) snprintf(out, n, "was %lu", (unsigned long)(uint32_t)r.detail);
      else snprintf(out, n, "was unknown");
      break;
    case RE_DATA_LOST:
      snprintf(out, n, "trip %ld deleted", (long)r.detail);
      break;
    default:
      break;
  }
}

void row_time_str(uint32_t utc, int tz_min, char *out, size_t n) {
  if (!utc) {
    out[0] = 0;
    return;
  }
  const time_t local = (time_t)utc + tz_min * 60;
  struct tm tm;
  gmtime_r(&local, &tm);
  snprintf(out, n, "%02d:%02d:%02d %02d/%02d/%04d", tm.tm_hour, tm.tm_min, tm.tm_sec,
           tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
}

void uuid_str(const uint8_t u[16], char out[37]) {
  snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0],
           u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13],
           u[14], u[15]);
}

bool uuid_parse(const char *s, uint8_t u[16]) {
  if (!s || strlen(s) != 36) return false;
  int k = 0;
  for (int i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-') return false;
      continue;
    }
    const char c = s[i];
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0) return false;
    if (k & 1) u[k / 2] = (uint8_t)(u[k / 2] | v);
    else u[k / 2] = (uint8_t)(v << 4);
    k++;
  }
  return true;
}

cJSON *row_json(const LogRow &r, uint32_t seq, const char *sn, const RowHeader &h, int tz_min) {
  cJSON *o = cJSON_CreateObject();
  const bool timeok = r.time_q != 0;
  char s[40];
  if (h.has_id) {
    uuid_str(h.uuid, s);
    cJSON_AddStringToObject(o, "trip_id", s);
    snprintf(s, sizeof(s), "%08lu", (unsigned long)h.date);
    cJSON_AddStringToObject(o, "trip_date", s);
    cJSON_AddNumberToObject(o, "trip_number", h.number);
  } else {
    cJSON_AddNullToObject(o, "trip_id");
    cJSON_AddNullToObject(o, "trip_date");
    cJSON_AddNullToObject(o, "trip_number");
  }
  cJSON_AddNumberToObject(o, "seq", seq);
  // On the wire (decided 2026-10-07): no sn (the batch and the topic
  // carry it), `timestamp` is Unix seconds (no `utc`, no local text), and
  // no `detail`. The CSV keeps its readable columns.
  (void)sn;
  (void)tz_min;
  if (timeok) cJSON_AddNumberToObject(o, "timestamp", r.utc);
  else cJSON_AddNullToObject(o, "timestamp");
  cJSON_AddStringToObject(o, "event", row_event_name(r.event));
  if (r.temp_c100 == I16_NONE) cJSON_AddNullToObject(o, "temp");
  else cJSON_AddNumberToObject(o, "temp", c100(r.temp_c100));
  if (h.valid) {
    cJSON_AddNumberToObject(o, "tempmin", h.tempmin_c10 / 10.0);
    cJSON_AddNumberToObject(o, "tempmax", h.tempmax_c10 / 10.0);
  }
  row_alarm_str(r.alarms, s, sizeof(s));
  cJSON_AddStringToObject(o, "alarm", s);
  cJSON_AddBoolToObject(o, "timeok", timeok);
  cJSON_AddStringToObject(o, "gnssstate", row_gnss_name(r.gnss));
  if (r.gnss != GNSS_NONE) {
    cJSON_AddNumberToObject(o, "latitude", r.lat_e7 / 1e7);
    cJSON_AddNumberToObject(o, "longitude", r.lon_e7 / 1e7);
  } else {
    cJSON_AddNullToObject(o, "latitude");
    cJSON_AddNullToObject(o, "longitude");
  }
  cJSON_AddNumberToObject(o, "motion", r.motion);
  if (r.battery == 0xFF) cJSON_AddNullToObject(o, "battery");
  else cJSON_AddNumberToObject(o, "battery", r.battery);
  cJSON_AddStringToObject(o, "internet", row_link_name(r.internet));
  if (!timeok) {
    cJSON_AddNumberToObject(o, "boot", r.boot);
    cJSON_AddNumberToObject(o, "up_s", r.tick_ms / 1000);
  }
  return o;
}

void row_csv_header(char *out, size_t n) {
  snprintf(out, n,
           "trip_id,trip_date,trip_number,seq,sn,timestamp,utc,event,temp,tempmin,tempmax,"
           "alarm,timeok,gnssstate,latitude,longitude,motion,battery,internet,detail");
}

void row_csv(const LogRow &r, uint32_t seq, const char *sn, const RowHeader &h, int tz_min,
             char *out, size_t n) {
  const bool timeok = r.time_q != 0;
  char ts[24] = "", temp[12] = "", tmin[10] = "", tmax[10] = "", al[40], lat[16] = "",
       lon[16] = "", batt[6] = "", det[40];
  if (timeok) row_time_str(r.utc, tz_min, ts, sizeof(ts));
  if (r.temp_c100 != I16_NONE) snprintf(temp, sizeof(temp), "%.2f", c100(r.temp_c100));
  if (h.valid) {
    snprintf(tmin, sizeof(tmin), "%.1f", h.tempmin_c10 / 10.0);
    snprintf(tmax, sizeof(tmax), "%.1f", h.tempmax_c10 / 10.0);
  }
  row_alarm_str(r.alarms, al, sizeof(al));
  if (r.gnss != GNSS_NONE) {
    snprintf(lat, sizeof(lat), "%.7f", r.lat_e7 / 1e7);
    snprintf(lon, sizeof(lon), "%.7f", r.lon_e7 / 1e7);
  }
  if (r.battery != 0xFF) snprintf(batt, sizeof(batt), "%u", r.battery);
  row_detail_str(r, det, sizeof(det));
  char utc[12] = "";
  if (timeok) snprintf(utc, sizeof(utc), "%lu", (unsigned long)r.utc);
  char tid[37] = "", tdate[10] = "", tnum[8] = "";
  if (h.has_id) {
    uuid_str(h.uuid, tid);
    snprintf(tdate, sizeof(tdate), "%08lu", (unsigned long)h.date);
    snprintf(tnum, sizeof(tnum), "%u", h.number);
  }
  snprintf(out, n, "%s,%s,%s,%lu,%s,%s,%s,%s,%s,%s,%s,%s,%d,%s,%s,%s,%u,%s,%s,%s", tid, tdate,
           tnum, (unsigned long)seq, sn, ts, utc, row_event_name(r.event), temp,
           tmin, tmax, al, timeok ? 1 : 0, row_gnss_name(r.gnss), lat, lon, r.motion, batt,
           row_link_name(r.internet), det);
}

void row_file_name(const char *sn, const LogRow &start, uint32_t trip, int tz_min, char *out,
                   size_t n) {
  const char *id = sn ? sn : "";
  if (start.time_q && start.utc) {
    const time_t local = (time_t)start.utc + tz_min * 60;
    struct tm tm;
    gmtime_r(&local, &tm);
    snprintf(out, n, "TRIP_%s_%02d%02d%02d%02d%02d.csv", id, tm.tm_year % 100, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min);
  } else {
    snprintf(out, n, "TRIP_%s_%lu.csv", id, (unsigned long)trip);
  }
}

namespace {
bool digits(const char *p, int n) {
  for (int i = 0; i < n; i++) {
    if (!isdigit((unsigned char)p[i])) return false;
  }
  return true;
}
}  // namespace

bool sn_pattern_ok(const char *s) {
  if (!s || strlen(s) != 20) return false;
  if (!isalpha((unsigned char)s[0]) || !isalpha((unsigned char)s[1]) ||
      !isalpha((unsigned char)s[2]) || s[3] != 'V' || !isdigit((unsigned char)s[4])) {
    return false;
  }
  if (s[5] != '-' || s[6] != 'L' || !digits(s + 7, 4) || s[11] != '-' || !digits(s + 12, 4) ||
      s[16] != '-' || !digits(s + 17, 3)) {
    return false;
  }
  const int lot = (s[7] - '0') * 10 + (s[8] - '0');
  const int month = (s[12] - '0') * 10 + (s[13] - '0');
  const int unit = (s[17] - '0') * 100 + (s[18] - '0') * 10 + (s[19] - '0');
  return lot >= 1 && month >= 1 && month <= 12 && unit >= 1;
}

void sn_usb_label(const char *sn, char *out, size_t n) {
  if (n < 12) {
    if (n) out[0] = 0;
    return;
  }
  if (sn_pattern_ok(sn)) {
    snprintf(out, n, "%c%c%cL%.4s%.3s", toupper((unsigned char)sn[0]),
             toupper((unsigned char)sn[1]), sn[4], sn + 7, sn + 17);
    return;
  }
  size_t k = 0;
  for (const char *p = sn ? sn : ""; *p && k < 11; p++) {
    if (isalnum((unsigned char)*p)) out[k++] = (char)toupper((unsigned char)*p);
  }
  out[k] = 0;
}
