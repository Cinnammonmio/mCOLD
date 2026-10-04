#include "uplink.h"

#include <cJSON.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mqtt_client.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "flashlog.h"
#include "net.h"
#include "ota.h"
#include "pm.h"
#include "config.h"
#include "timekeep.h"
#include "rpc.h"
#include "trip.h"

namespace {

char g_sn[SN_LEN] = "";
char g_host[64] = "";
uint16_t g_port = 1883;
char g_user[40] = "";
char g_pass[72] = "";
// Every topic is mcold/v1/<sn>/...: the product, then the version of this
// topic layout, so a later layout can run beside it on the same broker.
#define TOPIC_ROOT "mcold/v1/"
char t_rec[64], t_ack[64], t_status[64], t_online[64], t_fw[64], t_ota[64];

esp_mqtt_client_handle_t g_client = nullptr;
volatile bool g_mqtt = false;
volatile bool g_started = false;
TaskHandle_t g_task = nullptr;
SemaphoreHandle_t g_mx = nullptr;

// The batch waiting for its ACK. One at a time: simple, and the server's
// answer to one batch is what decides where the next one starts.
struct Flight {
  bool on;
  uint32_t trip, from, to, sent_at;
};
Flight g_flight = {};
bool g_last_was_live = false;

uint32_t g_batches = 0, g_acks = 0, g_rejected = 0;
uint32_t g_ack_wait = UPLINK_ACK_TIMEOUT_MS;   // grows while the server is silent

// Battery sessions, planned through deep sleep.
struct Plan {
  uint32_t magic;
  uint32_t next_at;         // mono_ms(); 0: due now
  uint8_t misses;           // sessions in a row that never reached the broker
  bool last_ok;
  uint32_t sessions;
  uint32_t last_ack;        // mono_ms() of the last ACK accepted
  uint32_t checkin_at;      // mono_ms() a session is due with nothing to send; 0: now
};
const uint32_t PLAN_MAGIC = 0x55504C32;   // "UPL2"
RTC_DATA_ATTR Plan g_plan;
#define g_last_ack g_plan.last_ack

const uint32_t SESSION_MAX_MS = 45000;    // join, broker, a few batches
const uint32_t SESSION_ACK_MS = 10000;    // an ACK later than this waits for the next
const uint32_t SESSION_BACKOFF_MAX_MS = 4 * 3600000;
// After the broker answers, long enough for a retained firmware message
// to arrive before a session with nothing to send ends.
const uint32_t SESSION_SETTLE_MS = 1500;

// Runs through deep sleep (timekeep.h), so times kept across one compare.
uint32_t now_ms(void) { return mono_ms(); }

// ---- the high-water marks, one NVS key per trip ------------------------

void ack_key(uint32_t trip, char *k) { snprintf(k, 12, "t%08lx", (unsigned long)trip); }

uint32_t ack_get(uint32_t trip) {
  nvs_handle_t h;
  uint32_t v = 0;
  char k[12];
  ack_key(trip, k);
  if (nvs_open("ack", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, k, &v);
    nvs_close(h);
  }
  return v;
}

void ack_put(uint32_t trip, uint32_t v) {
  nvs_handle_t h;
  char k[12];
  ack_key(trip, k);
  if (nvs_open("ack", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, k, v);
    nvs_commit(h);
    nvs_close(h);
  }
}

// ---- server and login, from NVS ------------------------------------------

void load(void) {
  nvs_handle_t h;
  g_host[0] = g_user[0] = g_pass[0] = 0;
  g_port = 1883;
  if (nvs_open("mqtt", NVS_READONLY, &h) != ESP_OK) return;
  size_t n = sizeof(g_host);
  if (nvs_get_str(h, "host", g_host, &n) != ESP_OK) g_host[0] = 0;
  n = sizeof(g_user);
  if (nvs_get_str(h, "user", g_user, &n) != ESP_OK) g_user[0] = 0;
  n = sizeof(g_pass);
  if (nvs_get_str(h, "pass", g_pass, &n) != ESP_OK) g_pass[0] = 0;
  uint16_t p;
  if (nvs_get_u16(h, "port", &p) == ESP_OK) g_port = p;
  nvs_close(h);
}

// ---- incoming ACKs ---------------------------------------------------------

void on_ack(const char *data, int len) {
  cJSON *j = cJSON_ParseWithLength(data, len);
  const cJSON *jt = j ? cJSON_GetObjectItemCaseSensitive(j, "trip") : nullptr;
  const cJSON *ju = j ? cJSON_GetObjectItemCaseSensitive(j, "upto") : nullptr;
  uint32_t last;
  if (!cJSON_IsNumber(jt) || !cJSON_IsNumber(ju) || jt->valuedouble < 1 ||
      ju->valuedouble < 0 ||
      !flashlog_last_seq((uint32_t)jt->valuedouble, &last) ||
      ju->valuedouble > last) {
    // Not an ACK for anything in this log: a malformed message, someone
    // else's trip, or records never sent. It marks nothing stored.
    g_rejected++;
    printf("[uplink] ACK rejected: %.*s\n", len > 80 ? 80 : len, data);
    cJSON_Delete(j);
    return;
  }
  const uint32_t trip = (uint32_t)jt->valuedouble;
  const uint32_t upto = (uint32_t)ju->valuedouble;
  cJSON_Delete(j);

  xSemaphoreTake(g_mx, portMAX_DELAY);
  if (upto + 1 > ack_get(trip)) ack_put(trip, upto + 1);   // only ever forward
  if (g_flight.on && g_flight.trip == trip && upto >= g_flight.to) g_flight.on = false;
  g_ack_wait = UPLINK_ACK_TIMEOUT_MS;    // the server is answering again
  g_acks++;
  g_last_ack = now_ms();
  xSemaphoreGive(g_mx);
  if (g_task) xTaskNotifyGive(g_task);
}

void on_mqtt(void *, esp_event_base_t, int32_t id, void *data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      g_mqtt = true;
      esp_mqtt_client_subscribe(g_client, t_ack, 1);
      esp_mqtt_client_subscribe(g_client, t_fw, 1);
      esp_mqtt_client_publish(g_client, t_online, "1", 1, 1, 1);
      printf("[uplink] connected to %s:%u\n", g_host, g_port);
      ota_on_connected();       // what a new image proves itself by
      if (g_task) xTaskNotifyGive(g_task);
      break;
    case MQTT_EVENT_DISCONNECTED:
      if (g_mqtt) printf("[uplink] disconnected from the broker\n");
      g_mqtt = false;
      xSemaphoreTake(g_mx, portMAX_DELAY);
      g_flight.on = false;            // resent after reconnecting
      xSemaphoreGive(g_mx);
      break;
    case MQTT_EVENT_DATA:
      if (e->topic_len == (int)strlen(t_ack) && !strncmp(e->topic, t_ack, e->topic_len) &&
          e->data_len == e->total_data_len) {
        on_ack(e->data, e->data_len);
      } else if (e->topic_len == (int)strlen(t_fw) && !strncmp(e->topic, t_fw, e->topic_len) &&
                 e->data_len == e->total_data_len && e->data_len < 160) {
        // Just a file name (or a URL); everything else is the OTA module's.
        char name[160];
        memcpy(name, e->data, e->data_len);
        name[e->data_len] = 0;
        if (e->data_len) printf("[uplink] firmware named by the server: %s\n", name);
        ota_request(name, true, false);
      }
      break;
    default:
      break;
  }
}

void start_client(void) {
  char uri[96];
  snprintf(uri, sizeof(uri), "mqtt://%s:%u", g_host, g_port);
  esp_mqtt_client_config_t c = {};
  c.broker.address.uri = uri;
  c.credentials.client_id = g_sn;
  c.credentials.username = g_user[0] ? g_user : nullptr;
  c.credentials.authentication.password = g_pass[0] ? g_pass : nullptr;
  // If the box drops off without a word, the broker says so for it.
  c.session.last_will.topic = t_online;
  c.session.last_will.msg = "0";
  c.session.last_will.msg_len = 1;
  c.session.last_will.qos = 1;
  c.session.last_will.retain = 1;
  c.buffer.size = 1024;
  c.buffer.out_size = 4096;           // a full batch
  g_client = esp_mqtt_client_init(&c);
  if (!g_client) return;
  esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY, on_mqtt, nullptr);
  if (esp_mqtt_client_start(g_client) == ESP_OK) g_started = true;
}

// ---- outgoing batches -------------------------------------------------------

struct Batch {
  cJSON *arr;
  int n;
  uint32_t last;
};

bool collect(const LogRecord &r, void *ctx) {
  Batch *b = (Batch *)ctx;
  unsigned char b64[160];
  size_t olen = 0;
  mbedtls_base64_encode(b64, sizeof(b64), &olen, r.payload, r.len);
  b64[olen] = 0;
  cJSON *e = cJSON_CreateObject();
  cJSON_AddNumberToObject(e, "seq", r.seq);
  cJSON_AddNumberToObject(e, "type", r.type);
  cJSON_AddStringToObject(e, "data", (const char *)b64);
  cJSON_AddItemToArray(b->arr, e);
  b->last = r.seq;
  return ++b->n < UPLINK_BATCH;
}

// The next trip with records the server has not confirmed. Oldest first,
// but the running trip gets every other batch, so live samples are not
// stuck behind a month of backlog (§9.5).
bool pick(uint32_t *trip, uint32_t *from) {
  TripStatus s;
  trip_status(&s);
  uint32_t trips[64];
  const int n = flashlog_trips(trips, 64);
  uint32_t last;
  if (s.active && !g_last_was_live && flashlog_last_seq(s.id, &last) &&
      last >= ack_get(s.id)) {
    *trip = s.id;
    *from = ack_get(s.id);
    g_last_was_live = true;
    return true;
  }
  for (int i = 0; i < n && i < 64; i++) {
    if (trips[i] > TRIP_ID_REAL_MAX) continue;      // bench records stay home
    const uint32_t a = ack_get(trips[i]);
    if (flashlog_last_seq(trips[i], &last) && last >= a) {
      *trip = trips[i];
      *from = a;
      g_last_was_live = s.active && trips[i] == s.id;
      return true;
    }
  }
  return false;
}

void send_batch(void) {
  uint32_t trip, from;
  if (!pick(&trip, &from)) return;
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "sn", g_sn);
  cJSON_AddNumberToObject(o, "trip", trip);
  cJSON_AddNumberToObject(o, "schema", 1);
  Batch b = {cJSON_AddArrayToObject(o, "records"), 0, from};
  flashlog_read(trip, from, collect, &b, nullptr);
  if (!b.n) {
    cJSON_Delete(o);
    return;
  }
  cJSON_AddNumberToObject(o, "from", from);
  cJSON_AddNumberToObject(o, "to", b.last);
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  if (!s) return;
  const int id = esp_mqtt_client_publish(g_client, t_rec, s, 0, 1, 0);
  free(s);
  if (id < 0) return;
  xSemaphoreTake(g_mx, portMAX_DELAY);
  g_flight = {true, trip, from, b.last, now_ms()};
  g_batches++;
  xSemaphoreGive(g_mx);
}

void publish_status(void) {
  char *st = rpc_handle("{\"id\":1,\"cmd\":\"GET_STATUS\"}", 27, nullptr);
  esp_mqtt_client_publish(g_client, t_status, st, 0, 0, 1);
  free(st);
}

// The OTA module's latest word, once each, retained.
uint32_t g_ota_sent = 0;
void publish_ota(void) {
  char s[200];
  uint32_t serial;
  if (!ota_outbox(s, sizeof(s), &serial) || serial == g_ota_sent) return;
  if (esp_mqtt_client_publish(g_client, t_ota, s, 0, 1, 1) < 0) return;
  g_ota_sent = serial;
  ota_outbox_sent(serial);
}

void stop_client(void) {
  if (!g_client) return;
  esp_mqtt_client_stop(g_client);
  esp_mqtt_client_destroy(g_client);
  g_client = nullptr;
  g_mqtt = false;
  g_started = false;
  xSemaphoreTake(g_mx, portMAX_DELAY);
  g_flight.on = false;
  xSemaphoreGive(g_mx);
}

uint32_t pending(void) {
  UplinkStatus us;
  uplink_status(&us);
  return us.records_pending;
}

// ---- on battery: one session, then the radio off -----------------------

struct Session {
  bool on;
  uint32_t at;
  bool reached;             // the broker answered
  bool status_sent;
  uint32_t acks_before;     // g_acks when it began
  uint32_t mqtt_at;         // when the broker answered; 0: not yet
};
Session g_ses = {};

void end_session(bool all_sent) {
  if (g_mqtt) {
    // Said in words, so the server need not wait for the last will: the
    // box is asleep, not lost.
    esp_mqtt_client_publish(g_client, t_online, "0", 1, 1, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
  }
  stop_client();
  net_want(false);
  const uint32_t t = now_ms();
  g_plan.sessions++;
  // A session that got the server to confirm something -- or had nothing
  // left to confirm -- is a success. One that reached the broker and
  // heard nothing back is not: a server that is not answering costs a
  // whole session of radio every period, so it backs off like a network
  // that is not there. The records are safe in flash either way.
  const bool ok = g_ses.reached && (all_sent || g_acks != g_ses.acks_before);
  g_plan.last_ok = g_ses.reached;
  g_plan.misses = ok ? 0 : (uint8_t)(g_plan.misses < 8 ? g_plan.misses + 1 : 8);
  uint64_t wait = (uint64_t)config().upload_period_s * 1000;
  if (g_plan.misses) wait <<= g_plan.misses;
  if (wait > SESSION_BACKOFF_MAX_MS) wait = SESSION_BACKOFF_MAX_MS;
  g_plan.next_at = t + (uint32_t)wait;
  if (!g_plan.next_at) g_plan.next_at = 1;
  g_plan.checkin_at = t + UPLINK_CHECKIN_MS;
  if (!g_plan.checkin_at) g_plan.checkin_at = 1;
  printf("[uplink] session over after %lu ms: %s; next in %lu s\n",
         (unsigned long)(t - g_ses.at),
         !g_ses.reached ? "broker not reached" : all_sent ? "everything acknowledged"
                                                          : "server has not acknowledged",
         (unsigned long)(wait / 1000));
  g_ses = {};
  pm_hold(Hold::Uplink, false);
  pm_no_light_sleep(false);
}

void battery_pass(void) {
  const uint32_t t = now_ms();
  if (!g_ses.on) {
    // After this wake's sample, so the record just written goes now and
    // not one period later. Ten seconds at most: a trip task that never
    // reports must not stop the upload.
    if (!pm_is_done(Duty::Trip) && esp_timer_get_time() < 10000000) return;
    const bool can = g_host[0] && net_configured();
    const uint32_t n = can ? pending() : 0;
    // Records waiting, on their schedule (which backs off); a check-in now
    // and then with nothing to send, so a status and a firmware message
    // still reach a box that is not on a trip; and at once for a new image
    // that has to reach the broker to be kept.
    const bool records = n && (!g_plan.next_at || (int32_t)(t - g_plan.next_at) >= 0);
    const bool checkin = can && (!g_plan.checkin_at || (int32_t)(t - g_plan.checkin_at) >= 0);
    const bool urgent = can && ota_needs_broker();
    if (!records && !checkin && !urgent) {
      stop_client();          // left over from USB power
      net_want(false);
      uint32_t at = n ? g_plan.next_at : 0;
      if (can && (!at || (int32_t)(g_plan.checkin_at - at) < 0)) at = g_plan.checkin_at;
      pm_next(Duty::Uplink, at);
      pm_done(Duty::Uplink);
      return;
    }
    g_ses = {true, t, false, false, g_acks, 0};
    pm_hold(Hold::Uplink, true);
    pm_no_light_sleep(true);    // Wi-Fi and the broker, without naps in between
    net_want(true);
    printf("[uplink] session: %lu records waiting%s\n", (unsigned long)n,
           urgent ? ", a new image to confirm" : (!records ? " (check-in)" : ""));
  }

  NetStatus ns;
  net_status(&ns);
  if (!g_started && ns.connected) start_client();
  if (g_mqtt) {
    g_ses.reached = true;
    if (!g_ses.mqtt_at) g_ses.mqtt_at = t ? t : 1;
    if (!g_ses.status_sent) {
      publish_status();
      g_ses.status_sent = true;
    }
    publish_ota();
    // A download needs the radio; the OTA module has its own time limits.
    if (ota_busy()) return;
    xSemaphoreTake(g_mx, portMAX_DELAY);
    const Flight f = g_flight;
    xSemaphoreGive(g_mx);
    if (f.on && t - f.sent_at >= SESSION_ACK_MS) {
      end_session(false);
      return;
    }
    if (!f.on) {
      if (!pending()) {
        if (t - g_ses.mqtt_at < SESSION_SETTLE_MS) return;
        end_session(true);
        return;
      }
      send_batch();
    }
  }
  if (ota_busy()) return;
  if (t - g_ses.at >= SESSION_MAX_MS) end_session(false);
}

void task(void *) {
  TripStatus prev = {};
  uint32_t status_at = 0;
  for (;;) {
    // Quick while this wake is still deciding: the chip waits on it.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g_ses.on || !pm_is_done(Duty::Uplink) ? 100 : 1000));
    if (!pm_external_power()) {
      battery_pass();
      continue;
    }
    // On USB power: always connected; nothing here keeps the chip up.
    if (g_ses.on) {
      g_ses = {};
      pm_hold(Hold::Uplink, false);
      pm_no_light_sleep(false);
    }
    net_want(true);
    pm_next(Duty::Uplink, 0);
    pm_done(Duty::Uplink);
    NetStatus ns;
    net_status(&ns);
    if (!g_started && g_host[0] && ns.connected) start_client();
    if (!g_mqtt) continue;
    publish_ota();

    // Status on change, and every five minutes regardless.
    TripStatus s;
    trip_status(&s);
    if (!status_at || now_ms() - status_at > 300000 || s.active != prev.active ||
        s.alarms_active != prev.alarms_active || s.samples != prev.samples) {
      publish_status();
      status_at = now_ms();
      prev = s;
    }

    xSemaphoreTake(g_mx, portMAX_DELAY);
    const bool waiting = g_flight.on && now_ms() - g_flight.sent_at < g_ack_wait;
    if (g_flight.on && !waiting) {
      // No answer. Send it again, but wait twice as long next time, up to
      // five minutes: a server that is not answering at all should not
      // cost the battery a batch every fifteen seconds.
      if (g_ack_wait < UPLINK_ACK_WAIT_MAX_MS) g_ack_wait *= 2;
      printf("[uplink] no ACK for trip %lu %lu..%lu; again, then wait %lu s\n",
             (unsigned long)g_flight.trip, (unsigned long)g_flight.from,
             (unsigned long)g_flight.to, (unsigned long)(g_ack_wait / 1000));
      g_flight.on = false;
    }
    xSemaphoreGive(g_mx);
    if (!waiting) send_batch();
  }
}

}  // namespace

void uplink_start(const char *sn) {
  snprintf(g_sn, sizeof(g_sn), "%s", sn ? sn : "MCOLD");
  snprintf(t_rec, sizeof(t_rec), TOPIC_ROOT "%s/rec", g_sn);
  snprintf(t_ack, sizeof(t_ack), TOPIC_ROOT "%s/ack", g_sn);
  snprintf(t_status, sizeof(t_status), TOPIC_ROOT "%s/status", g_sn);
  snprintf(t_online, sizeof(t_online), TOPIC_ROOT "%s/online", g_sn);
  snprintf(t_fw, sizeof(t_fw), TOPIC_ROOT "%s/firmware", g_sn);
  snprintf(t_ota, sizeof(t_ota), TOPIC_ROOT "%s/ota/state", g_sn);
  g_mx = xSemaphoreCreateMutex();
  load();
  if (!pm_warm() || g_plan.magic != PLAN_MAGIC) g_plan = {PLAN_MAGIC, 0, 0, false, 0, 0, 0};
  xTaskCreatePinnedToCore(task, "uplink", 6144, nullptr, 2, &g_task, 0);
}

bool uplink_set_server(const char *host, uint16_t port, const char *user,
                       const char *pass) {
  if (!host || !*host || strlen(host) >= sizeof(g_host) || !port ||
      (user && strlen(user) >= sizeof(g_user)) || (pass && strlen(pass) >= sizeof(g_pass))) {
    return false;
  }
  nvs_handle_t h;
  if (nvs_open("mqtt", NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_str(h, "host", host) == ESP_OK &&
                  nvs_set_u16(h, "port", port) == ESP_OK &&
                  nvs_set_str(h, "user", user ? user : "") == ESP_OK &&
                  nvs_set_str(h, "pass", pass ? pass : "") == ESP_OK &&
                  nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (!ok) return false;
  if (g_client) {               // reconnect with the new settings
    esp_mqtt_client_stop(g_client);
    esp_mqtt_client_destroy(g_client);
    g_client = nullptr;
    g_mqtt = false;
    g_started = false;
  }
  load();
  if (g_task) xTaskNotifyGive(g_task);
  return true;
}

bool uplink_configured(void) { return g_host[0] != 0; }

uint32_t uplink_acked(uint32_t trip) { return ack_get(trip); }

bool uplink_fully_acked(uint32_t trip) {
  uint32_t last;
  if (!flashlog_last_seq(trip, &last)) return true;   // nothing left to send
  return ack_get(trip) > last;
}

void uplink_forget(uint32_t trip) {
  nvs_handle_t h;
  char k[12];
  ack_key(trip, k);
  if (nvs_open("ack", NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, k);
    nvs_commit(h);
    nvs_close(h);
  }
}

void uplink_kick(void) {
  if (g_task) xTaskNotifyGive(g_task);
}

void uplink_status(UplinkStatus *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->configured = g_host[0] != 0;
  out->connected = g_mqtt;
  snprintf(out->host, sizeof(out->host), "%s", g_host);
  out->port = g_port;
  out->batches_sent = g_batches;
  out->acks = g_acks;
  out->acks_rejected = g_rejected;
  out->last_ack_ms = g_last_ack;
  out->sessions = g_plan.sessions;
  out->last_session_ok = g_plan.last_ok;
  out->next_session_ms = g_plan.next_at;
  uint32_t trips[64];
  const int n = flashlog_trips(trips, 64);
  for (int i = 0; i < n && i < 64; i++) {
    uint32_t last;
    if (trips[i] > TRIP_ID_REAL_MAX || !flashlog_last_seq(trips[i], &last)) continue;
    const uint32_t a = ack_get(trips[i]);
    if (last + 1 > a) out->records_pending += last + 1 - a;
  }
}

void uplink_inject_ack(const char *json) { on_ack(json, (int)strlen(json)); }
