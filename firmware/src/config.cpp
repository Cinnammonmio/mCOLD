#include "config.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace {

const char *NS = "cfg";
// 1: first release. 2: alarm thresholds left for the trip -- they are
// set when a trip starts, per trip, not once per device.
// 3: the door feature switched off; its polarity setting goes with it.
const uint32_t SCHEMA = 3;

// Keys that earlier schemas stored and this one no longer reads.
// Erased on the first boot that finds them, so a value nobody uses
// cannot be mistaken for one that is in force.
const char *const RETIRED[] = {
    "temp_low_c10", "temp_high_c10", "temp_hyst_c10", "temp_dwell_s",
    "door_closed_lvl",
};

struct Field {
  const char *key;           // NVS key: 15 characters at most
  size_t offset;
  int32_t min, max, def;
  const char *unit;
};

// The field name is the NVS key, and NVS takes 15 characters. A longer
// name fails here, at compile time, instead of silently never storing.
// (Calling a non-constexpr function is what fails the constant
// evaluation; exceptions are off in this build, so no throw.)
void nvs_key_longer_than_15_characters(void);
constexpr const char *key15(const char *k, size_t n) {
  return n <= 16 ? k : (nvs_key_longer_than_15_characters(), k);
}
#define F(name, lo, hi, d, u) \
  {key15(#name, sizeof(#name)), offsetof(Config, name), lo, hi, d, u}

// The contract. Ranges are what the hardware and the requirements
// allow, not what seems sensible today; the product team narrows them.
constexpr Field FIELDS[] = {
    F(sample_period_s, 60, 3600, 300, "s"),
    F(cal_offset_c100, -1000, 1000, 0, "0.01 C"),
    F(cal_gain_ppm, 900000, 1100000, 1000000, "ppm"),
    F(cal_version, 0, 0x7FFFFFFF, 0, ""),
    F(cal_date, 0, 0x7FFFFFFF, 0, "unix s"),
    F(accel_wake_ths, 1, 63, 2, "x31 mg"),
    F(led_bright_pct, 1, 100, 20, "%"),
    F(led_front_pct, 1, 100, 2, "%"),
    F(led_status_s, 60, 3600, 900, "s"),
    F(batt_off_mv, 3000, 3700, 3400, "mV"),
    F(batt_trip_mv, 3000, 4100, 3550, "mV"),
    F(batt_mah, 300, 6000, 1500, "mAh"),
    F(sleep_ua, 0, 5000, 250, "uA"),
    F(soc_source, 0, 1, 0, ""),
    F(tz_offset_min, -720, 840, 420, "min"),
    F(epd_inset_t, 0, 20, 0, "px"),
    F(epd_inset_b, 0, 20, 0, "px"),
    F(epd_inset_l, 0, 30, 0, "px"),
    F(epd_inset_r, 0, 30, 0, "px"),
    F(buzzer_enabled, 0, 1, 1, ""),
    F(sleep_en, 0, 1, 1, ""),
    F(idle_wake_s, 300, 86400, 3600, "s"),
    F(gnss_period_s, 300, 86400, 1800, "s"),
    F(upload_period_s, 60, 86400, 300, "s"),
    F(sleep_usb, 0, 1, 0, ""),
    F(sleep_meas, 0, 1, 0, ""),
    F(light_sleep, 0, 1, 0, ""),
    F(temp_adj_c100, -1000, 1000, 0, "0.01 C"),
    F(usb_drive, 0, 1, 1, ""),
    F(tap_test, 0, 1, 0, ""),
    F(rx_show, 0, 1, 0, ""),
};
#undef F

const int N = sizeof(FIELDS) / sizeof(FIELDS[0]);

Config g_cfg;
uint32_t g_schema = 0;
int g_rejected = 0;
ConfigHook g_hook = nullptr;
bool g_stored[sizeof(FIELDS) / sizeof(FIELDS[0])];

int32_t *slot(const Field &f) {
  return (int32_t *)((uint8_t *)&g_cfg + f.offset);
}

const Field *find(const char *key) {
  for (int i = 0; i < N; i++) {
    if (!strcmp(FIELDS[i].key, key)) return &FIELDS[i];
  }
  return nullptr;
}

void load(void) {
  for (int i = 0; i < N; i++) {
    *slot(FIELDS[i]) = FIELDS[i].def;
    g_stored[i] = false;
  }
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;   // nothing stored yet

  uint32_t s = 0;
  if (nvs_get_u32(h, "schema", &s) == ESP_OK) g_schema = s;

  for (int i = 0; i < N; i++) {
    const Field &f = FIELDS[i];
    int32_t v;
    if (nvs_get_i32(h, f.key, &v) != ESP_OK) continue;
    if (v < f.min || v > f.max) {
      printf("[config] %s = %ld is outside %ld..%ld; using default %ld\n",
             f.key, (long)v, (long)f.min, (long)f.max, (long)f.def);
      g_rejected++;
      continue;
    }
    *slot(f) = v;
    g_stored[i] = true;
  }
  nvs_close(h);
}

void migrate(void) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  for (const char *k : RETIRED) nvs_erase_key(h, k);   // absent is fine
  nvs_set_u32(h, "schema", SCHEMA);
  nvs_commit(h);
  nvs_close(h);
  printf("[config] settings migrated from schema %lu to %lu\n",
         (unsigned long)g_schema, (unsigned long)SCHEMA);
  g_schema = SCHEMA;
}

}  // namespace

void config_init(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    printf("[config] NVS unusable (%s): erasing it. Every setting is back"
           " to its default; the trip log is not affected.\n",
           esp_err_to_name(e));
    nvs_flash_erase();
    e = nvs_flash_init();
  }
  if (e != ESP_OK) {
    printf("[config] NVS failed (%s): running on defaults\n", esp_err_to_name(e));
  }
  load();
  if (g_schema && g_schema < SCHEMA) migrate();
}

const Config &config(void) { return g_cfg; }
uint32_t config_schema(void) { return SCHEMA; }
int config_rejected(void) { return g_rejected; }

bool config_set(const char *key, int32_t value) {
  const Field *f = find(key);
  if (!f || value < f->min || value > f->max) return false;

  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_i32(h, f->key, value) == ESP_OK &&
                  nvs_set_u32(h, "schema", SCHEMA) == ESP_OK &&
                  nvs_commit(h) == ESP_OK;
  nvs_close(h);
  // Memory follows NVS, never leads it: a value that did not reach
  // flash must not be in use as though it had.
  if (ok) {
    *slot(*f) = value;
    g_stored[f - FIELDS] = true;
    if (g_hook) g_hook(f->key, value);
  }
  return ok;
}

void config_print(void) {
  printf("\n  %-19s %10s  %-22s %s\n", "setting", "value", "range", "");
  for (int i = 0; i < N; i++) {
    const Field &f = FIELDS[i];
    char range[32];
    snprintf(range, sizeof(range), "%ld..%ld %s", (long)f.min, (long)f.max,
             f.unit);
    printf("  %-19s %10ld  %-22s %s\n", f.key, (long)*slot(f), range,
           g_stored[i] ? "" : "(default)");
  }
  printf("\n  schema %lu (stored %lu)", (unsigned long)SCHEMA,
         (unsigned long)g_schema);
  if (g_rejected) printf(", %d value(s) rejected at load", g_rejected);
  printf("\n\n");
}

void config_reset(void) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
  }
  g_rejected = 0;
  load();
}

int config_count(void) { return N; }

bool config_at(int i, const char **key, int32_t *value, int32_t *min,
               int32_t *max) {
  if (i < 0 || i >= N) return false;
  const Field &f = FIELDS[i];
  if (key) *key = f.key;
  if (value) *value = *slot(f);
  if (min) *min = f.min;
  if (max) *max = f.max;
  return true;
}

void config_on_change(ConfigHook hook) { g_hook = hook; }
