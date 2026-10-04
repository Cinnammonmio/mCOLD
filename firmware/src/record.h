// What a trip writes to the log, byte for byte (record format 3).
//
// §9.4: a documented binary layout, little-endian, never a C struct
// copied out of memory (padding and field order are the compiler's,
// not ours). Every field that can be unknown has a value that says so,
// and that value is never one a real reading could take -- an invalid
// temperature is INT16_MIN, not 0, because 0 C is a real temperature.
//
// One kind of record, the ROW (decided 2026-10-05): a sample and every
// event are the same row -- what happened, and the state of the box at
// that moment (temperature, alarms, position, motion, battery, link) --
// so the log, the server and a CSV file are one table with the same
// columns. logrow.h turns a row into those columns.
//
// ROW (type 0x10), every record:
//
//   0  u32  utc_s        seconds since 1970 UTC; meaningless if time_q 0
//   4  u8   time_q       TimeSource: 0 none, 1 rtc, 2 gnss, 3 host, 4 ntp
//   5  u16  boot         boot counter (low 16 bits)
//   7  u32  tick_ms      ms since that boot -- with boot, orders rows whose
//                        UTC cannot be trusted, and re-times them later
//  11  u8   event        RowEvent
//  12  i16  temp         calibrated, 0.01 C; INT16_MIN none
//  14  u8   alarms       active, bit per Alarm
//  15  u8   gnss         GnssState: 0 none, 1 last (an older fix), 2 fix
//  16  i32  latitude     1e-7 degree (0 with gnss none)
//  20  i32  longitude    1e-7 degree
//  24  u16  motion       motion events since the previous row
//  26  u8   battery      state of charge, %; 0xFF unknown
//  27  u8   internet     Link: result of the last upload attempt
//  28  i32  detail       per event (RowEvent)
//  32       -- end of the row
//
// TRIP_START rows go on (the trip's header):
//
//  32  u8   header format (3)
//  33  u32  trip id
//  37  i16  tempmin, alarm below, 0.1 C
//  39  i16  tempmax, alarm above, 0.1 C
//  41  u16  hysteresis, 0.1 C
//  43  u16  dwell, s
//  45  u16  sample period, s
//  47  u16  config schema
//  49  i32  calibration offset, 0.01 C
//  53  i32  calibration gain, ppm
//  57  u32  calibration version
//  61  c16  firmware version, NUL padded
//  77  c24  device SN, NUL padded
// 101
//
// TRIP_STOP rows go on (the summary):
//
//  32  u8   reason (1 console, 2 app)
//  33  u32  samples written
//  37  i16  lowest sampled temperature, 0.01 C; INT16_MIN none
//  39  i16  highest, 0.01 C
//  41  u16  alarms raised
//  43  u32  motion events
//  47
//
// Format 1 and 2 logs (types 0x01-0x04: separate header, sample and
// event records) are not read any more. A trip in that format is left
// alone: never uploaded, reclaimed when the space is needed.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum RecordType : uint8_t {
  REC_ROW = 0x10,
};

static const uint8_t ROW_HEADER_FORMAT = 3;

// What the row records. The names are the `event` column (logrow.cpp).
enum RowEvent : uint8_t {
  RE_TRIP_START = 1,
  RE_TRIP_STOP = 2,
  RE_SAMPLE = 3,       // every sample period
  RE_ALARM_HIGH = 4,   // above tempmax for longer than dwell
  RE_ALARM_LOW = 5,    // below tempmin for longer than dwell
  RE_ALARM_PROBE = 6,  // no valid temperature for longer than allowed
  RE_ALARM_CLEAR = 7,  // detail: the Alarm that cleared
  RE_ALARM_ACK = 8,    // detail: the alarm bits acknowledged
  RE_SHOCK = 9,        // above the shock threshold (not detected yet: no threshold set)
  RE_PROBE_FAULT = 10, // detail: TempStatus
  RE_PROBE_OK = 11,
  RE_USB_IN = 12,
  RE_USB_OUT = 13,
  RE_POWER_ON = 14,    // the trip carried on after a reset; detail: reset reason
  RE_POWER_OFF = 15,   // battery too low, the box switched itself off; detail: cell mV
  RE_BATTERY_LOW = 16, // detail: state of charge, %
  RE_TIME_SET = 17,    // detail: UTC seconds before (as u32; 0 if unknown)
  RE_DATA_LOST = 18,   // a trip deleted to make room; detail: its id
  RE_COUNT
};

enum Alarm : uint8_t {
  AL_TEMP_HIGH = 0,
  AL_TEMP_LOW = 1,
  AL_PROBE = 2,         // no valid temperature for longer than allowed
  AL_DOOR = 3,          // kept for the bit position; this product has no door
  AL_BATTERY = 4,       // low battery
  AL_COUNT
};

enum GnssState : uint8_t { GNSS_NONE = 0, GNSS_LAST = 1, GNSS_FIX = 2 };

// The last upload attempt, not the radio this instant: on battery Wi-Fi
// is off between sessions, and "offline" on every row would say nothing.
enum Link : uint8_t { LINK_OFFLINE = 0, LINK_WIFI_ONLY = 1, LINK_ONLINE = 2 };

static const uint32_t STAMP_LEN = 11;
static const uint32_t ROW_LEN = 32;
static const uint32_t ROW_START_LEN = 101;
static const uint32_t ROW_STOP_LEN = 47;
static const uint16_t U16_NONE = 0xFFFF;
static const int16_t I16_NONE = INT16_MIN;

// Little-endian writer over a fixed buffer. Writing past the end is
// dropped and remembered, so a payload that does not fit fails as a
// whole rather than being stored truncated.
struct Writer {
  uint8_t *p;
  size_t cap;
  size_t n = 0;
  bool overflow = false;

  Writer(uint8_t *buf, size_t c) : p(buf), cap(c) {}
  void u8(uint8_t v) { put(&v, 1); }
  void u16(uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(b, 2); }
  void i16(int16_t v) { u16((uint16_t)v); }
  void u32(uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 24)};
    put(b, 4);
  }
  void i32(int32_t v) { u32((uint32_t)v); }
  // Fixed width, NUL padded, never NUL terminated if it fills the field.
  void str(const char *s, size_t width) {
    for (size_t i = 0; i < width; i++) {
      const uint8_t c = (s && *s) ? (uint8_t)*s++ : 0;
      put(&c, 1);
    }
  }
  void put(const void *b, size_t k) {
    if (n + k > cap) { overflow = true; return; }
    memcpy(p + n, b, k);
    n += k;
  }
};

// The matching reader.
struct Reader {
  const uint8_t *p;
  size_t len;
  size_t n = 0;

  Reader(const uint8_t *buf, size_t l) : p(buf), len(l) {}
  bool ok(size_t k) const { return n + k <= len; }
  uint8_t u8() { return ok(1) ? p[n++] : 0; }
  uint16_t u16() {
    if (!ok(2)) return 0;
    const uint16_t v = (uint16_t)(p[n] | p[n + 1] << 8);
    n += 2;
    return v;
  }
  int16_t i16() { return (int16_t)u16(); }
  uint32_t u32() {
    if (!ok(4)) return 0;
    const uint32_t v = (uint32_t)p[n] | (uint32_t)p[n + 1] << 8 |
                       (uint32_t)p[n + 2] << 16 | (uint32_t)p[n + 3] << 24;
    n += 4;
    return v;
  }
  int32_t i32() { return (int32_t)u32(); }
  void str(char *out, size_t width) {
    for (size_t i = 0; i < width; i++) out[i] = ok(1) ? (char)p[n++] : 0;
  }
};
