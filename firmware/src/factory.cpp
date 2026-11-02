#include "factory.h"

#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "buzzer.h"
#include "bus.h"
#include "flashlog_test.h"
#include "gnss.h"
#include "leds.h"
#include "net.h"
#include "temp.h"
#include "timekeep.h"
#include "trip.h"

namespace {

// What `i2c` at the console expects on a built board (main.cpp).
const uint8_t I2C_EXPECTED[] = {0x18, 0x36, 0x40, 0x42, 0x53, 0x57, 0x68, 0x6B};

bool g_known = false, g_on = false;

bool wants(const cJSON *items, const char *name) {
  if (!cJSON_IsArray(items)) return true;          // none named: all of them
  const cJSON *it;
  cJSON_ArrayForEach(it, items) {
    if (cJSON_IsString(it) && !strcmp(it->valuestring, name)) return true;
  }
  return false;
}

bool test_i2c(cJSON *o) {
  uint8_t found[32];
  int sda = 0, scl = 0;
  const int n = i2c_scan(found, 32, &sda, &scl);
  cJSON_AddBoolToObject(o, "sda", sda);
  cJSON_AddBoolToObject(o, "scl", scl);
  if (n < 0) {
    cJSON_AddStringToObject(o, "err", "bus busy");
    return false;
  }
  int got = 0;
  cJSON *miss = cJSON_AddArrayToObject(o, "missing");
  for (uint8_t a : I2C_EXPECTED) {
    bool here = false;
    for (int i = 0; i < n; i++) here = here || found[i] == a;
    if (here) {
      got++;
    } else {
      char h[5];
      snprintf(h, sizeof(h), "0x%02X", a);
      cJSON_AddItemToArray(miss, cJSON_CreateString(h));
    }
  }
  cJSON_AddNumberToObject(o, "found", got);
  cJSON_AddNumberToObject(o, "expected", (int)sizeof(I2C_EXPECTED));
  return got == (int)sizeof(I2C_EXPECTED);
}

bool test_temp(cJSON *o) {
  float c = 0;
  const TempStatus s = temp_sample(&c);
  cJSON_AddStringToObject(o, "state", temp_status_name(s));
  cJSON_AddStringToObject(o, "sensor", temp_sensor_name(temp_sensor()));
  if (s != TempStatus::Ok) return false;
  cJSON_AddNumberToObject(o, "c", c);
  return c > -40 && c < 85;
}

// Sentences with a good checksum over the window, as `gnss` at the console
// counts them: bytes alone do not count (a wrong baud gives bytes too).
// The GNSS task owns the UART: it is asked to hold a session open, and
// only its counters are read here.
bool test_gnss(cJSON *o, int seconds) {
  gnss_hold_for((uint32_t)(seconds + 15) * 1000);
  for (int i = 0; i < 80 && !gnss_is_on(); i++) vTaskDelay(pdMS_TO_TICKS(100));
  if (!gnss_is_on()) {
    cJSON_AddStringToObject(o, "err", "the module did not power up (rail, or its health backing off)");
    return false;
  }
  GnssStats a, b;
  gnss_stats(&a);
  vTaskDelay(pdMS_TO_TICKS((uint32_t)seconds * 1000));
  gnss_stats(&b);
  const uint32_t good = b.sentences - a.sentences, bad = b.bad_checksum - a.bad_checksum;
  cJSON_AddNumberToObject(o, "good", good);
  cJSON_AddNumberToObject(o, "bad", bad);
  cJSON_AddNumberToObject(o, "bytes", b.bytes - a.bytes);
  return good >= 3;
}

bool test_log(cJSON *o) {
  // The self-test points the log at a RAM image while it runs: a running
  // trip's samples in those two minutes would go there and be lost.
  TripStatus ts;
  trip_status(&ts);
  if (ts.active) {
    cJSON_AddStringToObject(o, "err", "a trip is running");
    return false;
  }
  const int f = flashlog_selftest();
  cJSON_AddNumberToObject(o, "failures", f);
  return f == 0;
}

}  // namespace

bool factory_mode(void) {
  if (!g_known) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("factory", NVS_READONLY, &h) == ESP_OK) {
      nvs_get_u8(h, "on", &v);
      nvs_close(h);
    }
    g_on = v != 0;
    g_known = true;
  }
  return g_on;
}

void factory_set(bool on) {
  nvs_handle_t h;
  if (nvs_open("factory", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "on", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
  g_on = on;
  g_known = true;
  printf("[factory] mode %s\n", on ? "on" : "off");
}

void factory_test(cJSON *out, const cJSON *req) {
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(req, "items");
  const cJSON *js = cJSON_GetObjectItemCaseSensitive(req, "gnss_s");
  int gs = cJSON_IsNumber(js) ? (int)js->valuedouble : 15;
  if (gs < 3) gs = 3;
  if (gs > 60) gs = 60;
  bool all = true;
  struct { const char *name; } names[] = {{"i2c"}, {"temp"}, {"gnss"}, {"logtest"}};
  for (auto &n : names) {
    if (!wants(items, n.name)) continue;
    cJSON *o = cJSON_AddObjectToObject(out, n.name);
    bool ok = false;
    if (!strcmp(n.name, "i2c")) ok = test_i2c(o);
    else if (!strcmp(n.name, "temp")) ok = test_temp(o);
    else if (!strcmp(n.name, "gnss")) ok = test_gnss(o, gs);
    else ok = test_log(o);
    cJSON_AddBoolToObject(o, "ok", ok);
    all = all && ok;
  }
  cJSON_ReplaceItemInObject(out, "ok", cJSON_CreateBool(all));
}

void factory_identify(int seconds) {
  if (seconds < 1) seconds = 1;
  if (seconds > 30) seconds = 30;
  for (int s = 0; s < seconds; s++) {
    for (int i = 0; i < LED_COUNT; i++) leds_pulse(i, 255, 255, 255, 500);
    // lights and buzzer take turns (indicate.h: the buzzer's peak current)
    vTaskDelay(pdMS_TO_TICKS(600));
    if (s % 2 == 0) buzzer_beep(120);
    vTaskDelay(pdMS_TO_TICKS(400));
  }
}

void factory_debug(cJSON *out) {
  static const char *const RESET[] = {"UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT",
                                      "TASK_WDT", "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO",
                                      "USB", "JTAG", "EFUSE", "PWR_GLITCH", "CPU_LOCKUP"};
  const int rr = (int)esp_reset_reason();
  cJSON_AddStringToObject(out, "reset", rr >= 0 && rr < (int)(sizeof(RESET) / sizeof(*RESET)) ? RESET[rr] : "?");
  TimeStamp t;
  time_now(&t);
  cJSON_AddNumberToObject(out, "boot", t.boot);
  // Since this reset (or wake): mono_ms() is the RTC's count, which a
  // software reset does not restart (778 s just after `reboot` on 002).
  cJSON_AddNumberToObject(out, "uptime_s", (double)(esp_timer_get_time() / 1000000));
  cJSON_AddBoolToObject(out, "coredump", esp_core_dump_image_check() == ESP_OK);
  cJSON_AddNumberToObject(out, "heap", esp_get_free_heap_size());
  cJSON_AddNumberToObject(out, "min_heap", esp_get_minimum_free_heap_size());
  cJSON_AddNumberToObject(out, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  NetStatus ns;
  net_status(&ns);
  cJSON_AddStringToObject(out, "ip", ns.connected ? ns.ip : "");
  cJSON_AddStringToObject(out, "ssid", ns.connected ? ns.ssid : "");
  cJSON_AddNumberToObject(out, "wifi_fail_reason", ns.fail_reason);
  const esp_app_desc_t *d = esp_app_get_description();
  cJSON_AddStringToObject(out, "fw", d->version);
  char elf[17];
  esp_app_get_elf_sha256(elf, sizeof(elf));
  cJSON_AddStringToObject(out, "elf", elf);
  cJSON_AddBoolToObject(out, "factory_mode", factory_mode());
}
