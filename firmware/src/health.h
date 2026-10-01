// Device health registry.
//
// The rule this exists to enforce: one device failing must not stop the
// others, and must never be reported as a reading. A thermocouple that
// has come unplugged is not 0 degrees, a fuel gauge that will not answer
// is not 0 percent, and a GNSS with no fix is not at the equator. Each
// of those is the absence of a value, and the only way to keep that
// distinction all the way out to the display, the log and the app is to
// carry it from the point of failure rather than inventing a number and
// hoping someone downstream remembers.
//
// Every device gets a slot here. Drivers report each attempt; nothing
// else decides a device is dead. A device that fails repeatedly is
// backed off rather than hammered, and is retried slowly so that a
// connector reseated in the field recovers on its own.
#pragma once

#include <stdint.h>
#include <stddef.h>

enum class DevState : uint8_t {
  Unknown = 0,   // never tried: not the same as failed
  Ok,            // last attempt succeeded
  Degraded,      // failing, but has worked before and is still being retried
  Failed,        // failing since boot, or long past the retry limit
  Absent,        // optional and genuinely not fitted, e.g. no SD card
};

enum class Dev : uint8_t {
  Accel = 0,     // LIS2DW12
  Fuel,          // MAX17048
  Current,       // INA226
  PdSink,        // HUSB238A
  Nfc,           // ST25DV04KC
  Rtc,           // PCF8523
  Charger,       // BQ25601
  Thermo,        // MAX6675
  Gnss,          // ATGM336H
  Display,       // e-paper
  Leds,
  Buzzer,
  Sd,
  Count
};

struct DevHealth {
  DevState state;
  uint32_t ok_count;
  uint32_t fail_count;
  uint32_t consecutive_fails;
  int32_t last_error;         // driver-specific; 0 when none
  uint32_t last_ok_ms;        // 0 means never
  uint32_t last_attempt_ms;
};

void health_init(void);

// Called by drivers on every attempt. These are the only things that
// move a device between states.
void health_ok(Dev d);
void health_fail(Dev d, int32_t err);

// Mark something optional as simply not fitted, which is a different
// thing from broken and must not raise an alarm.
void health_absent(Dev d);

DevState health_state(Dev d);
const DevHealth *health_get(Dev d);
const char *health_name(Dev d);
const char *health_state_name(DevState s);

// True when a device is worth talking to right now. A failed device is
// retried on a slow schedule instead of on every pass, so that a dead
// part costs a bus transaction a minute rather than one per loop.
bool health_should_try(Dev d, uint32_t now_ms);

// How many devices are in each state, for the status line and for
// deciding whether the unit is fit to start a trip.
void health_summary(int *ok, int *degraded, int *failed, int *unknown);

// Threshold at which a device stops being trusted. Low enough that a
// genuinely dead part is noticed quickly, high enough that one NAK on a
// busy bus does not take a working sensor out of service.
static const uint32_t HEALTH_FAIL_LIMIT = 3;

// Retry interval once a device is Failed.
static const uint32_t HEALTH_RETRY_MS = 60000;
