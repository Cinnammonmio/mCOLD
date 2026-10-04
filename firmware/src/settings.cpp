#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "net.h"
#include "ota.h"
#include "trip.h"
#include "uplink.h"

namespace {

// What the app and the server may set (decided 2026-10-05). The rest --
// sleep switches, light sleep, the gauge source, the measured sleep
// current, the battery limits and size, the panel insets -- belong to the
// bench and the factory, at the console.
const char *const REMOTE[] = {
    "sample_period_s", "upload_period_s", "gnss_period_s",  "idle_wake_s",
    "tz_offset_min",   "led_front_pct",   "led_bright_pct", "led_status_s",
    "buzzer_enabled",  "accel_wake_ths",  "cal_offset_c100", "cal_gain_ppm",
    "cal_version",     "cal_date",
};

// A trip's header carries these; changing them under a running trip would
// make its rows say something the header does not.
const char *const TRIP_LOCKED[] = {
    "sample_period_s", "cal_offset_c100", "cal_gain_ppm", "cal_version", "cal_date",
};

template <size_t N>
bool listed(const char *const (&list)[N], const char *key) {
  for (const char *k : list) {
    if (!strcmp(k, key)) return true;
  }
  return false;
}

bool range_of(const char *key, int32_t *lo, int32_t *hi) {
  for (int i = 0; i < config_count(); i++) {
    const char *k;
    int32_t v;
    if (config_at(i, &k, &v, lo, hi) && !strcmp(k, key)) return true;
  }
  return false;
}

void err(cJSON *errors, const char *what, const char *why) {
  cJSON_AddStringToObject(errors, what, why);
}

void did(cJSON *applied, const char *what) {
  cJSON_AddItemToArray(applied, cJSON_CreateString(what));
}

void apply_config(const cJSON *cfg, cJSON *applied, cJSON *errors) {
  if (!cJSON_IsObject(cfg)) {
    err(errors, "config", "must be an object of key: integer");
    return;
  }
  TripStatus ts;
  trip_status(&ts);
  const cJSON *it;
  cJSON_ArrayForEach(it, cfg) {
    const char *k = it->string;
    int32_t lo, hi;
    if (!settings_remote_key(k)) {
      err(errors, k, range_of(k, &lo, &hi) ? "console only" : "unknown key");
    } else if (!cJSON_IsNumber(it) || it->valuedouble != floor(it->valuedouble)) {
      err(errors, k, "not an integer");
    } else if (!range_of(k, &lo, &hi) || it->valuedouble < lo || it->valuedouble > hi) {
      err(errors, k, "out of range");
    } else if (ts.active && settings_trip_locked(k)) {
      err(errors, k, "a trip is running: send it again after the trip");
    } else if (!config_set(k, (int32_t)it->valuedouble)) {
      err(errors, k, "not stored");
    } else {
      did(applied, k);
    }
  }
}

void apply_wifi(const cJSON *w, cJSON *applied, cJSON *errors) {
  if (!cJSON_IsObject(w)) {
    err(errors, "wifi", "must be {\"add\": [...], \"del\": [...]}");
    return;
  }
  // Removals first, so a full list can make room for what is added.
  const cJSON *del = cJSON_GetObjectItemCaseSensitive(w, "del");
  const cJSON *it;
  if (del) {
    cJSON_ArrayForEach(it, del) {
      if (!cJSON_IsString(it)) continue;
      char what[48];
      snprintf(what, sizeof(what), "wifi del %s", it->valuestring);
      if (net_remove(it->valuestring)) did(applied, what);
      else err(errors, what, "not a known network");
    }
  }
  const cJSON *add = cJSON_GetObjectItemCaseSensitive(w, "add");
  if (add) {
    cJSON_ArrayForEach(it, add) {
      const cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "ssid");
      const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "pass");
      if (!cJSON_IsString(s)) {
        err(errors, "wifi add", "each entry needs an ssid");
        continue;
      }
      char what[48];
      snprintf(what, sizeof(what), "wifi add %s", s->valuestring);
      if (net_add(s->valuestring, cJSON_IsString(p) ? p->valuestring : "")) did(applied, what);
      else err(errors, what, "ssid 1-32 bytes, pass empty or 8-63, at most 5 networks");
    }
  }
}

void apply_mqtt(const cJSON *m, cJSON *applied, cJSON *errors) {
  const cJSON *h = cJSON_GetObjectItemCaseSensitive(m, "host");
  const cJSON *u = cJSON_GetObjectItemCaseSensitive(m, "user");
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(m, "pass");
  const cJSON *port = cJSON_GetObjectItemCaseSensitive(m, "port");
  const int pt = cJSON_IsNumber(port) ? (int)port->valuedouble : 1883;
  if (!cJSON_IsString(h) || pt < 1 || pt > 65535) {
    err(errors, "mqtt", "host (string), port 1-65535, user and pass (strings)");
    return;
  }
  if (uplink_try_server(h->valuestring, (uint16_t)pt, cJSON_IsString(u) ? u->valuestring : "",
                        cJSON_IsString(p) ? p->valuestring : "")) {
    did(applied, "mqtt (on trial: kept once the box reaches it)");
  } else {
    err(errors, "mqtt", "too long, or not stored");
  }
}

}  // namespace

bool settings_remote_key(const char *key) { return listed(REMOTE, key); }
bool settings_trip_locked(const char *key) { return listed(TRIP_LOCKED, key); }

bool settings_apply(const cJSON *doc, cJSON *report) {
  cJSON *applied = cJSON_AddArrayToObject(report, "applied");
  cJSON *errors = cJSON_AddObjectToObject(report, "errors");
  if (!cJSON_IsObject(doc)) {
    err(errors, "document", "not a JSON object");
    return false;
  }
  const cJSON *c = cJSON_GetObjectItemCaseSensitive(doc, "config");
  if (c) apply_config(c, applied, errors);
  const cJSON *w = cJSON_GetObjectItemCaseSensitive(doc, "wifi");
  if (w) apply_wifi(w, applied, errors);
  const cJSON *o = cJSON_GetObjectItemCaseSensitive(doc, "ota_base");
  if (o) {
    if (cJSON_IsString(o) && ota_set_base(o->valuestring)) did(applied, "ota_base");
    else err(errors, "ota_base", "http(s)://host/path, under 128");
  }
  // The broker last: everything else is stored before the connection is
  // handed to a server that may not answer.
  const cJSON *m = cJSON_GetObjectItemCaseSensitive(doc, "mqtt");
  if (m) apply_mqtt(m, applied, errors);
  return cJSON_GetArraySize(errors) == 0;
}

void settings_network(cJSON *out) {
  cJSON *w = cJSON_AddArrayToObject(out, "wifi");
  for (int i = 0; i < net_count(); i++) {
    char s[33];
    if (net_known(i, s, sizeof(s))) cJSON_AddItemToArray(w, cJSON_CreateString(s));
  }
  UplinkStatus us;
  uplink_status(&us);
  cJSON *m = cJSON_AddObjectToObject(out, "mqtt");
  cJSON_AddStringToObject(m, "host", us.host);
  cJSON_AddNumberToObject(m, "port", us.port);
  cJSON_AddStringToObject(m, "user", us.user);
  cJSON_AddBoolToObject(m, "on_trial", us.trial);
  char base[128];
  if (ota_get_base(base, sizeof(base))) cJSON_AddStringToObject(out, "ota_base", base);
  else cJSON_AddNullToObject(out, "ota_base");
}
