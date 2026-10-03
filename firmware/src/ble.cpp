#include "ble.h"

#include <driver/gpio.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
void ble_store_config_init(void);
}

#include "auth.h"
#include "board.h"
#include "pm.h"
#include "record.h"
#include "rpc.h"
#include "trip.h"

namespace {

// ---- UUIDs: d2a5xxxx-3818-4667-a205-b3ed9c37b8a5 (PROTOCOL.md) --------

ble_uuid128_t uuid(uint16_t n) {
  // The string's bytes in reverse, as BLE carries 128-bit UUIDs.
  static const uint8_t BASE[16] = {0xa5, 0xb8, 0x37, 0x9c, 0xed, 0xb3, 0x05, 0xa2,
                                   0x67, 0x46, 0x18, 0x38, 0x00, 0x00, 0xa5, 0xd2};
  ble_uuid128_t u;
  u.u.type = BLE_UUID_TYPE_128;
  memcpy(u.value, BASE, 16);
  u.value[12] = (uint8_t)n;
  u.value[13] = (uint8_t)(n >> 8);
  return u;
}

ble_uuid128_t U_SVC, U_INFO, U_STATUS, U_CMD, U_RSP, U_EVT;
enum Chr : uintptr_t { C_INFO = 1, C_STATUS, C_CMD, C_RSP, C_EVT };

ble_gatt_chr_def g_chrs[6];
ble_gatt_svc_def g_svcs[2];
uint16_t h_status, h_rsp, h_evt;

// ---- state -------------------------------------------------------------

char g_sn[16] = "";
uint8_t g_own_addr = 0;
volatile bool g_synced = false;
bool g_up = false;              // the NimBLE stack has been started
volatile bool g_enabled = true;
volatile bool g_adv = false;
volatile uint16_t g_conn = BLE_HS_CONN_HANDLE_NONE;
volatile uint16_t g_mtu = 23;
volatile uint32_t g_window_until = 0;
volatile uint32_t g_requests = 0;
volatile bool g_sub_status = false, g_sub_rsp = false, g_sub_evt = false;

// The one connection's protocol session: its AUTH nonce and whether it
// has proved it read the key from the NFC tag.
RpcSession g_session = {};

// Reassembly of one request from COMMAND writes.
const size_t MSG_MAX = 4096;
uint8_t g_rx[MSG_MAX];
size_t g_rx_len = 0;
int g_rx_next = -1;          // index of the fragment expected; -1: idle

struct Msg {
  uint16_t conn;
  char *data;
  size_t n;
};
QueueHandle_t g_q = nullptr;

// STATUS is read in pieces when it is longer than the MTU; every piece
// must come from the same JSON, so one copy is kept for a moment.
char *g_status_cache = nullptr;
uint32_t g_status_at = 0;

const uint8_t F_FIRST = 0x80, F_LAST = 0x40, F_INDEX = 0x3F;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool external_power(void) { return pm_external_power(); }

// ---- sending: every notification is a framed message --------------------

void send_framed(uint16_t conn, uint16_t attr, const char *s, size_t n) {
  if (conn == BLE_HS_CONN_HANDLE_NONE) return;
  const size_t room = (g_mtu > 5 ? g_mtu : 23) - 3 - 1;
  uint8_t buf[256];
  size_t off = 0;
  int idx = 0;
  do {
    const size_t k = (n - off) < room ? (n - off) : (room < sizeof(buf) - 1 ? room : sizeof(buf) - 1);
    buf[0] = (uint8_t)((off == 0 ? F_FIRST : 0) | (off + k >= n ? F_LAST : 0) |
                       (idx & F_INDEX));
    memcpy(buf + 1, s + off, k);
    // The stack can run out of buffers when a long answer goes out in a
    // burst; it gets them back as the phone acknowledges. Wait and retry
    // rather than drop a fragment, which would lose the whole message.
    int rc = -1;
    for (int tries = 0; tries < 100 && rc != 0; tries++) {
      os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)(k + 1));
      rc = om ? ble_gatts_notify_custom(conn, attr, om) : BLE_HS_ENOMEM;
      if (rc != 0) vTaskDelay(pdMS_TO_TICKS(10));
      if (g_conn != conn) return;
    }
    if (rc != 0) return;
    off += k;
    idx++;
  } while (off < n);
}

void send_json(uint16_t attr, bool subscribed, const char *json) {
  if (subscribed) send_framed(g_conn, attr, json, strlen(json));
}

// ---- GATT access --------------------------------------------------------

int on_access(uint16_t conn, uint16_t, ble_gatt_access_ctxt *ctxt, void *arg) {
  const uintptr_t which = (uintptr_t)arg;

  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    char *s = nullptr;
    if (which == C_INFO) {
      // With this connection's nonce in it: the AUTH challenge.
      s = rpc_handle("{\"id\":1,\"cmd\":\"GET_INFO\"}", 25, &g_session);
    } else if (which == C_STATUS) {
      if (!g_status_cache || now_ms() - g_status_at > 1000) {
        free(g_status_cache);
        g_status_cache = rpc_handle("{\"id\":1,\"cmd\":\"GET_STATUS\"}", 27, nullptr);
        g_status_at = now_ms();
      }
      s = strdup(g_status_cache);
    } else {
      return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    const int rc = os_mbuf_append(ctxt->om, s, strlen(s));
    free(s);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }

  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == C_CMD) {
    uint8_t buf[260];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0 || len < 1) {
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    const uint8_t flags = buf[0];
    const int idx = flags & F_INDEX;
    if (flags & F_FIRST) {
      g_rx_len = 0;
      g_rx_next = 0;
    }
    if (g_rx_next < 0 || idx != (g_rx_next & F_INDEX) ||
        g_rx_len + (len - 1) > MSG_MAX - 1) {
      // Out of order or too long: drop it whole and say so.
      g_rx_next = -1;
      static const char BAD[] = "{\"id\":0,\"ok\":false,\"err\":\"BAD_REQUEST\",\"msg\":\"fragments\"}";
      Msg m = {conn, strdup(BAD), sizeof(BAD) - 1};
      m.n = 0;     // a ready-made answer, not a request
      if (xQueueSend(g_q, &m, 0) != pdTRUE) free(m.data);
      return 0;
    }
    memcpy(g_rx + g_rx_len, buf + 1, len - 1);
    g_rx_len += len - 1;
    g_rx_next++;
    if (flags & F_LAST) {
      Msg m = {conn, (char *)malloc(g_rx_len + 1), g_rx_len};
      if (m.data) {
        memcpy(m.data, g_rx, g_rx_len);
        m.data[g_rx_len] = 0;
        if (xQueueSend(g_q, &m, 0) != pdTRUE) free(m.data);
      }
      g_rx_next = -1;
    }
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

// ---- GAP ------------------------------------------------------------------

int on_gap(ble_gap_event *ev, void *);

void advertise(void) {
  if (!g_synced || g_adv || g_conn != BLE_HS_CONN_HANDLE_NONE) return;
  ble_hs_adv_fields f = {};
  f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  f.uuids128 = &U_SVC;
  f.num_uuids128 = 1;
  f.uuids128_is_complete = 1;
  ble_hs_adv_fields r = {};
  r.name = (uint8_t *)g_sn;       // the SN: what iOS finds the box by
  r.name_len = (uint8_t)strlen(g_sn);
  r.name_is_complete = 1;
  if (ble_gap_adv_set_fields(&f) || ble_gap_adv_rsp_set_fields(&r)) return;

  ble_gap_adv_params p = {};
  p.conn_mode = BLE_GAP_CONN_MODE_UND;
  p.disc_mode = BLE_GAP_DISC_MODE_GEN;
  // 20-40 ms: a phone in someone's hand finds it at once.
  p.itvl_min = 32;
  p.itvl_max = 64;
  if (ble_gap_adv_start(g_own_addr, nullptr, BLE_HS_FOREVER, &p, on_gap, nullptr) == 0) {
    g_adv = true;
  }
}

void stop_advertising(void) {
  if (g_adv) ble_gap_adv_stop();
  g_adv = false;
}

int on_gap(ble_gap_event *ev, void *) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      g_adv = false;
      if (ev->connect.status == 0) {
        g_conn = ev->connect.conn_handle;
        g_mtu = 23;
        g_session = {};
        auth_new_nonce(g_session.nonce);
        g_session.has_nonce = true;
        printf("[ble] connected\n");
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      printf("[ble] disconnected (reason 0x%X)\n", ev->disconnect.reason);
      g_conn = BLE_HS_CONN_HANDLE_NONE;
      g_sub_status = false;
      g_sub_rsp = false;
      g_sub_evt = false;
      g_rx_next = -1;
      // One tap, one session: a key that authorized a session is
      // replaced shortly after it ends.
      if (g_session.authorized) auth_session_ended();
      g_session = {};
      // A phone that drops off may come straight back: keep the door
      // open a little longer.
      if ((int32_t)(g_window_until - now_ms()) < 30000) g_window_until = now_ms() + 30000;
      break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      g_adv = false;
      break;

    case BLE_GAP_EVENT_MTU:
      g_mtu = ev->mtu.value;
      break;

    case BLE_GAP_EVENT_SUBSCRIBE:
      if (ev->subscribe.attr_handle == h_status) g_sub_status = ev->subscribe.cur_notify;
      if (ev->subscribe.attr_handle == h_rsp) g_sub_rsp = ev->subscribe.cur_notify;
      if (ev->subscribe.attr_handle == h_evt) g_sub_evt = ev->subscribe.cur_notify;
      break;

  }
  return 0;
}

void on_sync(void) {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &g_own_addr);
  g_synced = true;
}

void on_reset(int reason) {
  g_synced = false;
  printf("[ble] host reset, reason %d\n", reason);
}

void host_task(void *) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

// ---- the worker: requests, notifications, advertising policy -------------

const char *alarm_ev(int a) {
  switch (a) {
    case AL_TEMP_HIGH: return "TEMP_HIGH";
    case AL_TEMP_LOW:  return "TEMP_LOW";
    case AL_PROBE:     return "NO_TEMP";
    case AL_DOOR:      return "DOOR";
    case AL_BATTERY:   return "BATTERY_LOW";
  }
  return "?";
}

void event(const char *ev, const char *alarm = nullptr) {
  char s[96];
  if (alarm) snprintf(s, sizeof(s), "{\"ev\":\"%s\",\"alarm\":\"%s\"}", ev, alarm);
  else snprintf(s, sizeof(s), "{\"ev\":\"%s\"}", ev);
  send_json(h_evt, g_sub_evt, s);
}

bool stack_up(void);

bool window_open(void) { return (int32_t)(g_window_until - now_ms()) > 0; }

void worker(void *) {
  TripStatus prev;
  trip_status(&prev);
  for (;;) {
    // The stack starts the first time there is a reason to advertise.
    // Most wakes from sleep have none, and skip its start-up and its RAM.
    if (!g_up && g_enabled && (external_power() || window_open())) g_up = stack_up();

    Msg m;
    if (xQueueReceive(g_q, &m, pdMS_TO_TICKS(250)) == pdTRUE) {
      if (m.n == 0) {     // a ready-made error
        send_framed(m.conn, h_rsp, m.data, strlen(m.data));
      } else {
        g_requests = g_requests + 1;
        // A request from a link that has since closed has no session.
        RpcSession none = {};
        char *resp = rpc_handle(m.data, m.n, m.conn == g_conn ? &g_session : &none);
        send_framed(m.conn, h_rsp, resp, strlen(resp));
        free(resp);
      }
      free(m.data);
    }

    // Changes the app wants to hear about without asking.
    TripStatus s;
    trip_status(&s);
    if (g_conn != BLE_HS_CONN_HANDLE_NONE) {
      if (s.active && !prev.active) event("TRIP_START");
      if (!s.active && prev.active) event("TRIP_STOP");
      for (int a = 0; a < AL_COUNT; a++) {
        const uint16_t b = (uint16_t)(1u << a);
        if ((s.alarms_active & b) && !(prev.alarms_active & b)) event("ALARM_RAISE", alarm_ev(a));
        if (!(s.alarms_active & b) && (prev.alarms_active & b)) event("ALARM_CLEAR", alarm_ev(a));
      }
      if (s.acked && !prev.acked) event("ALARM_ACK");
      if (g_sub_status && (s.active != prev.active || s.alarms_active != prev.alarms_active ||
                           s.acked != prev.acked || s.samples != prev.samples)) {
        char *st = rpc_handle("{\"id\":1,\"cmd\":\"GET_STATUS\"}", 27, nullptr);
        send_json(h_status, true, st);
        free(st);
      }
    }
    prev = s;

    // Advertise only with a reason: a tap's window, or external power.
    const bool want = g_up && g_enabled && g_conn == BLE_HS_CONN_HANDLE_NONE &&
                      (external_power() || window_open());
    if (want) advertise();
    else if (g_adv) stop_advertising();
    // Asleep, the radio is off and no phone can reach the box.
    pm_hold(Hold::Ble, g_adv || g_conn != BLE_HS_CONN_HANDLE_NONE ||
                           (g_enabled && window_open()));
  }
}

}  // namespace

void ble_start(const char *sn) {
  snprintf(g_sn, sizeof(g_sn), "%s", sn ? sn : "MCOLD");
  g_q = xQueueCreate(4, sizeof(Msg));
  xTaskCreatePinnedToCore(worker, "ble", 6144, nullptr, 3, nullptr, 0);
}

namespace {

bool stack_up(void) {
  if (nimble_port_init() != ESP_OK) {
    printf("[ble] controller did not start\n");
    return false;
  }
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  // No pairing is asked for: authority comes from the NFC tap (auth.h),
  // so the phone shows no pairing prompt. If a phone pairs anyway, it
  // gets LE Secure Connections, Just Works.
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;

  U_SVC = uuid(0x0000);
  U_INFO = uuid(0x0001);
  U_STATUS = uuid(0x0002);
  U_CMD = uuid(0x0003);
  U_RSP = uuid(0x0004);
  U_EVT = uuid(0x0005);

  memset(g_chrs, 0, sizeof(g_chrs));
  g_chrs[0].uuid = &U_INFO.u;
  g_chrs[0].access_cb = on_access;
  g_chrs[0].arg = (void *)C_INFO;
  g_chrs[0].flags = BLE_GATT_CHR_F_READ;
  g_chrs[1].uuid = &U_STATUS.u;
  g_chrs[1].access_cb = on_access;
  g_chrs[1].arg = (void *)C_STATUS;
  g_chrs[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
  g_chrs[1].val_handle = &h_status;
  // Anyone may write a request; what it may do is up to AUTH.
  g_chrs[2].uuid = &U_CMD.u;
  g_chrs[2].access_cb = on_access;
  g_chrs[2].arg = (void *)C_CMD;
  g_chrs[2].flags = BLE_GATT_CHR_F_WRITE;
  g_chrs[3].uuid = &U_RSP.u;
  g_chrs[3].access_cb = on_access;
  g_chrs[3].arg = (void *)C_RSP;
  g_chrs[3].flags = BLE_GATT_CHR_F_NOTIFY;
  g_chrs[3].val_handle = &h_rsp;
  g_chrs[4].uuid = &U_EVT.u;
  g_chrs[4].access_cb = on_access;
  g_chrs[4].arg = (void *)C_EVT;
  g_chrs[4].flags = BLE_GATT_CHR_F_NOTIFY;
  g_chrs[4].val_handle = &h_evt;

  memset(g_svcs, 0, sizeof(g_svcs));
  g_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  g_svcs[0].uuid = &U_SVC.u;
  g_svcs[0].characteristics = g_chrs;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  if (ble_gatts_count_cfg(g_svcs) || ble_gatts_add_svcs(g_svcs)) {
    printf("[ble] GATT table rejected\n");
    return false;
  }
  ble_svc_gap_device_name_set(g_sn);
  ble_store_config_init();
  nimble_port_freertos_init(host_task);
  return true;
}

}  // namespace

void ble_window(uint32_t ms) {
  const uint32_t until = now_ms() + ms;
  if ((int32_t)(until - g_window_until) > 0) g_window_until = until;
  // At once, not at the worker's next pass: the tap that opened the
  // window may be the last duty of a wake, and the chip would be asleep
  // before the worker looked.
  if (g_enabled) pm_hold(Hold::Ble, true);
}

void ble_enable(bool on) {
  g_enabled = on;
  if (!on) {
    stop_advertising();
    if (g_conn != BLE_HS_CONN_HANDLE_NONE) {
      ble_gap_terminate(g_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
  }
}

void ble_status(BleStatus *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->enabled = g_enabled;
  out->advertising = g_adv;
  out->connected = g_conn != BLE_HS_CONN_HANDLE_NONE;
  out->mtu = g_mtu;
  out->requests = g_requests;
  out->authorized = out->connected && g_session.authorized;
}
