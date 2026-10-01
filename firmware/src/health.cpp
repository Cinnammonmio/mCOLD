#include "health.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <string.h>

static DevHealth g_health[(size_t)Dev::Count];
static SemaphoreHandle_t g_lock;

static const char *const NAMES[(size_t)Dev::Count] = {
    "accel", "fuel", "current", "pd", "nfc", "rtc", "charger",
    "thermo", "gnss", "display", "leds", "buzzer", "sd",
};

static uint32_t now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

void health_init(void) {
  memset(g_health, 0, sizeof(g_health));
  for (size_t i = 0; i < (size_t)Dev::Count; i++) {
    g_health[i].state = DevState::Unknown;
  }
  g_lock = xSemaphoreCreateMutex();
}

// The lock is held only to swap a few words, never across a bus
// transaction, so no caller can be blocked here for longer than another
// caller's handful of assignments.
static inline bool take(void) {
  return g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) == pdTRUE;
}
static inline void give(void) {
  if (g_lock) xSemaphoreGive(g_lock);
}

void health_ok(Dev d) {
  if (d >= Dev::Count) return;
  if (!take()) return;
  DevHealth *h = &g_health[(size_t)d];
  h->state = DevState::Ok;
  h->ok_count++;
  h->consecutive_fails = 0;
  h->last_error = 0;
  h->last_ok_ms = now_ms();
  h->last_attempt_ms = h->last_ok_ms;
  give();
}

void health_fail(Dev d, int32_t err) {
  if (d >= Dev::Count) return;
  if (!take()) return;
  DevHealth *h = &g_health[(size_t)d];
  h->fail_count++;
  h->consecutive_fails++;
  h->last_error = err;
  h->last_attempt_ms = now_ms();
  if (h->consecutive_fails >= HEALTH_FAIL_LIMIT) {
    // A part that has worked at some point is Degraded rather than
    // Failed: the distinction matters because it separates "this unit
    // was built wrong" from "something came loose in the field", and
    // those want different responses from whoever reads the report.
    h->state = h->last_ok_ms ? DevState::Degraded : DevState::Failed;
  }
  give();
}

void health_absent(Dev d) {
  if (d >= Dev::Count) return;
  if (!take()) return;
  g_health[(size_t)d].state = DevState::Absent;
  give();
}

DevState health_state(Dev d) {
  if (d >= Dev::Count) return DevState::Unknown;
  return g_health[(size_t)d].state;
}

const DevHealth *health_get(Dev d) {
  if (d >= Dev::Count) return nullptr;
  return &g_health[(size_t)d];
}

const char *health_name(Dev d) {
  return d < Dev::Count ? NAMES[(size_t)d] : "?";
}

const char *health_state_name(DevState s) {
  switch (s) {
    case DevState::Unknown:  return "unknown";
    case DevState::Ok:       return "ok";
    case DevState::Degraded: return "degraded";
    case DevState::Failed:   return "failed";
    case DevState::Absent:   return "absent";
  }
  return "?";
}

bool health_should_try(Dev d, uint32_t now) {
  if (d >= Dev::Count) return false;
  const DevHealth *h = &g_health[(size_t)d];
  switch (h->state) {
    case DevState::Absent:
      return false;
    case DevState::Degraded:
    case DevState::Failed:
      // Backed off, but never given up on. A reseated connector or a
      // part that was simply slow to come up recovers without anyone
      // having to power-cycle the unit.
      return (uint32_t)(now - h->last_attempt_ms) >= HEALTH_RETRY_MS;
    default:
      return true;
  }
}

void health_summary(int *ok, int *degraded, int *failed, int *unknown) {
  int o = 0, d = 0, f = 0, u = 0;
  for (size_t i = 0; i < (size_t)Dev::Count; i++) {
    switch (g_health[i].state) {
      case DevState::Ok:       o++; break;
      case DevState::Degraded: d++; break;
      case DevState::Failed:   f++; break;
      case DevState::Unknown:  u++; break;
      case DevState::Absent:   break;
    }
  }
  if (ok) *ok = o;
  if (degraded) *degraded = d;
  if (failed) *failed = f;
  if (unknown) *unknown = u;
}
