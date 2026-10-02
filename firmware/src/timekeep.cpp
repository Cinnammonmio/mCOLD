#include "timekeep.h"

#include <esp_timer.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

namespace {

uint32_t g_boot = 0;

// Satellite time outranks the app's, which outranks the RTC carrying
// on from before the reset: the RTC's crystal is the thing being
// corrected, not the thing doing the correcting.
int rank(TimeSource s) {
  switch (s) {
    case TimeSource::Gnss: return 3;
    case TimeSource::Ntp:  return 3;   // as good as satellite time, for this purpose
    case TimeSource::Host: return 2;
    case TimeSource::Rtc:  return 1;
    default:               return 0;
  }
}

TimeSource g_synced_by = TimeSource::None;   // best source this boot

// timegm() without depending on newlib exposing it: the system runs on
// UTC (TZ set in time_init), so mktime is timegm here.
time_t to_epoch(const struct tm *t) {
  struct tm c = *t;
  c.tm_isdst = 0;
  return mktime(&c);
}

void set_system(const struct tm *t) {
  timeval tv = {};
  tv.tv_sec = to_epoch(t);
  settimeofday(&tv, nullptr);
}

}  // namespace

void time_init(void) {
  // Everything internal is UTC; local time is for the display and the
  // app to work out, never for a record.
  setenv("TZ", "UTC0", 1);
  tzset();

  nvs_handle_t h;
  if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
    uint32_t b = 0;
    nvs_get_u32(h, "boots", &b);
    g_boot = b + 1;
    nvs_set_u32(h, "boots", g_boot);
    nvs_commit(h);
    nvs_close(h);
  }

  struct tm t;
  if (rtc_time_valid() && rtc_get(&t) && rtc_time_valid()) {
    set_system(&t);
    g_synced_by = TimeSource::Rtc;
  }
}

void time_now(TimeStamp *out) {
  if (!out) return;
  timeval tv;
  gettimeofday(&tv, nullptr);
  out->utc_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  // The RTC's own verdict, not ours: if its oscillator stopped since
  // boot, rtc_source() has already dropped to None.
  out->quality = rtc_time_valid() ? rtc_source() : TimeSource::None;
  out->boot = g_boot;
  out->tick_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

bool time_valid(void) { return rtc_time_valid(); }

uint32_t time_boot_count(void) { return g_boot; }

bool time_set(const struct tm *utc, TimeSource src, bool force) {
  if (!utc || src == TimeSource::None) return false;
  if (!force && rtc_time_valid() && rank(src) < rank(g_synced_by)) {
    return false;
  }
  if (!rtc_set(utc, src)) return false;
  set_system(utc);
  g_synced_by = src;
  return true;
}

void time_format(const TimeStamp &t, char *out, size_t n) {
  if (t.quality == TimeSource::None) {
    snprintf(out, n, "----------");
    return;
  }
  const time_t s = (time_t)(t.utc_ms / 1000);
  struct tm tm;
  gmtime_r(&s, &tm);
  strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}
