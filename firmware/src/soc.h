// State of charge by counting charge, not by reading the voltage.
//
// The MAX17048 estimates the percent from the cell voltage through a
// generic Li-ion model -- this cell's model is not loaded (no datasheet
// yet) -- and the box makes that hard: every wake draws ~130 mA for a
// second, and right after a full charge the cell's surface charge
// relaxes, so the percent falls fast near the top and wanders with load.
//
// Counted instead (coulomb counting): the INA226 measures the current in
// and out of the cell while the chip is awake; while it sleeps the INA226
// is off, and the sleep is charged at config `sleep_ua` for as long as it
// lasted. Anchors keep the count honest:
//
//   charge done (BQ25601)       -> 100 %
//   first reading, or the kept count disagreeing with the gauge by more
//   than 15 % (charged with the power switch off, a new cell)  -> the
//                                  gauge's percent, once
//   switched off at batt_off_mv after a full charge -> the capacity is
//                                  learned from what was drawn since
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

// Each power reading: count, anchor, and write the counted percent into
// `ps.soc_percent` (the gauge's own stays in `ps.gauge_percent`). Does
// nothing to an invalid or absent cell.
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
};
void soc_status(SocStatus *out);

// Bench: set the count by hand (0..100), or mark the cell full now.
void soc_set(float percent);
