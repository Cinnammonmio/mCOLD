#include "net.h"

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

namespace {

const char *NS = "net";

char g_ssid[33] = "";
char g_pass[65] = "";
volatile bool g_started = false;
volatile bool g_connected = false;
volatile uint32_t g_up_since = 0;
volatile uint32_t g_reconnects = 0;
volatile uint32_t g_retry_at = 0;
uint32_t g_backoff_ms = 2000;
char g_ip[16] = "";
esp_netif_t *g_netif = nullptr;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void load(void) {
  nvs_handle_t h;
  g_ssid[0] = g_pass[0] = 0;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
  size_t n = sizeof(g_ssid);
  if (nvs_get_str(h, "ssid", g_ssid, &n) != ESP_OK) g_ssid[0] = 0;
  n = sizeof(g_pass);
  if (nvs_get_str(h, "pass", g_pass, &n) != ESP_OK) g_pass[0] = 0;
  nvs_close(h);
}

void apply(void) {
  wifi_config_t wc = {};
  memcpy(wc.sta.ssid, g_ssid, strlen(g_ssid));
  memcpy(wc.sta.password, g_pass, strlen(g_pass));
  // An open network if there is no password; WPA2 at least otherwise,
  // so a look-alike open access point cannot take the box.
  wc.sta.threshold.authmode = g_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  wc.sta.pmf_cfg.capable = true;
  esp_wifi_set_config(WIFI_IF_STA, &wc);
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (g_ssid[0]) esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const bool was = g_connected;
    g_connected = false;
    g_up_since = 0;
    g_ip[0] = 0;
    if (was) printf("[net] disconnected\n");
    // Back off, doubling to a minute: an access point that is gone
    // should not cost the battery a connection attempt every second.
    g_retry_at = now_ms() + g_backoff_ms;
    if (g_backoff_ms < 60000) g_backoff_ms *= 2;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&e->ip_info.ip));
    g_connected = true;
    g_up_since = now_ms();
    g_backoff_ms = 2000;
    printf("[net] connected to %s, %s\n", g_ssid, g_ip);
  }
}

void task(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t at = g_retry_at;
    if (g_ssid[0] && !g_connected && at && (int32_t)(now_ms() - at) >= 0) {
      g_retry_at = 0;
      g_reconnects = g_reconnects + 1;
      esp_wifi_connect();
    }
  }
}

}  // namespace

void net_start(void) {
  load();
  esp_netif_init();
  esp_event_loop_create_default();
  g_netif = esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK) {
    printf("[net] Wi-Fi did not start\n");
    return;
  }
  // The credentials are ours to keep, in our own NVS keys; the driver is
  // not to store its own copy.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
  esp_wifi_set_mode(WIFI_MODE_STA);
  if (g_ssid[0]) {
    apply();
    esp_wifi_start();
    g_started = true;
  }
  xTaskCreatePinnedToCore(task, "net", 3072, nullptr, 2, nullptr, 0);
}

bool net_set(const char *ssid, const char *pass) {
  const size_t sl = ssid ? strlen(ssid) : 0, pl = pass ? strlen(pass) : 0;
  if (sl < 1 || sl > 32 || (pl && (pl < 8 || pl > 63))) return false;
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_str(h, "ssid", ssid) == ESP_OK &&
                  nvs_set_str(h, "pass", pass ? pass : "") == ESP_OK &&
                  nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (!ok) return false;
  load();
  esp_wifi_disconnect();
  apply();
  g_backoff_ms = 2000;
  if (!g_started) {
    esp_wifi_start();       // STA_START then connects
    g_started = true;
  } else {
    esp_wifi_connect();
  }
  return true;
}

bool net_configured(void) { return g_ssid[0] != 0; }

void net_status(NetStatus *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->configured = g_ssid[0] != 0;
  out->connected = g_connected;
  snprintf(out->ssid, sizeof(out->ssid), "%s", g_ssid);
  snprintf(out->ip, sizeof(out->ip), "%s", g_ip);
  out->reconnects = g_reconnects;
  out->up_since_ms = g_up_since;
  wifi_ap_record_t ap;
  if (g_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
}
