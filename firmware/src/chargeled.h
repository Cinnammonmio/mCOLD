// The side light (LED4) as the charge indicator, per docs/led-design.md.
//
//   fast charge             breathe yellow
//   taper, SOC >= 80 %      breathe green
//   charge done             steady green
//   input, not charging     yellow blink every 2 s
//   charge fault            red blink every 1 s
//   no external power       off
//
// It lights only while external power is present, so it costs the
// battery nothing -- the one exception the design allows to the rule
// that nothing stays lit. Presence is read straight from the charger's
// PG# pin on every frame, so unplugging puts the light out at once
// instead of at the next five-second power read.
#pragma once

#include "power.h"

enum class ChargeLed : uint8_t {
  Off = 0,
  Charging,      // breathe yellow
  Topping,       // breathe green
  Full,          // steady green
  NotCharging,   // slow yellow blink
  Fault,         // red blink
};

void chargeled_start(void);

// Latest reading from the power task. The light follows it.
void chargeled_update(const PowerStatus &ps);

ChargeLed chargeled_mode(void);
const char *chargeled_name(ChargeLed m);

// docs/led-design.md, open question 5: the BQ25601 reports constant
// current and constant voltage alike as "fast charge", so the
// yellow-to-green change is taken from the fuel gauge. The gauge reads
// low after a deep discharge until it has seen a full cycle.
static const float CHARGELED_TAPER_SOC = 80.0f;
