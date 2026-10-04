#include "ota.h"

#include <ctype.h>
#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "pm.h"
#include "soc.h"
#include "timekeep.h"
#include "trip.h"

namespace {

const char *NS = "ota";
const uint32_t NET_WAIT_MS = 30000;   // for Wi-Fi, once a download is asked for
const uint32_t LOOK_MS = 60000;       // a waiting request looks again this often
const uint8_t MAX_TRIES = 3;          // downloads of one file that fail, before giving up on it

TaskHandle_t g_task = nullptr;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// One request at a time, handed over from the MQTT task or the console.
struct Request {
  bool on;
  bool from_server;
  bool force;
  char name[160];
};
Request g_req = {};

volatile bool g_busy = false;        // downloading
volatile bool g_pending = false;     // a new image, not confirmed yet
volatile bool g_connected = false;   // the broker answered since the task last looked
bool g_no_confirm = false;           // bench: let this new image roll back
uint32_t g_verify_by = 0;
char g_state[96] = "idle";           // the console's view

// What the server should hear, on ota/state. A final word (ok, failed,
// rolled back, waiting) is also kept in NVS until it has gone out, so a
// sleep or a reboot does not lose it; progress is not worth a flash write.
char g_out[200] = "";
uint32_t g_out_serial = 0;
bool g_out_kept = false;

// Failed downloads of one file, through deep sleep: a file the server
// keeps naming but that never arrives is given up after MAX_TRIES.
struct Tries {
  char name[64];
  uint8_t n;
};
RTC_DATA_ATTR Tries g_tries;

// ---- NVS ---------------------------------------------------------------

bool get_str(const char *k, char *out, size_t n) {
  nvs_handle_t h;
  out[0] = 0;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = n;
  const bool ok = nvs_get_str(h, k, out, &len) == ESP_OK;
  nvs_close(h);
  if (!ok) out[0] = 0;
  return ok && out[0];
}

// Empty or null erases the key.
void put_str(const char *k, const char *v) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
  if (v && *v) nvs_set_str(h, k, v);
  else nvs_erase_key(h, k);
  nvs_commit(h);
  nvs_close(h);
}

// ---- versions: a.b.c[-suffix] -------------------------------------------

// Digit runs compare as numbers, so 0.10 is after 0.9 and dev.10 after dev.9.
int natcmp(const char *a, const char *b) {
  while (*a && *b) {
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
      char *ea, *eb;
      const unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
      if (x != y) return x < y ? -1 : 1;
      a = ea;
      b = eb;
    } else {
      if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
      a++;
      b++;
    }
  }
  return *a ? 1 : *b ? -1 : 0;
}

// A release is after its own pre-releases: 0.7.1 > 0.7.1-dev.
int vercmp(const char *a, const char *b) {
  if (*a == 'v') a++;
  if (*b == 'v') b++;
  char na[32], nb[32];
  snprintf(na, sizeof(na), "%.*s", (int)strcspn(a, "-"), a);
  snprintf(nb, sizeof(nb), "%.*s", (int)strcspn(b, "-"), b);
  const int c = natcmp(na, nb);
  if (c) return c;
  const char *sa = strchr(a, '-'), *sb = strchr(b, '-');
  if (!sa && !sb) return 0;
  if (!sa) return 1;
  if (!sb) return -1;
  return natcmp(sa + 1, sb + 1);
}

// ---- what the server hears ---------------------------------------------

void say(bool keep, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void say(bool keep, const char *fmt, ...) {
  char s[sizeof(g_out)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s, sizeof(s), fmt, ap);
  va_end(ap);
  printf("[ota] %s\n", s);
  portENTER_CRITICAL(&g_mux);
  memcpy(g_out, s, sizeof(g_out));
  g_out_serial++;
  g_out_kept = keep;
  portEXIT_CRITICAL(&g_mux);
  // Kept, or a newer word that makes the kept one stale.
  put_str("report", keep ? s : nullptr);
}

void set_state(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void set_state(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_state, sizeof(g_state), fmt, ap);
  va_end(ap);
}

const char *running_version(void) { return esp_app_get_description()->version; }

// ---- the download ----------------------------------------------------------

// A name is a file on the base URL, nothing else: no slashes, so it cannot
// walk off the server's firmware folder. A whole URL is taken as it is --
// the signature, not the address, is what makes an image acceptable.
bool make_url(const char *name, char *url, size_t n, const char **why) {
  if (!strncmp(name, "http://", 7) || !strncmp(name, "https://", 8)) {
    if (strlen(name) >= n) { *why = "URL too long"; return false; }
    snprintf(url, n, "%s", name);
    return true;
  }
  if (!*name || strlen(name) > 63) { *why = "bad file name"; return false; }
  for (const char *p = name; *p; p++) {
    if (!isalnum((unsigned char)*p) && !strchr("._-+", *p)) { *why = "bad file name"; return false; }
  }
  char base[128];
  if (!get_str("base", base, sizeof(base))) { *why = "no base URL (ota base ...)"; return false; }
  const size_t bl = strlen(base);
  const bool slash = bl && base[bl - 1] == '/';
  if (bl + 1 + strlen(name) >= n) { *why = "URL too long"; return false; }
  snprintf(url, n, "%s%s%s", base, slash ? "" : "/", name);
  return true;
}

// A file the server named and that is settled one way or the other: not
// fetched again because a retained message names it at every session.
void settle(const Request &r) {
  if (r.from_server) put_str("done", r.name);
  put_str("want", nullptr);
  g_tries = {};
}

// A failure that may pass (Wi-Fi, a server down): tried again at the next
// request, up to MAX_TRIES for one file.
void failed_soft(const Request &r, const char *why) {
  if (strncmp(g_tries.name, r.name, sizeof(g_tries.name) - 1)) {
    snprintf(g_tries.name, sizeof(g_tries.name), "%s", r.name);
    g_tries.n = 0;
  }
  if (++g_tries.n >= MAX_TRIES) {
    say(true, "{\"file\":\"%s\",\"state\":\"failed\",\"reason\":\"%s\",\"tries\":%u}", r.name, why,
        g_tries.n);
    settle(r);
  } else {
    say(true, "{\"file\":\"%s\",\"state\":\"failed\",\"reason\":\"%s\",\"retry\":true}", r.name, why);
  }
  set_state("failed: %s", why);
}

void failed_hard(const Request &r, const char *why) {
  say(true, "{\"file\":\"%s\",\"state\":\"failed\",\"reason\":\"%s\"}", r.name, why);
  set_state("failed: %s", why);
  settle(r);
}

// Not now; once the trip is over or the battery is up. Said once per reason.
void defer(const Request &r, const char *why) {
  char want[160];
  get_str("want", want, sizeof(want));
  if (strcmp(want, r.name) || strstr(g_state, why) == nullptr) {
    say(true, "{\"file\":\"%s\",\"state\":\"deferred\",\"reason\":\"%s\"}", r.name, why);
  }
  put_str("want", r.name);
  put_str("want_srv", r.from_server ? "1" : nullptr);
  set_state("waiting (%s): %s", why, r.name);
}

const char *why_not_now(void) {
  TripStatus s;
  trip_status(&s);
  if (s.active) return "trip";
  if (!pm_external_power()) {
    SocStatus so;
    soc_status(&so);
    if (!so.valid || so.percent < OTA_MIN_SOC) return "battery";
  }
  return nullptr;
}

void finish_busy(void) {
  g_busy = false;
  pm_no_light_sleep(false);
  pm_hold(Hold::Ota, false);
}

void run(const Request &r) {
  char url[224];
  const char *why = nullptr;
  if (!make_url(r.name, url, sizeof(url), &why)) {
    failed_hard(r, why);
    return;
  }
  if (!r.force && (why = why_not_now()) != nullptr) {
    defer(r, why);
    return;
  }

  g_busy = true;
  pm_hold(Hold::Ota, true);
  pm_no_light_sleep(true);
  set_state("waiting for Wi-Fi");
  NetStatus ns;
  const uint32_t t0 = mono_ms();
  for (net_status(&ns); !ns.connected && mono_ms() - t0 < NET_WAIT_MS; net_status(&ns)) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  if (!ns.connected) {
    failed_soft(r, "no network");
    finish_busy();
    return;
  }

  printf("[ota] fetching %s\n", url);
  set_state("connecting: %s", r.name);
  esp_http_client_config_t hc = {};
  hc.url = url;
  hc.crt_bundle_attach = esp_crt_bundle_attach;   // public CAs, for an https file server
  hc.timeout_ms = 15000;
  hc.keep_alive_enable = true;
  hc.buffer_size = 4096;
  hc.buffer_size_tx = 1024;
  esp_https_ota_config_t oc = {};
  oc.http_config = &hc;
  esp_https_ota_handle_t h = nullptr;
  esp_err_t e = esp_https_ota_begin(&oc, &h);
  if (e != ESP_OK) {
    failed_soft(r, "cannot fetch the file");
    finish_busy();
    return;
  }

  // 1. The header, from the first few KB: whose image, which version.
  esp_app_desc_t d;
  const esp_app_desc_t *me = esp_app_get_description();
  e = esp_https_ota_get_img_desc(h, &d);
  if (e != ESP_OK) {
    esp_https_ota_abort(h);
    failed_soft(r, "no image header");
    finish_busy();
    return;
  }
  d.version[sizeof(d.version) - 1] = 0;
  d.project_name[sizeof(d.project_name) - 1] = 0;
  printf("[ota] image: %s %s, built %s %s\n", d.project_name, d.version, d.date, d.time);
  if (strncmp(d.project_name, me->project_name, sizeof(d.project_name))) {
    esp_https_ota_abort(h);
    failed_hard(r, "not an mCOLD image");
    finish_busy();
    return;
  }
  if (!r.force && vercmp(d.version, me->version) <= 0) {
    esp_https_ota_abort(h);
    say(true, "{\"file\":\"%s\",\"ver\":\"%s\",\"state\":\"skipped\",\"reason\":\"not newer\",\"running\":\"%s\"}",
        r.name, d.version, me->version);
    set_state("skipped: %s is not newer than %s", d.version, me->version);
    settle(r);
    finish_busy();
    return;
  }

  // 2. The body, straight into the other slot. esp_ota_end checks the
  //    image's SHA-256 and (3.) the signature once it is all there.
  const int total = esp_https_ota_get_image_size(h);
  say(false, "{\"ver\":\"%s\",\"state\":\"downloading\",\"pct\":0}", d.version);
  int shown = 0;
  for (;;) {
    e = esp_https_ota_perform(h);
    if (e != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
    const int got = esp_https_ota_get_image_len_read(h);
    const int pct = total > 0 ? (int)((int64_t)got * 100 / total) : 0;
    set_state("downloading %s: %d%%", d.version, pct);
    if (pct >= shown + 10) {
      shown = pct - pct % 10;
      say(false, "{\"ver\":\"%s\",\"state\":\"downloading\",\"pct\":%d}", d.version, shown);
    }
  }
  if (e != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
    esp_https_ota_abort(h);
    failed_soft(r, "download broke off");
    finish_busy();
    return;
  }
  e = esp_https_ota_finish(h);
  if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
    failed_hard(r, "image check failed (signature or SHA-256)");
    finish_busy();
    return;
  }
  if (e != ESP_OK) {
    failed_soft(r, esp_err_to_name(e));
    finish_busy();
    return;
  }

  // 4. Booted next, on probation: see ota_start.
  put_str("from", me->version);
  put_str("to", d.version);
  settle(r);
  say(false, "{\"ver\":\"%s\",\"state\":\"rebooting\",\"from\":\"%s\"}", d.version, me->version);
  set_state("rebooting into %s", d.version);
  // A moment for the uplink to pass that on.
  vTaskDelay(pdMS_TO_TICKS(1500));
  fflush(stdout);
  esp_restart();
}

void confirm(void) {
  char from[32];
  get_str("from", from, sizeof(from));
  if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    printf("[ota] could not mark this image valid\n");
    return;
  }
  g_pending = false;
  pm_hold(Hold::Ota, false);
  put_str("from", nullptr);
  put_str("to", nullptr);
  say(true, "{\"ver\":\"%s\",\"state\":\"ok\",\"from\":\"%s\"}", running_version(), from);
  set_state("idle; %s confirmed", running_version());
}

void task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g_pending ? 1000 : LOOK_MS));

    if (g_pending) {
      if (g_connected && !g_no_confirm) {
        confirm();
      } else if ((int32_t)(mono_ms() - g_verify_by) >= 0) {
        printf("[ota] this image never reached the broker: back to the last one\n");
        fflush(stdout);
        esp_ota_mark_app_invalid_rollback_and_reboot();
      }
      continue;
    }
    g_connected = false;

    Request r;
    portENTER_CRITICAL(&g_mux);
    r = g_req;
    g_req.on = false;
    portEXIT_CRITICAL(&g_mux);
    if (!r.on) {
      // A request that had to wait: its turn yet?
      char want[160], srv[4];
      if (!get_str("want", want, sizeof(want)) || why_not_now()) continue;
      r = {true, get_str("want_srv", srv, sizeof(srv)), false, ""};
      snprintf(r.name, sizeof(r.name), "%s", want);
    }
    run(r);
  }
}

}  // namespace

void ota_start(void) {
  char s[sizeof(g_out)];
  if (get_str("report", s, sizeof(s))) {      // not yet heard by the server
    memcpy(g_out, s, sizeof(g_out));
    g_out_serial = 1;
    g_out_kept = true;
  }

  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  char to[32];
  const bool updating = get_str("to", to, sizeof(to));
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    // On probation. The pm hold keeps the chip from sleeping meanwhile:
    // a deep-sleep wake runs the bootloader, which takes a still-pending
    // image for one that failed and goes back.
    g_pending = true;
    g_verify_by = mono_ms() + OTA_VERIFY_MS;
    pm_hold(Hold::Ota, true);
    char t[4];
    if (get_str("noconfirm", t, sizeof(t))) {
      g_no_confirm = true;
      put_str("noconfirm", nullptr);
    }
    printf("[ota] new image %s on %s: %s within %lu s\n", running_version(), run->label,
           g_no_confirm ? "bench rollback test, will NOT confirm" : "confirms itself on reaching the broker",
           (unsigned long)(OTA_VERIFY_MS / 1000));
    set_state("new image, waiting for the broker");
  } else if (updating) {
    if (strcmp(to, running_version())) {
      // Downloaded and rebooted into, but this is not it: the bootloader
      // came back here.
      say(true, "{\"ver\":\"%s\",\"state\":\"rolled_back\",\"running\":\"%s\"}", to, running_version());
      set_state("%s rolled back", to);
    }
    put_str("from", nullptr);
    put_str("to", nullptr);
  }
  xTaskCreatePinnedToCore(task, "ota", 8192, nullptr, 2, &g_task, 0);
}

void ota_request(const char *name, bool from_server, bool force) {
  if (!name) return;
  while (*name == ' ' || *name == '"') name++;
  char n[sizeof(g_req.name)];
  snprintf(n, sizeof(n), "%s", name);
  for (size_t l = strlen(n); l && strchr(" \"\r\n", n[l - 1]); l--) n[l - 1] = 0;
  if (!*n) return;             // the server clearing its retained message
  if (from_server) {
    char done[sizeof(n)];
    if (get_str("done", done, sizeof(done)) && !strcmp(done, n)) return;
  }
  if (g_busy || g_pending) {
    printf("[ota] busy; %s not taken\n", n);
    return;
  }
  portENTER_CRITICAL(&g_mux);
  g_req.on = true;
  g_req.from_server = from_server;
  g_req.force = force;
  memcpy(g_req.name, n, sizeof(g_req.name));
  portEXIT_CRITICAL(&g_mux);
  if (g_task) xTaskNotifyGive(g_task);
}

bool ota_busy(void) { return g_busy || g_pending || g_req.on; }

bool ota_needs_broker(void) { return g_pending; }

void ota_on_connected(void) {
  g_connected = true;
  if (g_task) xTaskNotifyGive(g_task);
}

bool ota_outbox(char *buf, size_t n, uint32_t *serial) {
  portENTER_CRITICAL(&g_mux);
  const bool any = g_out[0] != 0;
  if (any) snprintf(buf, n, "%s", g_out);
  *serial = g_out_serial;
  portEXIT_CRITICAL(&g_mux);
  return any;
}

void ota_outbox_sent(uint32_t serial) {
  portENTER_CRITICAL(&g_mux);
  const bool clear = serial == g_out_serial && g_out_kept;
  if (clear) g_out_kept = false;
  portEXIT_CRITICAL(&g_mux);
  if (clear) put_str("report", nullptr);
}

bool ota_set_base(const char *url) {
  if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) || strlen(url) >= 128) {
    return false;
  }
  put_str("base", url);
  return true;
}

void ota_print(void) {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  if (run) esp_ota_get_state_partition(run, &st);
  const char *sn = st == ESP_OTA_IMG_VALID            ? "valid"
                   : st == ESP_OTA_IMG_PENDING_VERIFY ? "pending verify"
                   : st == ESP_OTA_IMG_NEW            ? "new"
                   : st == ESP_OTA_IMG_INVALID        ? "invalid"
                   : st == ESP_OTA_IMG_ABORTED        ? "aborted"
                                                      : "undefined (flashed over USB)";
  char base[128], done[160], want[160];
  get_str("base", base, sizeof(base));
  get_str("done", done, sizeof(done));
  get_str("want", want, sizeof(want));
  printf("  running %s on %s (%s); updates go to %s\n", running_version(), run ? run->label : "?", sn,
         next ? next->label : "?");
  printf("  base    %s\n", base[0] ? base : "(none: ota base URL)");
  printf("  state   %s\n", g_state);
  if (g_pending) {
    printf("  confirm by reaching the broker in %ld s%s\n",
           (long)((int32_t)(g_verify_by - mono_ms()) / 1000), g_no_confirm ? " (bench: will not)" : "");
  }
  if (want[0]) printf("  waiting %s\n", want);
  if (done[0]) printf("  last    %s (from the server; named again, it is ignored)\n", done);
  if (g_out[0]) printf("  told    %s%s\n", g_out, g_out_kept ? "  (not sent yet)" : "");
  const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
  if (bad) printf("  rolled back from %s once\n", bad->label);
}

void ota_test_rollback(void) { put_str("noconfirm", "1"); }
