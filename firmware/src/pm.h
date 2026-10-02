// Power manager: when the box may deep-sleep, and for how long (§12).
//
// On battery the box sleeps between jobs. A wake from deep sleep is a
// reset as far as the CPU is concerned -- RAM is gone, every task starts
// again from app_main -- so a wake is run as a short boot: each module
// does one pass of its job, says it is done, and the last one to finish
// lets the chip sleep again until the earliest moment any of them asked
// to be woken.
//
// Three things decide it, and they are kept apart on purpose:
//
//   duties  each module's pass for this wake (read the probe, write the
//           sample if one is due, redraw if needed, ...). Sleep waits
//           until every duty has reported once.
//   holds   something going on that sleep must not cut short: a BLE
//           session, a phone on the tag, a GNSS session, a refresh in
//           progress, someone at the console.
//   next    when each module next needs the chip awake. The earliest
//           one sets the timer.
//
// USB power holds the box awake outright: plugged in, there is no
// battery to save, and the USB link (console, Dock) needs the chip up.
//
// What it does NOT decide is policy for any one module -- how often to
// fix GNSS or upload is the module's own business; this only collects
// the answers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum class Wake : uint8_t {
  Cold = 0,   // power-on or a reset: not a wake from our sleep
  Timer,
  Motion,     // accelerometer INT1 (EXT0)
  Nfc,        // a phone at the tag (ST25DV GPO, EXT1)
  Usb,        // power plugged in (BQ25601 PG#, EXT1)
  Other,
};

enum class Duty : uint8_t { Sensors = 0, Power, Trip, Display, Nfc, Uplink, Gnss, Indicate, Count };

enum class Hold : uint8_t {
  Ble = 0,      // advertising for a tap, or connected
  NfcField,     // a phone is on the tag
  Gnss,         // a session is running
  Uplink,       // Wi-Fi/MQTT session on battery
  Display,      // a refresh is in progress
  Indicate,     // a light or sound pattern is playing
  Console,      // typed at recently
  Count
};

// First thing in app_main, before any pin is configured: reads why the
// chip woke and lets go of the pins held through the sleep.
void pm_init(void);

Wake pm_wake(void);
const char *pm_wake_name(Wake w);
// Woke from this firmware's own deep sleep, with RTC memory intact. A
// module that keeps state across sleep checks this, not the reset reason.
bool pm_warm(void);
uint32_t pm_wakes(void);    // deep-sleep wakes since the last real boot

// External power, from PG#. While it is present the box never sleeps --
// unless config sleep_usb is set, a bench setting that makes the box
// behave as on battery with the cable in, so a whole sleep cycle can be
// watched on the console (which drops while the chip sleeps).
bool pm_external_power(void);

void pm_done(Duty d);
// For a duty that must come after another (the display after the sample).
bool pm_is_done(Duty d);
void pm_hold(Hold h, bool on);
// Keep the chip awake until mono_ms() reaches `until`, at least.
void pm_hold_until(uint32_t until);
// When `d` next needs the chip awake, in mono_ms(); 0 for "not for me".
void pm_next(Duty d, uint32_t at);

// Battery current seen while awake, for the wake-cost record.
void pm_note_current(float ma);

// Called just before sleep, from the pm task, after every duty is done:
// a module that must leave a part in a low-power state registers here.
typedef void (*PmHook)(void);
void pm_on_sleep(PmHook fn);

// Starts the task that decides. After every module has started.
void pm_start(void);

// Console: state, and the record of recent wakes.
void pm_print(void);
void pm_trace_clear(void);
// Bench: sleep now for `seconds`, timer wake only, even on USB power --
// so sleep current can be measured with the cable still in.
void pm_sleep_test(uint32_t seconds);
