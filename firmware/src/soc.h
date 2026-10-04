// State of charge by counting charge, not by reading the voltage.
//
// The MAX17048 estimates the percent from the cell voltage through a
// generic Li-ion model -- this cell's model is not loaded (no datasheet
// yet) -- and the box makes that hard: every wake draws ~130 mA for a
// second, and right after a full charge the cell's surface charge
// relaxes, so the percent falls fast near the top and wanders with load.
//
// Counted instead (coulomb counting), and checked against the voltage:
//
//   counting   the INA226 measures the current in and out of the cell
//              while the chip is awake; while it sleeps the INA226 is off,
//              and the sleep is charged at config `sleep_ua`. Smooth and
//              right in the short run; drifts in the long run, and needs
//              the capacity.
//   voltage    after a sleep the cell has rested, so its voltage (plus
//              what this wake's current takes off it) reads off an OCV
//              table. Never drifts, needs no capacity, but is poor where
//              the curve is flat. It pulls the count towards it, hard on
//              the steep ends and gently in the flat middle.
//   capacity   is not trusted from the label (decided 2026-10-04): two
//              reliable points -- charge done (100 %), a rested voltage on
//              a steep part of the curve, the switch-off voltage (~5 %) --
//              at least 30 % apart, with the charge counted between them,
//              give it. batt_mah is only the starting guess.
//
// The gauge's own percent is used once, to start (or when the kept count
// disagrees with it by more than 15 %: charged with the switch off).
//
// Kept through sleep and resets in RTC memory, and in NVS every 1 % so a
// power switch-off does not lose it. The low-battery switch-off itself
// stays on the cell voltage (main.cpp): a counting error must never be
// what lets a cell run flat.
#pragma once

#include <stdbool.h>

#include "power.h"

// After power_init(), before the first power_read(). Charges the sleep
// that just ended, if this is a wake.
void soc_init(void);

// Each power reading: count, anchor, and write the percent the box shows
// into `ps.soc_percent` -- the voltage's (OCV table) or the counted one,
// by config soc_source; the gauge's own stays in `ps.gauge_percent`.
// Does nothing to an invalid or absent cell.
void soc_update(PowerStatus &ps);

// Just before deep sleep or the battery switch-off: the last stretch of
// awake current, and the count to NVS if it moved.
void soc_before_sleep(bool switching_off);

struct SocStatus {
  bool valid;
  float percent;
  float remaining_mah;
  float capacity_mah;       // config batt_mah, or learned
  bool capacity_learned;
  float drawn_since_full_mah;  // -1: no full charge seen yet
  const char *anchor;       // how the count was last set
  float ocv_percent;        // the last rested-voltage reading; -1: none yet
  float ref_percent;        // the reference point a capacity is measured from; -1: none
  float last_capacity_estimate;  // 0: none this boot
  float voltage_percent;    // the OCV table's percent, smoothed; -1: none yet
};
void soc_status(SocStatus *out);

// Bench: set the count by hand (0..100), or mark the cell full now.
void soc_set(float percent);
