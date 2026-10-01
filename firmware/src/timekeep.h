// Time as the rest of the firmware sees it: UTC with a statement of how
// far it can be trusted, and an ordering that survives the clock being
// wrong.
//
// Two clocks, for two different questions:
//
//   UTC          "when did this happen". Only as good as its source --
//                the RTC loses it on every power loss (no backup cell),
//                so it carries a quality, and a record stamped with
//                quality None has no time, not the time 1970.
//
//   boot + tick  "in what order did things happen". The boot counter
//                lives in NVS and only ever goes up; the tick is
//                milliseconds since this boot. Together they order every
//                event this device ever logs, even across a reset, even
//                when UTC is later corrected backwards (§4.5).
//
// The ESP32's own clock (gettimeofday) is set from the RTC at boot and
// from any later sync, so ordinary C time functions agree with the RTC.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "rtcclock.h"

struct TimeStamp {
  int64_t utc_ms;        // ms since 1970-01-01 UTC; meaningless if quality None
  TimeSource quality;
  uint32_t boot;         // boot counter, from NVS
  uint32_t tick_ms;      // ms since this boot
};

// After rtc_begin() and nvs init. Counts this boot and, if the RTC's
// time survived, puts it on the system clock.
void time_init(void);

void time_now(TimeStamp *out);
bool time_valid(void);
uint32_t time_boot_count(void);

// Sets RTC and system clock together from a trusted source. A source
// of lower standing does not overwrite a higher one within the same
// boot (the app's phone clock does not undo a satellite fix) unless
// `force` is set.
bool time_set(const struct tm *utc, TimeSource src, bool force);

// "2026-10-02 04:18:10" in UTC, or "----------" if unknown.
void time_format(const TimeStamp &t, char *out, size_t n);
