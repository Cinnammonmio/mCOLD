#include "soc.h"

#include <esp_attr.h>
#include <math.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "pm.h"
#include "timekeep.h"

namespace {

// Kept through deep sleep and resets (RTC_NOINIT), and checked, because
// after a power-on that memory holds noise.
struct Kept {
  uint32_t magic;
  float mah;            // charge remaining
  float cap;            // capacity in use
  float drawn;          // drawn since the last full charge; < 0: none seen
  uint8_t learned;      // cap learned from a full-to-empty run
  uint32_t check;
};
const uint32_t MAGIC = 0x534F4331;   // "SOC1"
RTC_NOINIT_ATTR Kept g_k;

const char *NS = "soc";
// The current while ROM and bootloader run, before anything can measure
// it: a rough figure, for ~0.13 s a wake.
const float BOOT_MA = 60.0f;
// Kept count and gauge further apart than this at first reading: the
// cell was charged (or swapped) while the count could not see it.
const float DISAGREE_PCT = 15.0f;
// What is left when batt_off_mv switches the box off, for learning the
// capacity from a full-to-empty run.
const float LEFT_AT_OFF = 0.05f;
// Longest gap between two readings that is counted at the earlier
// reading's current; longer and the gap is not this awake stretch.
const uint32_t MAX_GAP_MS = 60000;

bool g_valid = false;         // the count is established this boot
bool g_have_nvs = false;
Kept g_nvs = {};
uint32_t g_last_t = 0;
float g_last_ma = 0;
bool g_have_last = false;
float g_saved_mah = -1000;
const char *g_anchor = "none";

uint32_t sum(const Kept &k) {
  const uint8_t *p = (const uint8_t *)&k;
  uint32_t s = 0x9E3779B9;
  for (size_t i = 0; i < offsetof(Kept, check); i++) s = (s ^ p[i]) * 16777619u;
  return s;
}

bool sane(const Kept &k) {
  return k.cap >= 300 && k.cap <= 6000 && k.mah >= -1 && k.mah <= k.cap + 1 &&
         k.drawn < 20000 && !isnan(k.mah) && !isnan(k.drawn);
}

void keep(void) {
  g_k.magic = MAGIC;
  g_k.check = sum(g_k);
}

float percent(void) {
  const float p = g_k.mah / g_k.cap * 100.0f;
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

void count(float ma, uint32_t ms) {
  const float d = ma * (float)ms / 3600000.0f;   // mAh, + into the cell
  g_k.mah += d;
  if (d < 0 && g_k.drawn >= 0) g_k.drawn -= d;
  if (g_k.mah < 0) g_k.mah = 0;
  if (g_k.mah > g_k.cap) g_k.mah = g_k.cap;
}

void save(void) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  if (nvs_set_blob(h, "k", &g_k, sizeof(g_k)) == ESP_OK && nvs_commit(h) == ESP_OK) {
    g_saved_mah = g_k.mah;
  }
  nvs_close(h);
}

bool load(Kept *out) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t n = sizeof(*out);
  const bool ok = nvs_get_blob(h, "k", out, &n) == ESP_OK && n == sizeof(*out) &&
                  out->magic == MAGIC && sane(*out);
  nvs_close(h);
  return ok;
}

}  // namespace

void soc_init(void) {
  if (g_k.magic == MAGIC && g_k.check == sum(g_k) && sane(g_k)) {
    // Carried over a sleep or a reset: the count goes on.
    g_valid = true;
    g_anchor = "carried over";
    if (pm_warm()) {
      count(-(float)config().sleep_ua / 1000.0f, pm_slept_ms());
      count(-BOOT_MA, pm_boot_ms());
    }
    keep();
  } else {
    // After a power-off: what NVS had, to be checked against the gauge
    // at the first reading.
    g_have_nvs = load(&g_nvs);
    g_valid = false;
  }
  g_last_t = mono_ms();
  g_have_last = false;
}

void soc_update(PowerStatus &ps) {
  ps.gauge_percent = ps.soc_percent;
  if (!ps.cell_valid || !ps.current_valid) return;
  const uint32_t t = mono_ms();

  if (!g_valid) {
    const float gauge = ps.soc_percent;
    const float cap = g_have_nvs && g_nvs.learned ? g_nvs.cap : (float)config().batt_mah;
    if (g_have_nvs && fabsf(g_nvs.mah / g_nvs.cap * 100.0f - gauge) <= DISAGREE_PCT) {
      g_k = g_nvs;
      g_anchor = "kept (NVS)";
    } else {
      memset(&g_k, 0, sizeof(g_k));
      g_k.cap = cap;
      g_k.learned = g_have_nvs ? g_nvs.learned : 0;
      g_k.mah = cap * gauge / 100.0f;
      g_k.drawn = -1;
      g_anchor = "fuel gauge";
    }
    g_valid = true;
    g_saved_mah = g_have_nvs ? g_nvs.mah : -1000;
  } else {
    // Trapezoid between this reading and the last; from boot to the first
    // reading, at this reading's current.
    uint32_t dt = t - g_last_t;
    if (dt > MAX_GAP_MS) dt = MAX_GAP_MS;
    const float ma = g_have_last ? (g_last_ma + ps.battery_ma) / 2 : ps.battery_ma;
    count(ma, dt);
  }
  g_last_t = t;
  g_last_ma = ps.battery_ma;
  g_have_last = true;

  // A capacity set from config, not learned, follows config.
  if (!g_k.learned && g_k.cap != (float)config().batt_mah) {
    g_k.mah = g_k.mah / g_k.cap * (float)config().batt_mah;
    g_k.cap = (float)config().batt_mah;
  }
  if (ps.charger_valid && ps.power_good && ps.charge == ChargeState::Done) {
    if (g_k.mah != g_k.cap || g_k.drawn != 0) g_anchor = "charge done";
    g_k.mah = g_k.cap;
    g_k.drawn = 0;
  }
  ps.soc_percent = percent();
  keep();
  if (fabsf(g_k.mah - g_saved_mah) >= g_k.cap / 100.0f) save();
}

void soc_before_sleep(bool switching_off) {
  if (!g_valid) return;
  if (g_have_last) {
    uint32_t dt = mono_ms() - g_last_t;
    if (dt > MAX_GAP_MS) dt = MAX_GAP_MS;
    count(g_last_ma, dt);
    g_have_last = false;
  }
  if (switching_off && g_k.drawn > 0.5f * g_k.cap) {
    // From full to the switch-off voltage: that is the capacity, less
    // what is left at the switch-off. Blended with what was known, and
    // kept within half to one and a half of the rated figure.
    const float run = g_k.drawn / (1.0f - LEFT_AT_OFF);
    const float rated = (float)config().batt_mah;
    float cap = g_k.learned ? 0.5f * g_k.cap + 0.5f * run : run;
    if (cap < 0.5f * rated) cap = 0.5f * rated;
    if (cap > 1.5f * rated) cap = 1.5f * rated;
    printf("[soc] capacity learned: %.0f mAh (drawn %.0f mAh since full)\n", cap, g_k.drawn);
    g_k.cap = cap;
    g_k.learned = 1;
    g_k.drawn = -1;
  }
  if (switching_off) g_k.mah = g_k.cap * LEFT_AT_OFF;
  keep();
  if (switching_off || fabsf(g_k.mah - g_saved_mah) >= g_k.cap / 100.0f) save();
}

void soc_status(SocStatus *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->valid = g_valid;
  out->percent = g_valid ? percent() : 0;
  out->remaining_mah = g_k.mah;
  out->capacity_mah = g_k.cap;
  out->capacity_learned = g_k.learned;
  out->drawn_since_full_mah = g_k.drawn;
  out->anchor = g_anchor;
}

void soc_set(float percent) {
  if (!g_valid) {
    memset(&g_k, 0, sizeof(g_k));
    g_k.cap = (float)config().batt_mah;
    g_k.drawn = -1;
    g_valid = true;
  }
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  g_k.mah = g_k.cap * percent / 100.0f;
  g_anchor = "set by hand";
  keep();
  save();
}
