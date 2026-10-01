#include "config.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace {

const char *NS = "cfg";
const uint32_t SCHEMA = 1;

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
    F(temp_low_c10, -400, 1000, 20, "0.1 C"),
    F(temp_high_c10, -400, 1000, 80, "0.1 C"),
    F(temp_hyst_c10, 0, 100, 5, "0.1 C"),
    F(temp_dwell_s, 0, 3600, 300, "s"),
    F(cal_offset_c100, -1000, 1000, 0, "0.01 C"),
    F(cal_gain_ppm, 900000, 1100000, 1000000, "ppm"),
    F(cal_version, 0, 0x7FFFFFFF, 0, ""),
    F(cal_date, 0, 0x7FFFFFFF, 0, "unix s"),
    F(door_closed_lvl, 0, 1, 0, "level"),
    F(accel_wake_ths, 1, 63, 2, "x31 mg"),
    F(led_bright_pct, 1, 100, 20, "%"),
    F(buzzer_enabled, 0, 1, 1, ""),
};
#undef F

const int N = sizeof(FIELDS) / sizeof(FIELDS[0]);

Config g_cfg;
uint32_t g_schema = 0;
int g_rejected = 0;
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

  // Low below high, or neither alarm means anything. Checked as a pair
  // because each value alone can be in range and the two still nonsense.
  if (g_cfg.temp_low_c10 >= g_cfg.temp_high_c10) {
    printf("[config] temp_low_c10 %ld is not below temp_high_c10 %ld;"
           " using defaults for both\n",
           (long)g_cfg.temp_low_c10, (long)g_cfg.temp_high_c10);
    g_cfg.temp_low_c10 = find("temp_low_c10")->def;
    g_cfg.temp_high_c10 = find("temp_high_c10")->def;
    g_rejected++;
  }
}

const Config &config(void) { return g_cfg; }
uint32_t config_schema(void) { return SCHEMA; }
int config_rejected(void) { return g_rejected; }

bool config_set(const char *key, int32_t value) {
  const Field *f = find(key);
  if (!f || value < f->min || value > f->max) return false;

  // The pair rule again, against the value about to change.
  Config next = g_cfg;
  *(int32_t *)((uint8_t *)&next + f->offset) = value;
  if (next.temp_low_c10 >= next.temp_high_c10) return false;

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
