// The trip: what the box is for.
//
// A trip begins with START (from the app over BLE in P5; from the
// console until then), writes a header record carrying that trip's own
// alarm thresholds, then a sample every sample period and an event
// whenever something happens, and ends with STOP and a summary record.
// Everything goes to the trip log first (§9.5); nothing here keeps a
// value only in RAM that the log does not also have.
//
// The rules this module exists to keep (§8):
//
//   A reset in the middle of a trip resumes that trip and says so in the
//   log (EV_RESUMED). It never quietly starts a new one, and never drops
//   the old one.
//
//   An alarm is raised, cleared and acknowledged as events. Acknowledging
//   silences; it never erases. The history stays in the log.
//
//   Nothing starts a trip by itself -- not an NFC tap, not motion, not
//   plugging into the dock.
//
//   When the log is full, the oldest finished trip is deleted whole and a
//   loss event is written first (§9.6). The running trip is never touched.
//
// The operational state is only Idle / Active here. Power, connectivity
// and storage states are separate axes held by their own modules, as §8
// asks; a box can be on a trip and charging at once.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "accel.h"
#include "door.h"
#include "power.h"
#include "rtcclock.h"
#include "temp.h"

// Set by the app at START, per trip (decided 2026-10-02).
struct TripParams {
  int16_t low_c10;          // alarm below this, 0.1 C
  int16_t high_c10;         // alarm above this, 0.1 C
  uint16_t hyst_c10;        // must come back this far inside to clear
  uint16_t dwell_s;         // must stay outside this long to raise
  uint16_t door_alarm_s;    // door open longer than this raises; 0 = never
};

enum class TripErr : uint8_t {
  Ok = 0,
  AlreadyActive,
  NotActive,
  BadParams,
  NoLog,          // the trip log is not available: nothing can be recorded
  LogFull,        // full, and nothing that may be deleted to make room
  Flash,
};

struct TripStatus {
  bool active;
  uint32_t id;
  TripParams params;
  uint32_t samples;
  uint16_t alarms_active;   // bit per Alarm
  uint16_t alarms_raised;   // since the start of the trip
  bool acked;
  uint16_t door_opens;
  uint32_t motion_events;
  bool have_temp;
  bool temp_read_since_boot;   // any attempt has completed since power-up
  int16_t min_c100, max_c100;
  uint32_t lost_trips;      // deleted to make room, since the device was new
};

// After the log, config and time are up. Resumes a trip that a reset
// interrupted.
void trip_init(const char *sn);

TripErr trip_start(const TripParams &p, uint32_t *id);
TripErr trip_stop(uint8_t reason);
void trip_ack_alarms(void);
void trip_status(TripStatus *out);

// Inputs, from whichever task owns each source. All thread-safe.
void trip_note_temp(TempStatus st, float celsius);
void trip_note_motion(const AccelEvent &ev);
void trip_note_power(const PowerStatus &ps);
void trip_note_door(DoorState now, uint32_t previous_lasted_ms);
void trip_note_time_set(TimeSource src, uint32_t utc_before);

// Time-based alarms (door held open, probe gone). Call about once a
// second.
void trip_tick(void);

// Writes a sample if a trip is running. The caller decides when.
void trip_sample(void);

// Last trip id used, for the console's record dump.
uint32_t trip_last_id(void);

const char *trip_err_name(TripErr e);
const char *alarm_name(uint8_t a);

// Ids at or above this are not real trips (the console's bench trip
// lives up there) and never affect numbering.
static const uint32_t TRIP_ID_REAL_MAX = 0x7FFFFFFF;

// No valid temperature for this long raises the probe alarm.
static const uint32_t TRIP_PROBE_ALARM_MS = 60000;

// Low battery: raise below, clear at or above.
static const float TRIP_BATT_LOW_PCT = 15.0f;
static const float TRIP_BATT_OK_PCT = 20.0f;

// Motion is recorded as one event per burst, at most this often; the
// count in between goes into the samples. A box being carried fires the
// detector many times a second, and the log is not for that.
static const uint32_t TRIP_MOTION_EVENT_MS = 60000;
