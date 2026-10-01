// The four addressable pixels.
//
// Chain order, measured: index 0 is LED4 (XL-4020RGBC-2812B, the side
// light), 1..3 are LED1..3 (SK6812MINI-E, the front). Two different
// parts on one data line, both driven as GRB -- which is what bring-up
// ran them as, and how they lit.
//
// The rule that matters more than any colour: the rail is on only while
// a pixel is lit. The pixels draw current dark (around 1 mA each, still
// to be measured), and four of them left powered would take more than
// half the battery over a seven-day trip on their own. So this driver
// takes the rail when something lights and gives it back the moment
// everything is dark, and nothing else decides when it is on.
//
// The data line is GPIO43, the ROM's UART TX pad. The ROM prints there
// on every boot and wake before the application runs; with the rail off
// that noise has nothing to light. The line idles low, so it never
// back-powers a pixel whose supply is off.
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool leds_init(void);

// Stage a colour; nothing changes until leds_show(). Values are scaled
// down by the brightness cap, so callers think in full-scale colours.
void leds_set(int index, uint8_t r, uint8_t g, uint8_t b);
void leds_clear(void);

// Sends the frame. Takes the rail first if anything is lit (blocking
// for its settle time: a pixel without supply when its frame arrives
// never latches it) and releases it if nothing is.
bool leds_show(void);

// Light one pixel for `ms`, then turn it off without the caller
// waiting. Bounded: a pulse cannot outlast LEDS_PULSE_MAX_MS, because
// the requirements forbid a light left on through a trip.
void leds_pulse(int index, uint8_t r, uint8_t g, uint8_t b, uint32_t ms);

// Everything dark and the rail released.
void leds_off(void);

static const int LED_SIDE = 0;     // LED4: charge status
static const int LED_CARGO = 1;    // LED1
static const int LED_ALIVE = 2;    // LED2
static const int LED_DEVICE = 3;   // LED3

// 20% of full scale: docs/led-design.md, configurable later.
static const uint8_t LEDS_BRIGHTNESS_CAP = 51;
static const uint32_t LEDS_PULSE_MAX_MS = 2000;
