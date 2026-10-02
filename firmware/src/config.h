// Device configuration in NVS.
//
// One NVS key per setting, all described by a single table in
// config.cpp: name, range, default, unit. That table is the whole
// contract, and it is what makes the three awkward cases boring:
//
//   a key that was never written    -> its default
//   a key from an older firmware    -> kept, if still in range
//   a value out of range            -> its default, and reported
//
// so there is no migration code to get wrong, and a corrupted or
// hand-edited value can never reach the code that uses it. The schema
// number is stored too, so a future change that cannot be expressed
// that way -- a unit changing, say -- has something to key off.
//
// Alarm thresholds are not here on purpose: they are set by the app
// when a trip starts and belong to that trip (P3), because one box
// carries 2-8 C vaccine one week and something else the next.
//
// Credentials (Wi-Fi, MQTT) are not here yet: they arrive with P6 and
// belong in an encrypted NVS namespace, not beside the sample period.
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct Config {
  int32_t sample_period_s;    // trip sampling; 60 is the fastest §9.3 allows
  int32_t cal_offset_c100;    // added after gain, 0.01 C
  int32_t cal_gain_ppm;       // 1000000 = unity
  int32_t cal_version;        // 0 = never calibrated
  int32_t cal_date;           // unix seconds of the calibration, 0 = none
  int32_t accel_wake_ths;     // 1..63, FS/64 per step (31.25 mg at +-2 g)
  int32_t led_bright_pct;     // not yet applied: the cap is fixed until P4
  int32_t buzzer_enabled;     // not yet applied: alarms are P3

  // Field names are the NVS keys, 15 characters at most; config.cpp
  // refuses to compile a longer one.
};

// Brings up NVS and loads the configuration. NVS is erased only in the
// two cases ESP-IDF says it cannot be used otherwise (no free pages, or
// a newer format); that resets every setting to its default and says
// so on the console. The trip log lives elsewhere and is never touched.
void config_init(void);

const Config &config(void);
uint32_t config_schema(void);

// By name, for the console now and BLE later. Returns false for an
// unknown key or a value out of range -- nothing is stored in that case.
bool config_set(const char *key, int32_t value);

// Prints every setting with its range, default, and whether it is the
// default.
void config_print(void);

// Back to defaults: every key erased.
void config_reset(void);

// Settings that were out of range when loaded, since boot.
int config_rejected(void);
