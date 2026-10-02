#include "net.h"

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rtcclock.h"
#include "timekeep.h"
#include "trip.h"

namespace {

const char *NS = "net";

struct Known {
  char ssid[33];
  char pass[65];
};
Known g_nets[NET_MAX];
int g_n = 0;
SemaphoreHandle_t g_mx = nullptr;

char g_cur[33] = "";            // the network being joined, or joined
char g_failed[33] = "";         // the last one that refused us
volatile bool g_started = false;
volatile bool g_connected = false;
volatile bool g_connecting = false;
volatile uint32_t g_connect_at = 0;
volatile uint32_t g_up_since = 0;
volatile uint32_t g_reconnects = 0;
volatile uint32_t g_retry_at = 1;   // due at once
uint32_t g_backoff_ms = 2000;
char g_ip[16] = "";

const uint32_t CONNECT_TIMEOUT_MS = 20000;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

struct Lock {
  Lock() { xSemaphoreTake(g_mx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_mx); }
};

// ---- the list in NVS: s0..s4 and p0..p4 ---------------------------------

void load(void) {
  g_n = 0;
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
  for (int i = 0; i < NET_MAX; i++) {
    char ks[4], kp[4];
    snprintf(ks, sizeof(ks), "s%d", i);
    snprintf(kp, sizeof(kp), "p%d", i);
    Known k = {};
    size_t n = sizeof(k.ssid);
    if (nvs_get_str(h, ks, k.ssid, &n) != ESP_OK || !k.ssid[0]) continue;
    n = sizeof(k.pass);
    if (nvs_get_str(h, kp, k.pass, &n) != ESP_OK) k.pass[0] = 0;
    g_nets[g_n++] = k;
  }
  nvs_close(h);
}

bool save(void) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = true;
  for (int i = 0; i < NET_MAX; i++) {
    char ks[4], kp[4];
    snprintf(ks, sizeof(ks), "s%d", i);
    snprintf(kp, sizeof(kp), "p%d", i);
    if (i < g_n) {
      ok = ok && nvs_set_str(h, ks, g_nets[i].ssid) == ESP_OK &&
           nvs_set_str(h, kp, g_nets[i].pass) == ESP_OK;
    } else {
      nvs_erase_key(h, ks);   // absent is fine
      nvs_erase_key(h, kp);
    }
  }
  ok = ok && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

// The first firmware with Wi-Fi kept one network under "ssid"/"pass".
// Carried into the list once, then those keys go.
void migrate(void) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  Known k = {};
  size_t n = sizeof(k.ssid);
  if (nvs_get_str(h, "ssid", k.ssid, &n) == ESP_OK && k.ssid[0]) {
    n = sizeof(k.pass);
    if (nvs_get_str(h, "pass", k.pass, &n) != ESP_OK) k.pass[0] = 0;
    bool have = false;
    for (int i = 0; i < g_n; i++) have = have || !strcmp(g_nets[i].ssid, k.ssid);
    if (!have && g_n < NET_MAX) g_nets[g_n++] = k;
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
    save();
    return;
  }
  nvs_close(h);
}

// ---- joining ------------------------------------------------------------

void schedule_retry(void) {
  // Back off, doubling to a minute: networks that are not there should
  // not cost the battery a scan every second.
  g_retry_at = now_ms() + g_backoff_ms;
  if (!g_retry_at) g_retry_at = 1;
  if (g_backoff_ms < 60000) g_backoff_ms *= 2;
}

// Scan, then join the strongest known network in range -- unless it is
// the one that just refused us and there is another to try.
void attempt(void) {
  Known list[NET_MAX];
  int n;
  {
    Lock l;
    n = g_n;
    memcpy(list, g_nets, sizeof(list));
  }
  if (!n) return;

  wifi_scan_config_t sc = {};
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
    schedule_retry();
    return;
  }
  // Scan results in static storage, not on this task's stack: 24 of them
  // are over 2 KB, and on a 4 KB stack they overflowed it -- into memory
  // the Wi-Fi driver then crashed on (2026-10-02).
  static wifi_ap_record_t aps[24];
  uint16_t found = 24;
  if (esp_wifi_scan_get_ap_records(&found, aps) != ESP_OK) found = 0;

  int best = -1, second = -1;
  int8_t best_rssi = -128, second_rssi = -128;
  for (int i = 0; i < n; i++) {
    int8_t rssi = -128;
    bool seen = false;
    for (int a = 0; a < found; a++) {
      if (!strcmp((const char *)aps[a].ssid, list[i].ssid) && aps[a].rssi > rssi) {
        rssi = aps[a].rssi;
        seen = true;
      }
    }
    if (!seen) continue;
    if (best < 0 || rssi > best_rssi) {
      second = best;
      second_rssi = best_rssi;
      best = i;
      best_rssi = rssi;
    } else if (second < 0 || rssi > second_rssi) {
      second = i;
      second_rssi = rssi;
    }
  }
  if (best < 0) {
    printf("[net] none of the %d known networks in range\n", n);
    schedule_retry();
    return;
  }
  int pick = best;
  if (second >= 0 && !strcmp(list[best].ssid, g_failed)) pick = second;
  const Known &k = list[pick];

  wifi_config_t wc = {};
  memcpy(wc.sta.ssid, k.ssid, strlen(k.ssid));
  memcpy(wc.sta.password, k.pass, strlen(k.pass));
  // An open network only if it was stored without a password; WPA2 at
  // least otherwise, so a look-alike open access point cannot take the box.
  wc.sta.threshold.authmode = k.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  wc.sta.pmf_cfg.capable = true;
  esp_wifi_set_config(WIFI_IF_STA, &wc);
  snprintf(g_cur, sizeof(g_cur), "%s", k.ssid);
  g_connecting = true;
  g_connect_at = now_ms();
  g_reconnects = g_reconnects + 1;
  printf("[net] joining %s (%d dBm)\n", k.ssid, pick == best ? best_rssi : second_rssi);
  esp_wifi_connect();
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const bool was = g_connected;
    g_connected = false;
    g_connecting = false;
    g_up_since = 0;
    g_ip[0] = 0;
    // The reason, so "will not connect" can be told apart: 201 no AP
    // found, 15/204 handshake (password), 2/200 beacon lost, 205 the
    // connection itself failed.
    const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
    if (was) {
      printf("[net] disconnected from %s, reason %u\n", g_cur, e->reason);
    } else {
      printf("[net] %s refused, reason %u\n", g_cur, e->reason);
      snprintf(g_failed, sizeof(g_failed), "%s", g_cur);   // try another next
    }
    schedule_retry();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&e->ip_info.ip));
    g_connected = true;
    g_connecting = false;
    g_up_since = now_ms();
    g_backoff_ms = 2000;
    g_failed[0] = 0;
    printf("[net] connected to %s, %s\n", g_cur, g_ip);
  }
}

// ---- time from the network --------------------------------------------------

// SNTP answered. Its callback runs in the network stack's own task, which
// must not be held up by an I2C write and the RTC's 1.2 s oscillator
// check -- so it only raises this flag, and the work happens below.
volatile bool g_ntp_pending = false;
void on_sntp(struct timeval *) { g_ntp_pending = true; }

// Without a backup cell the RTC's time is gone after a power-off, and
// until something sets it records carry no time. A time server answers
// within seconds of Wi-Fi coming up, so this is usually the first source
// back (GNSS needs open sky, the app needs a tap). With the cell, it
// corrects the crystal's drift.
void apply_ntp(void) {
  const time_t now = time(nullptr);       // SNTP has set the system clock
  struct tm utc;
  gmtime_r(&now, &utc);
  // What the RTC said before, for the trip's record of the correction.
  uint32_t before = 0;
  struct tm rt;
  if (rtc_time_valid() && rtc_get(&rt) && rtc_time_valid()) before = (uint32_t)mktime(&rt);
  if (!time_set(&utc, TimeSource::Ntp, false)) return;
  const long drift = before ? (long)now - (long)before : 0;
  // Re-syncs come every hour; only a real correction goes in the log.
  if (!before || drift > 2 || drift < -2) {
    trip_note_time_set(TimeSource::Ntp, before);
    if (before) printf("[net] clock corrected by %ld s from NTP\n", drift);
    else printf("[net] clock set from NTP\n");
  } else {
    printf("[net] NTP agrees with the clock (%+ld s)\n", drift);
  }
}

void task(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (g_ntp_pending) {
      g_ntp_pending = false;
      apply_ntp();
    }
    int n;
    {
      Lock l;
      n = g_n;
    }
    if (!n) continue;
    if (!g_started) {
      esp_wifi_start();
      g_started = true;
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (g_connecting && now_ms() - g_connect_at > CONNECT_TIMEOUT_MS) {
      // No answer at all: give up on this attempt and try again.
      g_connecting = false;
      snprintf(g_failed, sizeof(g_failed), "%s", g_cur);
      esp_wifi_disconnect();
      schedule_retry();
    }
    const uint32_t at = g_retry_at;
    if (!g_connected && !g_connecting && at && (int32_t)(now_ms() - at) >= 0) {
      g_retry_at = 0;
      attempt();
    }
  }
}

bool fits(const char *ssid, const char *pass) {
  const size_t sl = ssid ? strlen(ssid) : 0, pl = pass ? strlen(pass) : 0;
  return sl >= 1 && sl <= 32 && (!pl || (pl >= 8 && pl <= 63));
}

}  // namespace

void net_start(void) {
  g_mx = xSemaphoreCreateMutex();
  load();
  migrate();
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK) {
    printf("[net] Wi-Fi did not start\n");
    return;
  }
  // The credentials are ours to keep, in our own NVS keys; the driver is
  // not to store its own copy.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
  esp_wifi_set_mode(WIFI_MODE_STA);
  // Time from the network once it is up; SNTP waits for an address by
  // itself and re-syncs every hour.
  esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  sc.sync_cb = on_sntp;
  esp_netif_sntp_init(&sc);
  xTaskCreatePinnedToCore(task, "net", 6144, nullptr, 2, nullptr, 0);
}

bool net_add(const char *ssid, const char *pass) {
  if (!fits(ssid, pass)) return false;
  {
    Lock l;
    int i = 0;
    while (i < g_n && strcmp(g_nets[i].ssid, ssid)) i++;
    if (i == g_n) {
      if (g_n >= NET_MAX) return false;
      g_n++;
    }
    snprintf(g_nets[i].ssid, sizeof(g_nets[i].ssid), "%s", ssid);
    snprintf(g_nets[i].pass, sizeof(g_nets[i].pass), "%s", pass ? pass : "");
    if (!save()) return false;
  }
  // A changed password for the network in use: rejoin with it.
  if (g_connected && !strcmp(g_cur, ssid)) esp_wifi_disconnect();
  if (!strcmp(g_failed, ssid)) g_failed[0] = 0;
  g_backoff_ms = 2000;
  g_retry_at = 1;
  return true;
}

bool net_remove(const char *ssid) {
  {
    Lock l;
    int i = 0;
    while (i < g_n && strcmp(g_nets[i].ssid, ssid)) i++;
    if (i == g_n) return false;
    for (; i + 1 < g_n; i++) g_nets[i] = g_nets[i + 1];
    g_n--;
    if (!save()) return false;
  }
  if (g_connected && !strcmp(g_cur, ssid)) esp_wifi_disconnect();
  return true;
}

int net_count(void) {
  Lock l;
  return g_n;
}

bool net_known(int i, char *ssid, int n) {
  Lock l;
  if (i < 0 || i >= g_n) return false;
  snprintf(ssid, (size_t)n, "%s", g_nets[i].ssid);
  return true;
}

bool net_configured(void) { return net_count() > 0; }

void net_status(NetStatus *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->known = net_count();
  out->configured = out->known > 0;
  out->connected = g_connected;
  if (g_connected) snprintf(out->ssid, sizeof(out->ssid), "%s", g_cur);
  snprintf(out->ip, sizeof(out->ip), "%s", g_ip);
  out->reconnects = g_reconnects;
  out->up_since_ms = g_up_since;
  wifi_ap_record_t ap;
  if (g_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
}
