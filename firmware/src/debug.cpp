#include "debug.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <mqtt_client.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>
#include <sys/lock.h>    // the FILE lock: newlib here has no flockfile()

#include "factory.h"
#include "net.h"
#include "pm.h"
#include "power.h"
#include "rpc.h"
#include "timekeep.h"
#include "uplink.h"
#include "secrets_gen.h"   // [debug] host, generated and gitignored: tools/secrets_gen.py

namespace {

const char *const CH_NAMES[] = {"power", "sys", "net", "log"};
const int CH_COUNT = 4;
const uint32_t CH_POWER = 1, CH_SYS = 2, CH_NET = 4, CH_LOG = 8;
const uint32_t CH_ALL = CH_POWER | CH_SYS | CH_NET | CH_LOG;
const uint32_t MIN_USB = 60, MIN_BATTERY = 15;       // the default, and on battery the most
const uint32_t MAX_USB_MIN = 240;
const uint32_t BATCH_MS = 1000;                      // one message a second per channel
const int ROWS_MAX = 10;                             // 1000 / the fastest period
const uint32_t UNREACHABLE_MS = 20000;

char g_sn[40] = "";
char t_ch[CH_COUNT][72], t_cmd[72], t_reply[72];

// The switch. Written by debug_set from any task, read by ours.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_on = false;
uint32_t g_until = 0;            // mono_ms()
uint32_t g_channels = CH_ALL;
uint32_t g_period = 100;
volatile uint32_t g_serial = 1;  // moves with every change of the state
const char *g_broker = "off";    // off, not set, connecting, connected, unreachable

// The broker: NVS "dbg", then secrets.ini's host, then broker 1's port and login.
char g_host[64] = "", g_user[40] = "", g_pass[72] = "";
uint16_t g_port = 1883;

esp_mqtt_client_handle_t g_client = nullptr;
volatile bool g_mqtt = false;
uint32_t g_started_at = 0;
// Its stack is in PSRAM (internal RAM is what Wi-Fi lives on, and there
// was 6 KB of it left, 2026-11-02), so nothing it calls may touch flash:
// no NVS, no trip log -- load() runs in the caller's task instead.
TaskHandle_t g_task = nullptr;
const int CMD_MAX = 1024;

// ---- the console's output, for the log channel ----------------------------
// stdout is one FILE every task shares. While debug is on its write
// function is wrapped, so each write is also copied here; TinyUSB's
// console reopens stdout when the port changes hands (freopen puts its own
// write function back), so the wrap is checked and put back each pass.

typedef _READ_WRITE_RETURN_TYPE (*WriteFn)(struct _reent *, void *, const char *,
                                           _READ_WRITE_BUFSIZE_TYPE);
WriteFn g_orig_write = nullptr;
portMUX_TYPE g_log_mux = portMUX_INITIALIZER_UNLOCKED;
const int LOG_RING = 8192;
char *g_log = nullptr;           // PSRAM
int g_log_len = 0;
uint32_t g_log_dropped = 0;

_READ_WRITE_RETURN_TYPE tee_write(struct _reent *r, void *cookie, const char *buf,
                                  _READ_WRITE_BUFSIZE_TYPE n) {
  if (g_on && g_log && n > 0) {
    portENTER_CRITICAL(&g_log_mux);
    const int room = LOG_RING - g_log_len;
    const int take = (int)n < room ? (int)n : room;
    memcpy(g_log + g_log_len, buf, take);
    g_log_len += take;
    if (take < (int)n) g_log_dropped += (int)n - take;
    portEXIT_CRITICAL(&g_log_mux);
  }
  return g_orig_write ? g_orig_write(r, cookie, buf, n) : (_READ_WRITE_RETURN_TYPE)n;
}

void tee(bool on) {
  FILE *f = _GLOBAL_REENT->_stdout;
  if (!f) return;
  __lock_acquire_recursive(f->_lock);
  if (on && f->_write != tee_write) {
    g_orig_write = f->_write;
    f->_write = tee_write;
  } else if (!on && f->_write == tee_write && g_orig_write) {
    f->_write = g_orig_write;
  }
  __lock_release_recursive(f->_lock);
}

// ---- the broker -------------------------------------------------------------

void load(void) {
  g_host[0] = g_user[0] = g_pass[0] = 0;
  g_port = 0;
  bool have_user = false, have_pass = false;
  nvs_handle_t h;
  if (nvs_open("dbg", NVS_READONLY, &h) == ESP_OK) {
    size_t n = sizeof(g_host);
    if (nvs_get_str(h, "host", g_host, &n) != ESP_OK) g_host[0] = 0;
    n = sizeof(g_user);
    have_user = nvs_get_str(h, "user", g_user, &n) == ESP_OK;
    n = sizeof(g_pass);
    have_pass = nvs_get_str(h, "pass", g_pass, &n) == ESP_OK;
    uint16_t p;
    if (nvs_get_u16(h, "port", &p) == ESP_OK) g_port = p;
    nvs_close(h);
  }
#ifdef DEBUG_DEFAULT_HOST
  if (!g_host[0]) snprintf(g_host, sizeof(g_host), "%s", DEBUG_DEFAULT_HOST);
#endif
  // Port and login left out are broker 1's (decided 2026-11-02: the same as client 1).
  uint16_t p1 = 1883;
  char u1[40] = "", w1[72] = "";
  uplink_login(&p1, u1, sizeof(u1), w1, sizeof(w1));
  if (!g_port) g_port = p1;
  if (!have_user) snprintf(g_user, sizeof(g_user), "%s", u1);
  if (!have_pass) snprintf(g_pass, sizeof(g_pass), "%s", w1);
}

void changed(void) { g_serial = g_serial + 1; }
bool debug_set_cmd(char *req);

void set_broker(const char *s) {
  if (g_broker != s) {
    g_broker = s;
    changed();
  }
}

void on_mqtt(void *, esp_event_base_t, int32_t id, void *data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      g_mqtt = true;
      esp_mqtt_client_subscribe(g_client, t_cmd, 1);
      break;
    case MQTT_EVENT_DISCONNECTED:
      g_mqtt = false;
      break;
    case MQTT_EVENT_DATA:
      if (e->topic_len == (int)strlen(t_cmd) && !strncmp(e->topic, t_cmd, e->topic_len) &&
          e->data_len == e->total_data_len && e->data_len > 0 && e->data_len < CMD_MAX &&
          !e->retain) {
        char *d = (char *)malloc(e->data_len + 1);
        if (d) {
          memcpy(d, e->data, e->data_len);
          d[e->data_len] = 0;
          if (!debug_set_cmd(d)) uplink_rpc_submit(d, true);   // the rest: as broker 1's rpc
        }
      }
      break;
    default:
      break;
  }
}

void start_client(void) {
  char uri[96], cid[56];
  snprintf(uri, sizeof(uri), "mqtt://%s:%u", g_host, g_port);
  snprintf(cid, sizeof(cid), "%s-dbg", g_sn);     // not the SN: broker 1 may be the same broker
  esp_mqtt_client_config_t c = {};
  c.broker.address.uri = uri;
  c.credentials.client_id = cid;
  c.credentials.username = g_user[0] ? g_user : nullptr;
  c.credentials.authentication.password = g_pass[0] ? g_pass : nullptr;
  c.buffer.size = 2048;
  c.buffer.out_size = 4096;
  c.network.reconnect_timeout_ms = 5000;
  g_client = esp_mqtt_client_init(&c);
  if (!g_client) return;
  esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY, on_mqtt, nullptr);
  if (esp_mqtt_client_start(g_client) != ESP_OK) {
    esp_mqtt_client_destroy(g_client);
    g_client = nullptr;
    return;
  }
  g_started_at = mono_ms() ? mono_ms() : 1;
  printf("[debug] to %s:%u as %s\n", g_host, g_port, cid);
}

void stop_client(void) {
  if (!g_client) return;
  esp_mqtt_client_stop(g_client);
  esp_mqtt_client_destroy(g_client);
  g_client = nullptr;
  g_mqtt = false;
  g_started_at = 0;
}

// ---- the stream -------------------------------------------------------------

// The battery current every period (the INA226 alone, one register); the
// rest of the power picture once per batch.
int16_t g_prow[ROWS_MAX];       // 0.1 mA, positive into the cell; INT16_MIN: no reading
int g_pn = 0;
uint32_t g_batch_mono = 0;      // mono_ms() of the batch's first row
int64_t g_batch_utc = 0;        // its UTC ms; 0: the clock is not to be trusted

cJSON *batch_head(void) {
  cJSON *o = cJSON_CreateObject();
  if (g_batch_utc) cJSON_AddNumberToObject(o, "from", (double)g_batch_utc);
  else cJSON_AddNullToObject(o, "from");
  cJSON_AddNumberToObject(o, "mono", g_batch_mono);
  return o;
}

void publish(int ch, cJSON *o) {
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  if (!s) return;
  if (g_client && g_mqtt) esp_mqtt_client_publish(g_client, t_ch[ch], s, 0, 0, 0);
  free(s);
}

void publish_batch(void) {
  const uint32_t ch = g_channels;
  if ((ch & CH_POWER) && g_pn) {
    PowerStatus ps = {};
    power_read(&ps);
    cJSON *o = batch_head();
    cJSON_AddNumberToObject(o, "period_ms", g_period);
    if (ps.cell_valid) {
      cJSON_AddNumberToObject(o, "mv", (int)(ps.cell_volts * 1000));
      cJSON_AddNumberToObject(o, "soc", (int)ps.soc_percent);
    }
    if (ps.charger_valid) {
      cJSON_AddStringToObject(o, "charge", charge_state_name(ps.charge));
      cJSON_AddBoolToObject(o, "pg", ps.power_good);
      cJSON_AddNumberToObject(o, "fault", ps.fault_reg);
    }
    cJSON *rows = cJSON_AddArrayToObject(o, "rows");
    for (int i = 0; i < g_pn; i++) {
      cJSON *r = cJSON_CreateObject();
      if (g_prow[i] != INT16_MIN) cJSON_AddNumberToObject(r, "ma", g_prow[i] / 10.0);
      else cJSON_AddNullToObject(r, "ma");
      cJSON_AddItemToArray(rows, r);
    }
    publish(0, o);
  }
  g_pn = 0;
  if (ch & CH_SYS) {
    cJSON *o = batch_head();
    cJSON_AddNumberToObject(o, "period_ms", BATCH_MS);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "min_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(r, "internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(r, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(r, "up_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(r, "factory", factory_mode());
    cJSON_AddItemToArray(cJSON_AddArrayToObject(o, "rows"), r);
    publish(1, o);
  }
  if (ch & CH_NET) {
    NetStatus ns;
    net_status(&ns);
    cJSON *o = batch_head();
    cJSON_AddNumberToObject(o, "period_ms", BATCH_MS);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "wifi", ns.connected);
    if (ns.connected) cJSON_AddNumberToObject(r, "rssi", ns.rssi);
    cJSON_AddBoolToObject(r, "broker1", uplink_connected());
    cJSON_AddItemToArray(cJSON_AddArrayToObject(o, "rows"), r);
    publish(2, o);
  }
  if ((ch & CH_LOG) && g_log) {
    // Whole lines only; a partial one waits for the rest.
    static EXT_RAM_BSS_ATTR char buf[LOG_RING + 1];
    int n = 0;
    uint32_t dropped = 0;
    portENTER_CRITICAL(&g_log_mux);
    int end = g_log_len;
    while (end > 0 && g_log[end - 1] != '\n') end--;
    if (!end && g_log_len == LOG_RING) end = LOG_RING;      // one line fills it: send it anyway
    memcpy(buf, g_log, end);
    memmove(g_log, g_log + end, g_log_len - end);
    g_log_len -= end;
    n = end;
    dropped = g_log_dropped;
    g_log_dropped = 0;
    portEXIT_CRITICAL(&g_log_mux);
    if (n || dropped) {
      buf[n] = 0;
      cJSON *o = cJSON_CreateObject();
      cJSON *lines = cJSON_AddArrayToObject(o, "lines");
      char *save = nullptr;
      for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(nullptr, "\n", &save)) {
        const size_t k = strlen(l);
        if (k && l[k - 1] == '\r') l[k - 1] = 0;
        if (*l) cJSON_AddItemToArray(lines, cJSON_CreateString(l));
      }
      if (dropped) cJSON_AddNumberToObject(o, "dropped", dropped);
      publish(3, o);
    }
  }
}

void sample(void) {
  if (!(g_channels & CH_POWER) || g_pn >= ROWS_MAX) return;
  if (!g_pn) {
    g_batch_mono = mono_ms();
    TimeStamp t;
    time_now(&t);
    g_batch_utc = t.quality != TimeSource::None ? t.utc_ms : 0;
  }
  float ma = 0;
  g_prow[g_pn++] = power_battery_ma(&ma) ? (int16_t)(ma * 10) : INT16_MIN;
}

// ---- commands on the debug broker ----------------------------------------------

uint32_t parse_channels(const cJSON *a) {
  if (!cJSON_IsArray(a)) return 0;
  uint32_t m = 0;
  const cJSON *it;
  cJSON_ArrayForEach(it, a) {
    for (int i = 0; i < CH_COUNT; i++) {
      if (cJSON_IsString(it) && !strcmp(it->valuestring, CH_NAMES[i])) m |= 1u << i;
    }
  }
  return m;
}

uint32_t num(const cJSON *o, const char *k, uint32_t dflt) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsNumber(v) && v->valuedouble >= 0 ? (uint32_t)v->valuedouble : dflt;
}

// DEBUG_SET: channels and period for this session (on/off is broker 1's).
// True when it was one, and then `req` is freed here.
bool debug_set_cmd(char *req) {
  cJSON *j = cJSON_Parse(req);
  const cJSON *cmd = j ? cJSON_GetObjectItemCaseSensitive(j, "cmd") : nullptr;
  if (!cJSON_IsString(cmd) || strcmp(cmd->valuestring, "DEBUG_SET")) {
    cJSON_Delete(j);
    return false;
  }
  portENTER_CRITICAL(&g_mux);
  const uint32_t m = parse_channels(cJSON_GetObjectItemCaseSensitive(j, "channels"));
  if (m) g_channels = m;
  const uint32_t p = num(j, "period_ms", g_period);
  g_period = p < 100 ? 100 : p > 1000 ? 1000 : p;
  portEXIT_CRITICAL(&g_mux);
  changed();
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "id", num(j, "id", 0));
  cJSON_AddTrueToObject(o, "ok");
  char st[256];
  debug_state_json(st, sizeof(st));
  cJSON_AddItemToObject(o, "state", cJSON_Parse(st));
  char *resp = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  cJSON_Delete(j);
  free(req);
  debug_reply(resp);
  free(resp);
  return true;
}

void task(void *) {
  uint32_t batch_at = 0;
  bool held = false;
  for (;;) {
    if (!g_on) {
      if (g_client) {
        stop_client();
        tee(false);
        printf("[debug] off\n");
      }
      if (held) {
        pm_hold(Hold::Debug, false);
        held = false;
      }
      set_broker("off");
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    const uint32_t now = mono_ms();
    if ((int32_t)(now - g_until) >= 0) {
      printf("[debug] its time is up\n");
      debug_set(false, 0, 0, 0);
      continue;
    }
    if (!pm_external_power()) {
      // On battery the box would sleep and the radio go off: keep both up.
      if (!held) {
        pm_hold(Hold::Debug, true);
        held = true;
      }
      net_want(true);
    }
    tee(true);
    if (!g_host[0]) {
      set_broker("not set");
    } else {
      NetStatus ns;
      net_status(&ns);
      if (!g_client && ns.connected) start_client();
      set_broker(g_mqtt ? "connected"
                 : g_started_at && now - g_started_at > UNREACHABLE_MS ? "unreachable"
                                                                       : "connecting");
    }
    sample();
    if (!batch_at || now - batch_at >= BATCH_MS) {
      if (g_mqtt) publish_batch();
      else g_pn = 0;
      batch_at = now;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g_period));
  }
}

}  // namespace

void debug_start(const char *sn) {
  snprintf(g_sn, sizeof(g_sn), "%s", sn ? sn : "MCOLD");
  for (int i = 0; i < CH_COUNT; i++) snprintf(t_ch[i], sizeof(t_ch[i]), "mcold/%s/debug/%s", g_sn, CH_NAMES[i]);
  snprintf(t_cmd, sizeof(t_cmd), "mcold/v1/%s/debug/cmd", g_sn);
  snprintf(t_reply, sizeof(t_reply), "mcold/%s/debug/reply", g_sn);
  g_log = (char *)heap_caps_malloc(LOG_RING, MALLOC_CAP_SPIRAM);
  load();
  factory_mode();     // read from NVS here, once: the task may not (its stack is in PSRAM)
}

void debug_set(bool on, uint32_t minutes, uint32_t channels, uint32_t period_ms) {
  const bool usb = pm_external_power();
  if (!minutes) minutes = usb ? MIN_USB : MIN_BATTERY;
  const uint32_t most = usb ? MAX_USB_MIN : MIN_BATTERY;
  if (minutes > most) minutes = most;
  portENTER_CRITICAL(&g_mux);
  if (on) {
    g_until = mono_ms() + minutes * 60000;
    if (channels) g_channels = channels & CH_ALL;
    if (period_ms) g_period = period_ms < 100 ? 100 : period_ms > 1000 ? 1000 : period_ms;
  }
  g_on = on;
  portEXIT_CRITICAL(&g_mux);
  if (on) {
    load();                      // a `debug server set` since boot counts
    if (!g_task) {
      // Made at the first "on", never at boot: most boxes are never debugged.
      xTaskCreatePinnedToCoreWithCaps(task, "debug", 6144, nullptr, 2, &g_task, 0, MALLOC_CAP_SPIRAM);
    }
    printf("[debug] on for %lu min\n", (unsigned long)minutes);
  }
  changed();
  uplink_kick();                 // the new state to broker 1 at once
  if (g_task) xTaskNotifyGive(g_task);
}

bool debug_on(void) { return g_on; }

void debug_ctl(const char *json, int len, bool retained) {
  if (retained) {
    // Left on the broker, it would switch debug on at every reconnect.
    printf("[debug] a retained debug/ctl is ignored\n");
    return;
  }
  cJSON *j = cJSON_ParseWithLength(json, len);
  if (!j) return;
  const cJSON *on = cJSON_GetObjectItemCaseSensitive(j, "on");
  if (cJSON_IsBool(on)) {
    debug_set(cJSON_IsTrue(on), num(j, "min", 0),
              parse_channels(cJSON_GetObjectItemCaseSensitive(j, "channels")),
              num(j, "period_ms", 0));
  }
  cJSON_Delete(j);
}

uint32_t debug_state_serial(void) { return g_serial; }

void debug_state_json(char *out, size_t n) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "on", g_on);
  if (g_on) {
    const int32_t left = (int32_t)(g_until - mono_ms());
    TimeStamp t;
    time_now(&t);
    if (t.quality != TimeSource::None) cJSON_AddNumberToObject(o, "until", (double)((t.utc_ms + left) / 1000));
    else cJSON_AddNullToObject(o, "until");
    cJSON_AddNumberToObject(o, "min_left", left > 0 ? (left + 59999) / 60000 : 0);
    cJSON *a = cJSON_AddArrayToObject(o, "channels");
    for (int i = 0; i < CH_COUNT; i++) {
      if (g_channels & (1u << i)) cJSON_AddItemToArray(a, cJSON_CreateString(CH_NAMES[i]));
    }
    cJSON_AddNumberToObject(o, "period_ms", g_period);
  }
  cJSON_AddStringToObject(o, "broker", g_broker);
  if (!cJSON_PrintPreallocated(o, out, (int)n, false) && n) out[0] = 0;
  cJSON_Delete(o);
}

bool debug_set_server(const char *host, uint16_t port, const char *user, const char *pass) {
  if (!host || !*host || strlen(host) >= sizeof(g_host) || !port ||
      (user && strlen(user) >= sizeof(g_user)) || (pass && strlen(pass) >= sizeof(g_pass))) {
    return false;
  }
  nvs_handle_t h;
  if (nvs_open("dbg", NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = nvs_set_str(h, "host", host) == ESP_OK && nvs_set_u16(h, "port", port) == ESP_OK;
  if (user && *user) ok = ok && nvs_set_str(h, "user", user) == ESP_OK;
  else nvs_erase_key(h, "user");
  if (pass && *pass) ok = ok && nvs_set_str(h, "pass", pass) == ESP_OK;
  else nvs_erase_key(h, "pass");
  ok = ok && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (ok && g_on) {
    stop_client();               // to the new one at the next pass
    load();
  }
  return ok;
}

bool debug_clear_server(void) {
  nvs_handle_t h;
  if (nvs_open("dbg", NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  load();
  return ok;
}

void debug_print(void) {
  char st[256];
  debug_state_json(st, sizeof(st));
  load();
  printf("  debug broker %s:%u%s\n  state  %s\n", g_host[0] ? g_host : "(not set)", g_port,
         g_user[0] ? "" : ", no login", st);
}

void debug_reply(const char *resp) {
  if (resp && g_client && g_mqtt) esp_mqtt_client_publish(g_client, t_reply, resp, 0, 1, 0);
}
