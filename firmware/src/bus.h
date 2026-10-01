// Bus access, guarded.
//
// Eight devices share one I2C bus and two more share SPI3 with the
// display. Without a single gate in front of each bus, two tasks
// eventually interleave a register pointer write with someone else's
// read, and the symptom is a sensor that returns another sensor's data
// once an hour. That bug is extremely hard to find from the outside, so
// it is designed out rather than debugged later.
//
// Every call here takes a timeout and returns a status. Nothing waits
// forever: a part that stops acknowledging must cost one timeout, not
// the task that was talking to it. Each call also reports to the health
// registry under the device tag it was given, so the failure accounting
// is a property of using the bus rather than something each driver has
// to remember.
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "health.h"

enum class BusErr : int32_t {
  Ok = 0,
  Timeout = -1,       // could not get the bus: another task is holding it
  Nak = -2,           // the device did not acknowledge
  Short = -3,         // fewer bytes came back than asked for
  NotReady = -4,      // bus not initialised
};

void bus_init(void);

// Default time to wait for the bus itself. Generous enough that a
// normal queue of callers gets through, short enough that a stuck
// transaction surfaces as an error rather than a hang.
static const uint32_t BUS_WAIT_MS = 100;

// ---- I2C, one shared bus -------------------------------------------
// `dev` is only used for health accounting; it does not select anything.

BusErr i2c_write(Dev dev, uint8_t addr, const uint8_t *data, size_t n);
BusErr i2c_read_reg(Dev dev, uint8_t addr, uint8_t reg, uint8_t *buf, size_t n);
BusErr i2c_write_reg(Dev dev, uint8_t addr, uint8_t reg, uint8_t v);
BusErr i2c_probe(Dev dev, uint8_t addr);

// Devices with 16-bit internal addressing -- the NFC tag's EEPROM is
// addressed this way, and treating it like an 8-bit register map reads
// nothing at all while looking exactly like a dead part.
BusErr i2c_read_mem16(Dev dev, uint8_t addr, uint16_t mem, uint8_t *buf, size_t n);
BusErr i2c_write_mem16(Dev dev, uint8_t addr, uint16_t mem, const uint8_t *data,
                       size_t n);

// ---- SPI3: e-paper and the thermocouple front end ------------------
// The display driver holds this for the length of a refresh, which on
// the four-ink panel is twenty-five seconds. Anything else wanting the
// bus in that window gets Timeout and is expected to try later rather
// than to wait it out.

bool spi3_take(uint32_t wait_ms);
void spi3_give(void);
BusErr spi3_transfer16(Dev dev, int cs_pin, uint32_t hz, uint16_t *out);

const char *bus_err_name(BusErr e);
