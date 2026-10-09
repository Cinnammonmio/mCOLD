#include "settings.h"

#include <math.h>
#include <nvs.h>
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
    "buzzer_enabled",  "accel_wake_ths",  "temp_adj_c100",  "usb_drive",
};
// usb_drive is here as the way back: if the drive ever made the port
// unusable, the server can turn it off and the console returns on the
// USB-Serial-JTAG, where esptool flashes.
// The factory calibration (cal_*) is the console's: the user's correction
// is temp_adj_c100, as eTEMP has it (decided 2026-10-05).

// A trip's header carries these; changing them under a running trip would
// make its rows say something the header does not.
const char *const TRIP_LOCKED[] = {
    "sample_period_s", "temp_adj_c100", "cal_offset_c100", "cal_gain_ppm", "cal_version",
    "cal_date",
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

// One network: its password (kept if absent for a known one) and how it
// gives the box an address -- "dhcp": true, or ip/gateway/subnet[/dns].
void str16(const cJSON *o, const char *k, char *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  snprintf(out, 16, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

}  // namespace

bool settings_wifi_entry(const cJSON *e, const char **why) {
  const cJSON *s = cJSON_GetObjectItemCaseSensitive(e, "ssid");
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "pass");
  const cJSON *sl = cJSON_GetObjectItemCaseSensitive(e, "slot");
  char name[33] = "";
  if (cJSON_IsNumber(sl)) {
    // By slot (1..5): rename, change the password or the address of what
    // is there, or fill an empty one. The password may be left out only
    // when the ssid stays.
    const int slot = (int)sl->valuedouble - 1;
    if (slot < 0 || slot >= NET_MAX) {
      *why = "slot 1..5";
      return false;
    }
    char cur[33];
    const bool have = net_slot_ssid(slot, cur, sizeof(cur));
    if (cJSON_IsString(s)) snprintf(name, sizeof(name), "%s", s->valuestring);
    else if (have) snprintf(name, sizeof(name), "%s", cur);
    else {
      *why = "an empty slot needs ssid and pass";
      return false;
    }
    const bool same = have && !strcmp(cur, name);
    if (!same && !cJSON_IsString(p)) {
      *why = "a new ssid needs pass (\"\" for an open one)";
      return false;
    }
    for (int j = 0; j < NET_MAX; j++) {
      char other[33];
      if (j != slot && net_slot_ssid(j, other, sizeof(other)) && !strcmp(other, name)) {
        *why = "that network is already in another slot";
        return false;
      }
    }
    if (!net_slot_put(slot, name, cJSON_IsString(p) ? p->valuestring : nullptr)) {
      *why = "ssid 1-32 bytes, pass empty or 8-63";
      return false;
    }
  } else {
    if (!cJSON_IsString(s)) {
      *why = "each entry needs an ssid (or a slot)";
      return false;
    }
    snprintf(name, sizeof(name), "%s", s->valuestring);
    bool known = false;
    for (int i = 0; i < net_count(); i++) {
      char n[33];
      known = known || (net_known(i, n, sizeof(n)) && !strcmp(n, name));
    }
    if (cJSON_IsString(p) || !known) {
      if (!known && !cJSON_IsString(p)) {
        *why = "a new network needs pass (\"\" for an open one)";
        return false;
      }
      if (!net_add(name, p->valuestring)) {
        *why = "ssid 1-32 bytes, pass empty or 8-63, at most 5 networks";
        return false;
      }
    }
  }
  const cJSON *dhcp = cJSON_GetObjectItemCaseSensitive(e, "dhcp");
  const cJSON *ip = cJSON_GetObjectItemCaseSensitive(e, "ip");
  if (dhcp || ip) {
    NetIp n = {};
    n.dhcp = cJSON_IsTrue(dhcp) || (!dhcp && !ip);
    if (!n.dhcp) {
      str16(e, "ip", n.ip);
      str16(e, "gateway", n.gateway);
      str16(e, "subnet", n.subnet);
      str16(e, "dns", n.dns);
    }
    if (!net_set_ip(name, n)) {
      *why = "dhcp true, or ip, gateway, subnet (and dns) as dotted quads on one subnet";
      return false;
    }
  }
  return true;
}

namespace {

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
      char what[48];
      if (cJSON_IsNumber(it)) {          // a slot, 1..5
        snprintf(what, sizeof(what), "wifi del slot %d", (int)it->valuedouble);
        if (net_slot_clear((int)it->valuedouble - 1)) did(applied, what);
        else err(errors, what, "that slot is empty");
        continue;
      }
      if (!cJSON_IsString(it)) continue;
      snprintf(what, sizeof(what), "wifi del %s", it->valuestring);
      if (net_remove(it->valuestring)) did(applied, what);
      else err(errors, what, "not a known network");
    }
  }
  const cJSON *add = cJSON_GetObjectItemCaseSensitive(w, "add");
  if (add) {
    cJSON_ArrayForEach(it, add) {
      const cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "ssid");
      char what[48];
      const cJSON *sl = cJSON_GetObjectItemCaseSensitive(it, "slot");
      if (cJSON_IsString(s)) snprintf(what, sizeof(what), "wifi add %s", s->valuestring);
      else if (cJSON_IsNumber(sl)) snprintf(what, sizeof(what), "wifi slot %d", (int)sl->valuedouble);
      else snprintf(what, sizeof(what), "wifi add ?");
      const char *why = nullptr;
      if (settings_wifi_entry(it, &why)) did(applied, what);
      else err(errors, what, why);
    }
  }
}

volatile uint32_t g_gen = 1;

}  // namespace

void settings_touch(void) { g_gen = g_gen + 1; }
uint32_t settings_gen(void) { return g_gen; }

void settings_snapshot(cJSON *out, bool authorized) {
  cJSON *c = cJSON_AddObjectToObject(out, "config");
  for (int i = 0; i < config_count(); i++) {
    const char *k;
    int32_t v;
    if (config_at(i, &k, &v, nullptr, nullptr)) cJSON_AddNumberToObject(c, k, v);
  }
  cJSON *e = cJSON_AddArrayToObject(out, "editable");
  for (const char *k : REMOTE) cJSON_AddItemToArray(e, cJSON_CreateString(k));
  cJSON *l = cJSON_AddArrayToObject(out, "trip_locked");
  for (const char *k : TRIP_LOCKED) cJSON_AddItemToArray(l, cJSON_CreateString(k));
  uint32_t rev = 0;
  nvs_handle_t h;
  if (nvs_open("cfgdoc", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, "rev", &rev);
    nvs_close(h);
  }
  cJSON_AddNumberToObject(out, "server_rev", rev);
  if (authorized) settings_network(cJSON_AddObjectToObject(out, "network"));
}

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
  // The broker is not set from outside (decided 2026-10-05): the console's.
  if (cJSON_GetObjectItemCaseSensitive(doc, "mqtt")) err(errors, "mqtt", "console only");
  if (cJSON_GetArraySize(applied)) settings_touch();
  return cJSON_GetArraySize(errors) == 0;
}

void settings_network(cJSON *out) {
  // All five slots, empty ones too: {"slot":3,"ssid":null}. The app shows
  // them as they are and edits or clears any by its number.
  cJSON *w = cJSON_AddArrayToObject(out, "wifi");
  for (int i = 0; i < NET_MAX; i++) {
    char s[33];
    NetIp ip;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "slot", i + 1);
    if (!net_slot_ssid(i, s, sizeof(s)) || !net_get_ip(i, &ip)) {
      cJSON_AddNullToObject(e, "ssid");
      cJSON_AddItemToArray(w, e);
      continue;
    }
    cJSON_AddStringToObject(e, "ssid", s);
    cJSON_AddBoolToObject(e, "dhcp", ip.dhcp);
    if (!ip.dhcp) {
      cJSON_AddStringToObject(e, "ip", ip.ip);
      cJSON_AddStringToObject(e, "gateway", ip.gateway);
      cJSON_AddStringToObject(e, "subnet", ip.subnet);
      cJSON_AddStringToObject(e, "dns", ip.dns);
    }
    cJSON_AddItemToArray(w, e);
  }
  char base[128];
  if (ota_get_base(base, sizeof(base))) cJSON_AddStringToObject(out, "ota_base", base);
  else cJSON_AddNullToObject(out, "ota_base");
}
