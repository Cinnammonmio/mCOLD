// Buzzer on GPIO6, through Q5.
//
// The schematic says 2.7 kHz; whether the part is active or passive is
// still unconfirmed. Bring-up drove it with a 2.7 kHz square wave and it
// sounded, and a square wave also sounds an active buzzer, so that is
// what this does until the part number is known.
//
// Nothing here blocks, and nothing can sound for long: a beep that
// outlives the code that started it -- a task that stalls mid-alarm --
// would be a box shrieking in a warehouse with nothing in charge of it.
// Each beep ends itself on a timer.
//
// The coil pulls a peak of around 200 mA. docs/led-design.md asks that
// LEDs and buzzer take turns rather than overlap; that is for the
// pattern layer (P4) to arrange, not this driver.
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool buzzer_init(void);

// Sound for `ms`, capped at BUZZER_MAX_MS. Replaces any beep already
// sounding. Silent while muted.
void buzzer_beep(uint32_t ms);
void buzzer_stop(void);

// Mute silences beeps; it does not clear anything that asked for them.
// Acknowledging an alarm must never erase its history.
void buzzer_mute(bool on);
bool buzzer_is_muted(void);

static const uint32_t BUZZER_FREQ_HZ = 2700;
static const uint32_t BUZZER_MAX_MS = 3000;
