// The four addressable pixels.
//
// Chain order and colour, checked by eye on 2026-10-02: index 0 is LED4
// (XL-4020RGBC-2812B) on the side of the case; index 1 is the front
// RIGHT, 2 the front middle, 3 the front LEFT. Both part types take
// GRB and showed the colour asked for.
//
// docs/led-design.md assumed LED1 was front left. It is front right.
// The design fixes meaning by position -- left cargo, middle alive,
// right device -- because position is what a colour-blind reader
// relies on, so the names below follow position, not designator.
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

// True while a pulse holds pixel `index`. Anything that animates a
// pixel on its own (the charge light) stands aside until it ends.
bool leds_pulse_active(int index);

// Everything dark and the rail released.
void leds_off(void);

static const int LED_SIDE = 0;     // LED4, side: charge status
static const int LED_DEVICE = 1;   // LED1, front right
static const int LED_ALIVE = 2;    // LED2, front middle
static const int LED_CARGO = 3;    // LED3, front left

// Cap on every channel, as a percentage of full scale. Colours are
// scaled to it when staged, so it applies from the next leds_set() on.
// Two caps: the side light (charge status, config led_bright_pct,
// 20 % per docs/led-design.md) and the three front lights (config
// led_front_pct, 8 % -- turned down 2026-10-03 to save power; the front
// lights blink at every wake, the side light only shows on a charger).
void leds_set_brightness(int pct);
void leds_set_front_brightness(int pct);

// The defaults until config is applied: 20 % and 8 % of full scale.
static const uint8_t LEDS_BRIGHTNESS_CAP = 51;
static const uint8_t LEDS_FRONT_BRIGHTNESS_CAP = 20;
static const uint32_t LEDS_PULSE_MAX_MS = 2000;
