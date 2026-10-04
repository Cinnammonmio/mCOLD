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
  int32_t led_bright_pct;     // side light brightness cap, % of full scale
  int32_t led_front_pct;      // front three lights' brightness cap, %
  int32_t led_status_s;       // on battery, the steady status blinks this often
  int32_t batt_off_mv;        // below this the box switches itself off (ship mode)
  int32_t batt_trip_mv;       // below this no new trip starts; the box stays up
  int32_t batt_mah;           // rated cell capacity, until one is learned (soc.h)
  int32_t sleep_ua;           // current asleep, for counting charge through a sleep
  int32_t soc_source;         // the percent shown: 0 voltage (OCV table), 1 counted
  int32_t buzzer_enabled;     // 0 silences alarm sounds
  int32_t tz_offset_min;      // local time for the display only; records are UTC
  int32_t epd_inset_t;        // panel pixels the case hides: top (screen cal)
  int32_t epd_inset_b;        //   bottom
  int32_t epd_inset_l;        //   left
  int32_t epd_inset_r;        //   right
  int32_t sleep_en;           // 0 keeps the box awake on battery (bench, measurement)
  int32_t idle_wake_s;        // on battery, the longest sleep with nothing due
  int32_t gnss_period_s;      // on battery, during a trip: one GNSS session this often
  int32_t upload_period_s;    // on battery: the longest records wait for Wi-Fi
  int32_t sleep_usb;          // bench: behave as on battery with USB in (pm.h)
  int32_t sleep_meas;         // bench: measure each sleep's current (costs 0.33 mA)
  int32_t light_sleep;        // on battery, light sleep between tasks while awake (trial)
  int32_t temp_adj_c100;      // user adjustment added to the reading, 0.01 C (eTEMP's tempAdj)

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

// Every setting in table order, for anything that lists them (the app).
int config_count(void);
bool config_at(int i, const char **key, int32_t *value, int32_t *min,
               int32_t *max);

// Called after a setting is stored, from wherever it was changed --
// console or app -- so that settings which act at once (accelerometer
// threshold, LED brightness) act the same way from both.
typedef void (*ConfigHook)(const char *key, int32_t value);
void config_on_change(ConfigHook hook);

// Settings that were out of range when loaded, since boot.
int config_rejected(void);
