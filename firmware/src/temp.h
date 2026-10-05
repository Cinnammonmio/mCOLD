// The temperature: a type K thermocouple through the MAX6675, or, on a box
// built with one instead (SN ...-002, 2026-10-05), an SHT-31 on the I2C
// bus. Which one is found at the cold boot -- an SHT-31 answering at 0x44
// wins -- so one firmware serves both. Humidity is not read.
//
// MAX6675 thermocouple front end.
//
// The product's whole reason to exist is this number, so the driver's
// job is as much about refusing to produce one as producing it. Three
// different things can come back from this part and only one of them is
// a temperature:
//
//   a conversion       -- a real reading
//   open thermocouple  -- the probe is not connected. This is a fault,
//                         and it is emphatically not 0 degrees.
//   nothing            -- all-zero or all-ones on the bus, which is
//                         what an unpowered part or a dead MISO line
//                         looks like.
//
// Bring-up found a fourth case the datasheet makes easy to miss: the
// conversion already in progress when the part powers up is not a
// measurement. On this board it read 175.5 C, and the three readings
// after it were 47.25, 46.75 and 46.00. So the first frame after the
// rail comes up is discarded, always.
#pragma once

#include <stdint.h>

enum class TempStatus : uint8_t {
  Ok = 0,
  Open,        // thermocouple not connected: a fault, never a value
  NoData,      // bus read all zeros or all ones: no part, or no power
  Garbled,     // frame structure wrong: not a MAX6675 answering
  Busy,        // could not get the bus in time
};

// Powers the rail, throws away the stale conversion, waits for a fresh
// one and returns it. Takes about 500 ms, which at a sample every sixty
// seconds is under one percent duty on that rail -- cheaper than
// leaving it powered, and the part is only accurate once it has settled
// anyway.
TempStatus temp_sample(float *celsius);

const char *temp_status_name(TempStatus s);

enum class TempSensor : uint8_t { Unknown = 0, Max6675, Sht31 };
TempSensor temp_sensor(void);
const char *temp_sensor_name(TempSensor s);

// MAX6675 conversion time is quoted at up to 220 ms. Waiting 250
// twice -- once to flush the stale frame, once for a fresh one -- is
// what the readings above cost to learn.
static const uint32_t TEMP_CONVERT_MS = 250;
