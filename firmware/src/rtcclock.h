// PCF8523 real-time clock, and the question of whether its time is
// worth believing.
//
// Two facts about this board make that question the important part:
//
//   As drawn, the VBAT net has no backup source, and Control_3 comes up
//   0xE0 -- switch-over disabled. When the main rail drops (SW3 off),
//   the clock stops and the date resets; bring-up found it reading
//   2020-07-02 with the oscillator-stopped flag set. This prototype now
//   has a CR1220 hand-soldered on VBAT_3V, and rtc_begin() turns the
//   switch-over on, so the time survives power-off. A board without
//   the cell behaves as before, and the rule below still holds.
//
//   CAP_SEL returns to 7 pF on every power loss, while the schematic
//   calls for 12.5 pF. Set it wrong and the clock still runs, just at
//   the wrong rate -- minutes a week, with nothing anywhere reporting
//   a fault. So it is written on every boot, not once in production.
//
// Hence the rule this header exists to enforce: a record is never
// stamped with a time the device cannot vouch for. Until something
// trustworthy has set the clock, the time is unknown, and unknown is
// reported as unknown rather than as 1 January 2000.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

enum class TimeSource : uint8_t {
  None = 0,    // never set, or lost: timestamps are not available
  Rtc,         // the clock kept running across this reset
  Gnss,        // from satellite time, the most trustworthy source here
  Host,        // set over BLE, USB or the app
  Ntp,         // from a time server, once Wi-Fi is up
};

// Writes CAP_SEL, reads the oscillator-stopped flag and decides whether
// the time survived. Call once at boot, before anything wants a
// timestamp.
bool rtc_begin(void);

// True only when the time can be defended. Everything that writes a
// record must check this; nothing may substitute a default date.
bool rtc_time_valid(void);
TimeSource rtc_source(void);

// Reads the clock. Returns false when the part did not answer -- which
// is separate from the time being invalid, and both are worth knowing.
bool rtc_get(struct tm *out);

// Sets the clock and records where the time came from. Verifies by
// reading the oscillator flag back: on a crystal that is not
// oscillating the flag reappears within a second, so this is also the
// test that the 32.768 kHz circuit works at all.
bool rtc_set(const struct tm *t, TimeSource src);

// The oscillator-stopped flag as last read, for diagnostics.
bool rtc_os_flag(void);

// Backup cell on VBAT (a CR1220 on this prototype; the board as drawn
// has none). Low: the part's own battery-low detector. Ran on backup:
// main power went away since the last boot and the cell carried the
// clock. Control_3 as found at boot, before it was set.
bool rtc_backup_low(void);
bool rtc_ran_on_backup(void);
uint8_t rtc_control3_at_boot(void);

const char *rtc_source_name(TimeSource s);
