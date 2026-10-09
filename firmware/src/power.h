// The power chain: fuel gauge, current monitor, charger.
//
// Three parts have to agree about one battery, and reading them
// together is the only way to notice when one of them is wrong. During
// bring-up all three moved in step when charging was enabled -- the
// charger reported fast charging, the shunt showed +480 mA into the
// cell, and the cell voltage rose -- which is the cross-check this
// driver keeps available at runtime.
//
// Every field carries its own validity. A fuel gauge that will not
// answer is not a flat battery, and a box that reports 0 percent
// because an I2C read failed would be dispatched for recharging when
// nothing was wrong with it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum class ChargeState : uint8_t {
  Unknown = 0,
  NotCharging,
  PreCharge,
  FastCharge,
  Done,
};

enum class VbusType : uint8_t {
  Unknown = 0,
  None,
  UsbHost,      // SDP, 500 mA class
  Adapter,
  Otg,
};

struct PowerStatus {
  // Fuel gauge
  bool cell_valid;          // false too when there is no battery (cell_absent)
  bool cell_absent;         // the charger is holding the cell node up by itself
  float cell_volts;
  float soc_percent;        // 0..100: counted (soc.h) once soc_update() has run
  float gauge_percent;      // the MAX17048's own estimate, from the voltage
  float rate_percent_hr;    // signed: negative while discharging

  // Current monitor, across the 10 mOhm shunt in the battery branch
  bool current_valid;
  float battery_ma;         // positive into the cell, negative out of it
  float bus_volts;

  // Charger
  bool charger_valid;
  ChargeState charge;
  VbusType vbus;
  bool power_good;
  uint8_t fault_reg;        // REG09 raw
  bool watchdog_expired;    // the charger has reset its own registers
};

void power_init(void);
// Before deep sleep: parts that would otherwise draw current for nothing.
// `measure` leaves the current monitor averaging through the sleep
// instead (it then draws 330 uA itself), for power_sleep_mean().
void power_sleep(bool measure);
// After a measured sleep: the mean battery current, mA out of the cell,
// over the last 8.4 s before waking. False if this wake has none.
bool power_sleep_mean(float *ma);

// The battery current now, + into the cell. One register read.
bool power_battery_ma(float *ma);

// Switches the box off for good: the charger opens its battery switch
// (BATFET_DIS, "ship mode") ten seconds after this returns, and the
// cell then feeds nothing but the fuel gauge. It comes back on when USB
// power is plugged in. For a cell too flat to run on (config
// batt_off_mv): deep-discharging a LiPo is what swells it.
bool power_ship_mode(void);

// Bench: the charger's input switched off (HIZ), so the box runs from
// its battery with the USB cable -- and the console -- still in. The
// only way to measure what the box draws while still watching it.
bool power_set_hiz(bool on);

// Reads all three. Fields whose device did not answer are left invalid
// rather than zeroed, so a failure cannot be mistaken for a reading.
void power_read(PowerStatus *out);

// The charger keeps a watchdog of about forty seconds. Let it expire
// and every register silently returns to its reset value, so a charge
// current set at boot quietly becomes the factory default later on.
// This must be called more often than that, and it is a read-modify-
// write: the kick bit lives in a register that also holds the charge
// enable, and writing the byte whole turns charging off.
void power_kick_watchdog(void);

const char *charge_state_name(ChargeState s);
const char *vbus_type_name(VbusType v);

static const uint32_t POWER_WATCHDOG_MS = 20000;
