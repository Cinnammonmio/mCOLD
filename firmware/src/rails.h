// Power rails, reference counted.
//
// Three switched rails feed overlapping sets of devices: the display
// shares its rail with the thermocouple, and the SD card shares its
// with the GNSS. So "I am finished with the display, turning its rail
// off" is wrong whenever a temperature sample is in flight, and the
// symptom is an occasional bad reading rather than an obvious fault.
//
// Each subsystem says what it needs and releases it; the rail is on
// while anyone needs it and off the moment nobody does. No code outside
// this file touches the enable pins.
//
// The polarities are not uniform -- two rails are enabled high and the
// LED rail is enabled low through a P-MOS -- which is exactly the kind
// of detail that gets inverted somewhere if it is written out more than
// once.
#pragma once

#include <stdint.h>

enum class Rail : uint8_t {
  Epd = 0,    // 3V3_EPD_TK: e-paper and MAX6675
  SdGnss,     // 3V3_SD_GNSS: microSD and the GNSS module
  Led,        // 3V3_LED: the four addressable pixels
  Count
};

void rails_init(void);

// Claim a rail. Safe to call when it is already on; the count rises.
// Returns once the rail has had its settle time, so a caller never has
// to know what that time is -- the LED chain needs fifty milliseconds
// and will silently not latch its first frame without it.
void rail_acquire(Rail r);

// Release a claim. The rail goes off when the last holder lets go.
void rail_release(Rail r);

bool rail_is_on(Rail r);
int rail_users(Rail r);
const char *rail_name(Rail r);

// Drop every rail regardless of claims, for the moment before sleep.
// Anything still holding a claim is a bug worth logging, so this says
// how many there were.
int rails_all_off(void);
