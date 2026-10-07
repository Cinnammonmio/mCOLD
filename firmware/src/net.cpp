#include "net.h"

#include <esp_attr.h>
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
  // Fixed address, network byte order; ip 0: DHCP.
  uint32_t ip, gw, mask, dns;
};
esp_netif_t *g_sta = nullptr;
Known g_join;                   // the network being joined, with its address
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
volatile bool g_want = false;
bool g_inited = false;          // driver, netif and SNTP set up

// The network last joined, through deep sleep: joined again directly on
// its channel and BSSID, with no scan. Forgotten the moment it refuses.
struct Fast {
  uint32_t magic;
  char ssid[33];
  uint8_t bssid[6];
  uint8_t channel;
  uint32_t last_up;             // mono_ms() it last gave an address
};
const uint32_t FAST_MAGIC = 0x4E465331;   // "NFS1"
RTC_DATA_ATTR Fast g_fast;
bool g_fast_tried = false;      // this session
bool g_joined_direct = false;   // the join in flight skipped the scan

const uint32_t CONNECT_TIMEOUT_MS = 20000;

// Runs through deep sleep (timekeep.h), so times kept across one compare.
uint32_t now_ms(void) { return mono_ms(); }

struct Lock {
  Lock() { xSemaphoreTake(g_mx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_mx); }
};

// ---- the list in NVS: s0..s4 and p0..p4 ---------------------------------

// The list is five fixed slots (decided 2026-10-07): a slot is empty when
// its ssid is, and keeps its place when another is cleared, so the app can
// name "slot 3" and mean the same network until it changes it. Stored
// networks from before it sit in slots 1..n, as they were written.
void load(void) {
  memset(g_nets, 0, sizeof(g_nets));
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
    char ki[4];
    snprintf(ki, sizeof(ki), "i%d", i);
    uint32_t a[4];
    n = sizeof(a);
    if (nvs_get_blob(h, ki, a, &n) == ESP_OK && n == sizeof(a)) {
      k.ip = a[0];
      k.gw = a[1];
      k.mask = a[2];
      k.dns = a[3];
    }
    g_nets[i] = k;
    g_n++;
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
    char ki[4];
    snprintf(ki, sizeof(ki), "i%d", i);
    if (g_nets[i].ssid[0]) {
      ok = ok && nvs_set_str(h, ks, g_nets[i].ssid) == ESP_OK &&
           nvs_set_str(h, kp, g_nets[i].pass) == ESP_OK;
      if (g_nets[i].ip) {
        const uint32_t a[4] = {g_nets[i].ip, g_nets[i].gw, g_nets[i].mask, g_nets[i].dns};
        ok = ok && nvs_set_blob(h, ki, a, sizeof(a)) == ESP_OK;
      } else {
        nvs_erase_key(h, ki);
      }
    } else {
      nvs_erase_key(h, ks);   // absent is fine
      nvs_erase_key(h, kp);
      nvs_erase_key(h, ki);
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
    int room = -1;
    for (int i = 0; i < NET_MAX; i++) {
      have = have || !strcmp(g_nets[i].ssid, k.ssid);
      if (room < 0 && !g_nets[i].ssid[0]) room = i;
    }
    if (!have && room >= 0) {
      g_nets[room] = k;
      g_n++;
    }
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

void join(const Known &k, const uint8_t *bssid, uint8_t channel) {
  wifi_config_t wc = {};
  memcpy(wc.sta.ssid, k.ssid, strlen(k.ssid));
  memcpy(wc.sta.password, k.pass, strlen(k.pass));
  // An open network only if it was stored without a password; WPA2 at
  // least otherwise, so a look-alike open access point cannot take the box.
  wc.sta.threshold.authmode = k.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  wc.sta.pmf_cfg.capable = true;
  g_joined_direct = bssid != nullptr;
  if (bssid) {
    wc.sta.bssid_set = true;
    memcpy(wc.sta.bssid, bssid, 6);
    wc.sta.channel = channel;
  }
  esp_wifi_set_config(WIFI_IF_STA, &wc);
  // DHCP unless this network has a fixed address; the fixed one is set
  // once associated (WIFI_EVENT_STA_CONNECTED), as ESP-IDF wants it.
  g_join = k;
  if (g_sta && !k.ip) esp_netif_dhcpc_start(g_sta);
  snprintf(g_cur, sizeof(g_cur), "%s", k.ssid);
  g_connecting = true;
  g_connect_at = now_ms();
  g_reconnects = g_reconnects + 1;
  esp_wifi_connect();
}

// Scan, then join the strongest known network in range -- unless it is
// the one that just refused us and there is another to try.
void attempt(void) {
  Known list[NET_MAX];
  int n = NET_MAX, known;
  {
    Lock l;
    known = g_n;
    memcpy(list, g_nets, sizeof(list));
  }
  if (!known) return;

  // First the network that worked last time, straight to it.
  if (!g_fast_tried && g_fast.magic == FAST_MAGIC) {
    g_fast_tried = true;
    for (int i = 0; i < n; i++) {
      if (strcmp(list[i].ssid, g_fast.ssid)) continue;
      printf("[net] joining %s directly (channel %u)\n", g_fast.ssid, g_fast.channel);
      join(list[i], g_fast.bssid, g_fast.channel);
      return;
    }
  }

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
    if (!list[i].ssid[0]) continue;      // an empty slot is not a hidden network
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
    printf("[net] none of the %d known networks in range\n", known);
    schedule_retry();
    return;
  }
  int pick = best;
  if (second >= 0 && !strcmp(list[best].ssid, g_failed)) pick = second;
  printf("[net] joining %s (%d dBm)\n", list[pick].ssid,
         pick == best ? best_rssi : second_rssi);
  join(list[pick], nullptr, 0);
}

void set_fixed_ip(void) {
  if (!g_sta || !g_join.ip) return;
  esp_netif_dhcpc_stop(g_sta);
  esp_netif_ip_info_t info = {};
  info.ip.addr = g_join.ip;
  info.gw.addr = g_join.gw;
  info.netmask.addr = g_join.mask;
  if (esp_netif_set_ip_info(g_sta, &info) != ESP_OK) {
    printf("[net] fixed address refused by the stack\n");
    return;
  }
  esp_netif_dns_info_t d = {};
  d.ip.type = ESP_IPADDR_TYPE_V4;
  d.ip.u_addr.ip4.addr = g_join.dns ? g_join.dns : g_join.gw;
  esp_netif_set_dns_info(g_sta, ESP_NETIF_DNS_MAIN, &d);
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    set_fixed_ip();     // a fixed address; DHCP needs nothing here
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const bool was = g_connected;
    g_connected = false;
    g_connecting = false;
    g_up_since = 0;
    g_ip[0] = 0;
    // The reason, so "will not connect" can be told apart: 201 no AP
    // found, 15/204 handshake (password), 2/200 beacon lost, 205 the
    // connection itself failed.
    const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
    if (!g_want) return;            // we hung up ourselves
    if (was) {
      printf("[net] disconnected from %s, reason %u\n", g_cur, e->reason);
    } else {
      printf("[net] %s refused, reason %u\n", g_cur, e->reason);
      snprintf(g_failed, sizeof(g_failed), "%s", g_cur);   // try another next
      g_fast.magic = 0;             // and not directly again
    }
    // A direct join that failed is retried at once, with a scan.
    // Only the direct one: a refusal after a scan backs off as usual, or
    // a network that refuses every time would be retried without pause.
    if (!was && g_joined_direct) g_retry_at = 1;
    else schedule_retry();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&e->ip_info.ip));
    g_connected = true;
    g_connecting = false;
    g_up_since = now_ms();
    g_backoff_ms = 2000;
    g_failed[0] = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      g_fast.magic = FAST_MAGIC;
      snprintf(g_fast.ssid, sizeof(g_fast.ssid), "%s", g_cur);
      memcpy(g_fast.bssid, ap.bssid, 6);
      g_fast.channel = ap.primary;
    }
    g_fast.last_up = g_up_since ? g_up_since : 1;
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

bool bring_up(void) {
  esp_netif_init();
  esp_event_loop_create_default();
  g_sta = esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK) {
    printf("[net] Wi-Fi did not start\n");
    return false;
  }
  // The credentials are ours to keep, in our own NVS keys; the driver is
  // not to store its own copy.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, nullptr);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_event, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
  esp_wifi_set_mode(WIFI_MODE_STA);
  // Time from the network once it is up; SNTP waits for an address by
  // itself and re-syncs every hour.
  esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  sc.sync_cb = on_sntp;
  esp_netif_sntp_init(&sc);
  g_inited = true;
  return true;
}

void task(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(g_connecting ? 200 : 1000));
    if (g_ntp_pending) {
      g_ntp_pending = false;
      apply_ntp();
    }
    int n;
    {
      Lock l;
      n = g_n;
    }
    if (!n || !g_want) {
      if (g_started) net_stop();
      continue;
    }
    if (!g_inited && !bring_up()) continue;
    if (!g_started) {
      esp_wifi_start();
      g_started = true;
      g_fast_tried = false;
      g_retry_at = 1;
      g_backoff_ms = 2000;
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
  if (g_fast.magic != FAST_MAGIC) memset(&g_fast, 0, sizeof(g_fast));
  xTaskCreatePinnedToCore(task, "net", 6144, nullptr, 2, nullptr, 0);
}

void net_want(bool on) { g_want = on; }

void net_stop(void) {
  g_want = false;
  if (!g_started) return;
  esp_wifi_disconnect();
  esp_wifi_stop();
  g_started = false;
  g_connected = false;
  g_connecting = false;
  g_up_since = 0;
  g_ip[0] = 0;
}

namespace {
// The slot a network is in; -1 if it is not known. Under the lock.
int slot_of(const char *ssid) {
  for (int i = 0; i < NET_MAX; i++) {
    if (g_nets[i].ssid[0] && !strcmp(g_nets[i].ssid, ssid)) return i;
  }
  return -1;
}
}  // namespace

bool net_add(const char *ssid, const char *pass) {
  if (!fits(ssid, pass)) return false;
  {
    Lock l;
    int i = slot_of(ssid);
    if (i < 0) {
      for (i = 0; i < NET_MAX && g_nets[i].ssid[0]; i++) {
      }
      if (i == NET_MAX) return false;
      g_n++;
    }
    snprintf(g_nets[i].ssid, sizeof(g_nets[i].ssid), "%s", ssid);
    snprintf(g_nets[i].pass, sizeof(g_nets[i].pass), "%s", pass ? pass : "");
    // A new network starts on DHCP; a known one keeps its address.
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
    const int i = slot_of(ssid);
    if (i < 0) return false;
    memset(&g_nets[i], 0, sizeof(g_nets[i]));
    g_n--;
    if (!save()) return false;
  }
  if (g_connected && !strcmp(g_cur, ssid)) esp_wifi_disconnect();
  return true;
}

bool net_slot_ssid(int slot, char *ssid, int n) {
  Lock l;
  if (slot < 0 || slot >= NET_MAX || !g_nets[slot].ssid[0]) return false;
  snprintf(ssid, (size_t)n, "%s", g_nets[slot].ssid);
  return true;
}

bool net_slot_put(int slot, const char *ssid, const char *pass) {
  if (slot < 0 || slot >= NET_MAX || !fits(ssid, pass ? pass : "")) return false;
  char was[33] = "";
  {
    Lock l;
    const bool had = g_nets[slot].ssid[0] != 0;
    const bool same = had && !strcmp(g_nets[slot].ssid, ssid);
    if (!same && !pass) return false;              // a new name has no password to keep
    for (int i = 0; i < NET_MAX; i++) {
      if (i != slot && !strcmp(g_nets[i].ssid, ssid)) return false;   // one slot per network
    }
    if (had) snprintf(was, sizeof(was), "%s", g_nets[slot].ssid);
    if (!had) g_n++;
    if (!same) memset(&g_nets[slot], 0, sizeof(g_nets[slot]));   // DHCP again, no old address
    snprintf(g_nets[slot].ssid, sizeof(g_nets[slot].ssid), "%s", ssid);
    if (pass) snprintf(g_nets[slot].pass, sizeof(g_nets[slot].pass), "%s", pass);
    if (!save()) return false;
  }
  if (g_connected && (!strcmp(g_cur, ssid) || !strcmp(g_cur, was))) esp_wifi_disconnect();
  if (!strcmp(g_failed, ssid)) g_failed[0] = 0;
  g_backoff_ms = 2000;
  g_retry_at = 1;
  return true;
}

bool net_slot_clear(int slot) {
  char name[33];
  if (!net_slot_ssid(slot, name, sizeof(name))) return false;
  return net_remove(name);
}

namespace {
bool quad(const char *s, uint32_t *out) {
  esp_ip4_addr_t a;
  if (!s || !*s || esp_netif_str_to_ip4(s, &a) != ESP_OK) return false;
  *out = a.addr;
  return true;
}
void unquad(uint32_t a, char *out) {
  if (!a) {
    out[0] = 0;
    return;
  }
  esp_ip4_addr_t x;
  x.addr = a;
  snprintf(out, 16, IPSTR, IP2STR(&x));
}
}  // namespace

bool net_set_ip(const char *ssid, const NetIp &ip) {
  uint32_t a = 0, g = 0, m = 0, d = 0;
  if (!ip.dhcp) {
    if (!quad(ip.ip, &a) || !quad(ip.gateway, &g) || !quad(ip.subnet, &m)) return false;
    if (ip.dns[0] && !quad(ip.dns, &d)) return false;
    // A mask is ones then zeros, and the gateway is on the same subnet.
    const uint32_t inv = ~__builtin_bswap32(m);   // host order, inverted: 0..0 1..1
    if (inv == 0xFFFFFFFFu || (inv & (inv + 1)) || (a & m) != (g & m)) return false;
  }
  {
    Lock l;
    const int i = slot_of(ssid);
    if (i < 0) return false;
    g_nets[i].ip = a;
    g_nets[i].gw = g;
    g_nets[i].mask = m;
    g_nets[i].dns = d;
    if (!save()) return false;
  }
  // In use: join again with the new address.
  if (g_connected && !strcmp(g_cur, ssid)) esp_wifi_disconnect();
  g_fast.magic = 0;
  return true;
}

bool net_get_ip(int i, NetIp *out) {
  Lock l;
  if (i < 0 || i >= NET_MAX || !g_nets[i].ssid[0]) return false;
  *out = {};
  out->dhcp = g_nets[i].ip == 0;
  unquad(g_nets[i].ip, out->ip);
  unquad(g_nets[i].gw, out->gateway);
  unquad(g_nets[i].mask, out->subnet);
  unquad(g_nets[i].dns, out->dns);
  return true;
}

int net_count(void) {
  Lock l;
  return g_n;
}

bool net_known(int i, char *ssid, int n) {
  Lock l;
  if (i < 0) return false;
  for (int s = 0; s < NET_MAX; s++) {      // the i-th network that is there
    if (!g_nets[s].ssid[0] || i--) continue;
    snprintf(ssid, (size_t)n, "%s", g_nets[s].ssid);
    return true;
  }
  return false;
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
  out->last_up_ms = g_fast.magic == FAST_MAGIC ? g_fast.last_up : 0;
  wifi_ap_record_t ap;
  if (g_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
}
