// What the lights and the buzzer say, per docs/led-design.md.
//
// Each front light owns one meaning, permanently, so nobody has to learn
// which light means what in which state:
//
//   front left   CARGO    the goods: temperature out of range   (red)
//   front middle ALIVE    a trip is running and logging         (green)
//   front right  DEVICE   the box itself needs attention         (amber)
//   side         CHARGE   charge state; chargeled.cpp owns it
//
// Patterns are short and end by themselves -- nothing stays lit through
// a trip (§4.8). How often the status repeats depends on what it costs:
//
//   on external power          every second (costs the battery nothing)
//   attention window           every second for 30 s after a phone tap
//                              or motion, at most 4 windows an hour
//   otherwise                  once per sample period
//
// Lights and buzzer never sound together: the buzzer's coil pulls about
// 200 mA at its peak, and the lights wait their turn.
//
// This module only reads state (trip, health, time, power) and turns it
// into light. It decides nothing about the trip, and nothing about the
// trip depends on it.
#pragma once

#include <stdint.h>

enum class Cue : uint8_t {
  Boot,          // sweep white, then the self-test result
  NfcTap,        // the whole row blinks white: "this is the box you tapped"
};

void indicate_start(void);

// One-off cues from outside. Trip start/stop, alarms and acks are seen
// by watching the trip's status, so the trip module does not call here.
void indicate_cue(Cue c);

// Motion or a tap: open an attention window, if the hourly budget allows.
void indicate_attention(void);

// Attention window length and how many may open in an hour. On battery
// the window is just long enough for one status frame: holding the chip
// awake 30 s to blink once a second cost ~5 mA on average the night of
// 2026-10-02 -- more than all the sampling.
static const uint32_t INDICATE_WINDOW_MS = 30000;
static const uint32_t INDICATE_BATTERY_WINDOW_MS = 2000;
static const int INDICATE_WINDOWS_PER_HOUR = 4;
