#include "soc.h"

#include <esp_attr.h>
#include <math.h>
#include <nvs.h>
#include <stddef.h>
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
  float net;            // every mAh counted in (+) and out (-), never corrected
  float ref_soc;        // the last reliable reference point, %; < 0: none
  float ref_net;        // `net` at that point
  uint32_t charged_at;  // mono_ms() charging was last seen; 0: not this boot
  uint8_t learned;      // cap learned from two reference points
  float ocv_pct;        // the voltage's percent, smoothed; < 0: none yet
  uint32_t check;
};
const uint32_t MAGIC = 0x534F4333;   // "SOC3"
RTC_NOINIT_ATTR Kept g_k;

const char *NS = "soc";
// The current while ROM and bootloader run, before anything can measure
// it: a rough figure, for ~0.13 s a wake.
const float BOOT_MA = 60.0f;
// Kept count and gauge further apart than this at first reading: the
// cell was charged (or swapped) while the count could not see it.
const float DISAGREE_PCT = 15.0f;
// What is left when batt_off_mv switches the box off (3.40 V on the
// table below is about 5 %).
const float LEFT_AT_OFF = 5.0f;
// Longest gap between two readings that is counted at the earlier
// reading's current; longer and the gap is not this awake stretch.
const uint32_t MAX_GAP_MS = 60000;

// ---- the voltage side ----------------------------------------------------
//
// Open-circuit voltage against state of charge for a generic LiPo cell at
// room temperature. Generic: this cell's own curve (its datasheet, or the
// capacity test: every sample logs cell mV next to the counted charge)
// replaces it when there is one. Good to a few percent at the ends, where
// the curve is steep; poor in the flat middle, 3.75-3.87 V, where tens of
// percent sit in a few millivolts -- so it is trusted by its slope.
struct Ocv {
  uint16_t mv;
  uint8_t pct;
};
const Ocv OCV[] = {
    {3270, 0},  {3610, 5},  {3690, 10}, {3710, 15}, {3730, 20}, {3750, 25}, {3770, 30},
    {3790, 35}, {3800, 40}, {3820, 45}, {3840, 50}, {3850, 55}, {3870, 60}, {3910, 65},
    {3950, 70}, {3980, 75}, {4020, 80}, {4080, 85}, {4110, 90}, {4150, 95}, {4200, 100},
};
const int N_OCV = sizeof(OCV) / sizeof(OCV[0]);

// The cell's internal resistance, for the voltage it loses under the
// ~140 mA of a wake: a typical figure for a 1500 mAh LiPo.
const float R_CELL_OHM = 0.15f;
// A sleep at least this long leaves the cell rested enough to read.
const uint32_t REST_MS = 240000;
// After charging the voltage relaxes for hours; not read as charge.
const uint32_t AFTER_CHARGE_MS = 2 * 3600000;
// Slope, in mV per %, from which a reading is a reference point.
const float STEEP_MV_PER_PCT = 6.0f;
// Two reference points this far apart give a capacity.
const float CAP_SPAN_PCT = 30.0f;

float ocv_percent(float mv, float *slope) {
  if (mv <= OCV[0].mv) {
    if (slope) *slope = 68;
    return 0;
  }
  for (int i = 1; i < N_OCV; i++) {
    if (mv <= OCV[i].mv) {
      const float dv = OCV[i].mv - OCV[i - 1].mv, dp = OCV[i].pct - OCV[i - 1].pct;
      if (slope) *slope = dv / dp;
      return OCV[i - 1].pct + dp * (mv - OCV[i - 1].mv) / dv;
    }
  }
  if (slope) *slope = 10;
  return 100;
}

bool g_valid = false;         // the count is established this boot
bool g_have_nvs = false;
Kept g_nvs = {};
uint32_t g_last_t = 0;
float g_last_ma = 0;
bool g_have_last = false;
bool g_first_reading = true;  // of this boot or wake
float g_saved_mah = -1000;
const char *g_anchor = "none";
float g_last_ocv_pct = -1;
float g_last_cap_est = 0;

uint32_t sum(const Kept &k) {
  const uint8_t *p = (const uint8_t *)&k;
  uint32_t s = 0x9E3779B9;
  for (size_t i = 0; i < offsetof(Kept, check); i++) s = (s ^ p[i]) * 16777619u;
  return s;
}

bool sane(const Kept &k) {
  return k.cap >= 300 && k.cap <= 6000 && k.mah >= -1 && k.mah <= k.cap + 1 &&
         k.drawn < 20000 && !isnan(k.mah) && !isnan(k.drawn) && !isnan(k.net) &&
         k.ref_soc <= 100;
}

void keep(void) {
  g_k.magic = MAGIC;
  g_k.check = sum(g_k);
}

float percent(void) {
  const float p = g_k.mah / g_k.cap * 100.0f;
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

void clamp(void) {
  if (g_k.mah < 0) g_k.mah = 0;
  if (g_k.mah > g_k.cap) g_k.mah = g_k.cap;
}

void count(float ma, uint32_t ms) {
  const float d = ma * (float)ms / 3600000.0f;   // mAh, + into the cell
  g_k.mah += d;
  g_k.net += d;
  if (d < 0 && g_k.drawn >= 0) g_k.drawn -= d;
  clamp();
}

// A point where the percent is known well (charge done, a rested voltage
// on a steep part of the curve, the switch-off voltage). Two of them far
// enough apart, with the charge counted between, give the capacity:
// counted mAh / difference in percent. No full-to-empty run needed.
void reference(float soc, const char *why) {
  if (g_k.ref_soc >= 0 && fabsf(soc - g_k.ref_soc) >= CAP_SPAN_PCT) {
    const float est = fabsf(g_k.net - g_k.ref_net) / (fabsf(soc - g_k.ref_soc) / 100.0f);
    const float rated = (float)config().batt_mah;
    if (est > 0.5f * rated && est < 1.5f * rated) {
      // Blended, so one bad point moves it only part of the way.
      const float cap = g_k.learned ? 0.7f * g_k.cap + 0.3f * est : 0.5f * g_k.cap + 0.5f * est;
      printf("[soc] capacity %.0f mAh from %.0f %% -> %.0f %% (%s): now %.0f mAh\n", est,
             g_k.ref_soc, soc, why, cap);
      g_k.mah *= cap / g_k.cap;
      g_k.cap = cap;
      g_k.learned = 1;
      g_last_cap_est = est;
    }
  }
  g_k.ref_soc = soc;
  g_k.ref_net = g_k.net;
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
    // at the first reading. The mono clock restarted with the power.
    g_have_nvs = load(&g_nvs);
    g_nvs.charged_at = 0;
    g_valid = false;
  }
  g_last_t = mono_ms();
  g_have_last = false;
  g_first_reading = true;
}

void soc_update(PowerStatus &ps) {
  ps.gauge_percent = ps.soc_percent;
  if (!ps.cell_valid || !ps.current_valid) return;
  const uint32_t t = mono_ms();
  const bool first = g_first_reading;
  g_first_reading = false;

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
      g_k.ref_soc = -1;
      g_k.ocv_pct = -1;
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

  const bool charging = ps.charger_valid && ps.power_good &&
                        (ps.charge == ChargeState::PreCharge || ps.charge == ChargeState::FastCharge);
  if (charging) g_k.charged_at = t ? t : 1;

  if (ps.charger_valid && ps.power_good && ps.charge == ChargeState::Done) {
    if (g_k.mah != g_k.cap || g_k.drawn != 0) {
      g_anchor = "charge done";
      reference(100.0f, "charge done");
    }
    g_k.mah = g_k.cap;
    g_k.drawn = 0;
    g_k.charged_at = t ? t : 1;
  } else if (first && pm_slept_ms() >= REST_MS && !ps.power_good &&
             (!g_k.charged_at || t - g_k.charged_at > AFTER_CHARGE_MS)) {
    // The first reading after a long sleep: the cell has rested at a
    // fraction of a milliamp. Add back what this wake's current takes off
    // the terminal voltage, read the table, and pull the count towards it
    // -- hard where the curve is steep, gently where it is flat.
    const float ocv = ps.cell_volts * 1000.0f - ps.battery_ma * R_CELL_OHM;
    float slope = 0;
    const float v = ocv_percent(ocv, &slope);
    float k = (slope - 3.0f) / 10.0f;
    if (k < 0.02f) k = 0.02f;
    if (k > 0.2f) k = 0.2f;
    g_k.mah += k * (v - percent()) / 100.0f * g_k.cap;
    clamp();
    g_last_ocv_pct = v;
    if (slope >= STEEP_MV_PER_PCT) reference(v, "rested voltage");
  }

  // The voltage's own percent at every reading on battery, the load added
  // back and smoothed over a few readings (a wake's first is the best, the
  // rest follow the radio's current). While charging the voltage says
  // nothing about the charge, and the last value stands.
  if (!ps.power_good) {
    const float v = ocv_percent(ps.cell_volts * 1000.0f - ps.battery_ma * R_CELL_OHM, nullptr);
    g_k.ocv_pct = g_k.ocv_pct < 0 ? v : 0.7f * g_k.ocv_pct + 0.3f * v;
  }

  // Which one the box shows (decided 2026-10-04: the voltage, until the
  // count has been checked against a measured capacity and a measured
  // sleep current -- the current monitor reads 0.25 mA a step and cannot
  // see the sleep). On a charger the count, anchored at charge done.
  if (config().soc_source == 0 && !ps.power_good && g_k.ocv_pct >= 0) {
    ps.soc_percent = g_k.ocv_pct;
  } else {
    ps.soc_percent = percent();
  }
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
  if (switching_off) {
    // batt_off_mv is a known point on the curve: a reference like any other.
    reference(LEFT_AT_OFF, "switch-off voltage");
    g_k.mah = g_k.cap * LEFT_AT_OFF / 100.0f;
  }
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
  out->ocv_percent = g_last_ocv_pct;
  out->ref_percent = g_k.ref_soc;
  out->last_capacity_estimate = g_last_cap_est;
  out->voltage_percent = g_k.ocv_pct;
}

void soc_set(float percent) {
  if (!g_valid) {
    memset(&g_k, 0, sizeof(g_k));
    g_k.cap = (float)config().batt_mah;
    g_k.drawn = -1;
    g_k.ref_soc = -1;
    g_k.ocv_pct = -1;
    g_valid = true;
  }
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  g_k.mah = g_k.cap * percent / 100.0f;
  g_anchor = "set by hand";
  keep();
  save();
}
