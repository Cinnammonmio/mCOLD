// What a trip writes to the log, byte for byte.
//
// §9.4: a documented binary layout, little-endian, never a C struct
// copied out of memory (padding and field order are the compiler's,
// not ours). Every field that can be unknown has a value that says so,
// and that value is never one a real reading could take -- an invalid
// temperature is INT16_MIN, not 0, because 0 C is a real temperature.
//
// Every payload starts with the same 11-byte stamp:
//
//   0  u32  utc_s        seconds since 1970 UTC; meaningless if quality 0
//   4  u8   time_q       TimeSource: 0 none, 1 rtc, 2 gnss, 3 host
//   5  u16  boot         boot counter (low 16 bits)
//   7  u32  tick_ms      ms since that boot
//
// so every record can be ordered (boot, tick) even when its UTC cannot
// be trusted, and re-timed later when it can.
//
// Record types and their payloads after the stamp:
//
// TRIP_START (0x01)                         TRIP_STOP (0x04)
//  11 u8   header format (1)                 11 u8   reason (1 console, 2 app)
//  12 u32  trip id                           12 u32  samples written
//  16 i16  temp alarm low, 0.1 C             16 i16  min valid temp, 0.01 C
//  18 i16  temp alarm high, 0.1 C            18 i16  max valid temp, 0.01 C
//  20 u16  hysteresis, 0.1 C                 20 u16  alarms raised
//  22 u16  dwell, s                          22 u16  door openings
//  24 u16  door open alarm, s                24 u32  door open total, s
//  26 u16  sample period, s                  28 u32  motion events
//  28 u16  config schema
//  30 i32  calibration offset, 0.01 C
//  34 i32  calibration gain, ppm
//  38 u32  calibration version
//  42 c16  firmware version, NUL padded
//  58 c12  device SN, NUL padded
//
// SAMPLE (0x02), one per sample period
//  11 u8   temp status (TempStatus; 0 ok)
//  12 u16  temp raw, MAX6675 counts of 0.25 C; 0xFFFF none
//  14 i16  temp calibrated, 0.01 C; INT16_MIN none
//  16 u8   door: 0 unknown, 1 closed, 2 open
//  17 u16  door openings so far this trip
//  19 u16  motion events since the previous sample
//  21 u8   gnss flags: bit0 fix in the latest session, bit1 any fix ever
//  22 i32  latitude, 1e-7 degree
//  26 i32  longitude, 1e-7 degree
//  30 u16  fix age, s; 0xFFFF none or older
//  32 u8   satellites used
//  33 u8   hdop x10; 0xFF unknown
//  34 u16  cell voltage, mV; 0 unknown
//  36 u8   state of charge, %; 0xFF unknown
//  37 i16  battery current, mA, + into the cell; INT16_MIN unknown
//  39 u8   charge state (ChargeState)
//  40 u16  alarms active (bit per Alarm)
//  42 u16  devices not healthy (bit per Dev: degraded or failed)
//
// EVENT (0x03)
//  11 u8   event code (EventCode)
//  12 ...  event data, per code (see EventCode)
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum RecordType : uint8_t {
  REC_TRIP_START = 0x01,
  REC_SAMPLE = 0x02,
  REC_EVENT = 0x03,
  REC_TRIP_STOP = 0x04,
};

enum EventCode : uint8_t {
  EV_RESUMED = 1,       // u8 reset reason: the trip carried on after a reset
  EV_DOOR_OPEN = 2,     //
  EV_DOOR_CLOSE = 3,    // u32 how long it was open, ms
  EV_MOTION = 4,        // u8 WAKE_UP_SRC of the first event, u16 events
                        //   in the burst so far (more follow in samples)
  EV_PROBE_FAULT = 5,   // u8 TempStatus
  EV_PROBE_OK = 6,      //
  EV_ALARM_RAISE = 7,   // u8 Alarm, i16 value that raised it (unit per alarm)
  EV_ALARM_CLEAR = 8,   // u8 Alarm
  EV_ALARM_ACK = 9,     // u16 alarms active when acknowledged
  EV_TIME_SET = 10,     // u8 source, u32 utc_s before (0 if unknown)
  EV_LOSS = 11,         // u32 trip deleted, u32 its records, u8 reason
};

enum Alarm : uint8_t {
  AL_TEMP_HIGH = 0,
  AL_TEMP_LOW = 1,
  AL_PROBE = 2,         // no valid temperature for longer than allowed
  AL_DOOR = 3,          // door open longer than allowed
  AL_BATTERY = 4,       // low battery
  AL_COUNT
};

static const uint32_t STAMP_LEN = 11;
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

// The matching reader, for the console and, later, the uploader.
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
