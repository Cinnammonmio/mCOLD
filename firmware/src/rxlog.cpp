#include "rxlog.h"

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "timekeep.h"

namespace {

const int N = 8;          // entries kept; more than ever fit on the glass
const int TEXT = 120;     // one entry, before wrapping

struct Entry {
  char time[9];           // hh:mm:ss local, or --:--:--
  char name[24];          // the command, for folding repeats
  char text[TEXT];
  uint16_t repeat;
};

Entry g_ring[N];
int g_count = 0;          // entries in the ring, up to N
int g_next = 0;           // where the next one goes
volatile uint32_t g_gen = 0;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void now_str(char out[9]) {
  TimeStamp ts;
  time_now(&ts);
  if (ts.quality == TimeSource::None) {
    snprintf(out, 9, "--:--:--");
    return;
  }
  const time_t local = (time_t)(ts.utc_ms / 1000) + config().tz_offset_min * 60;
  struct tm tm;
  gmtime_r(&local, &tm);
  snprintf(out, 9, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void add(const char *name, const char *text) {
  char t[9];
  now_str(t);
  portENTER_CRITICAL(&g_mux);
  const int last = (g_next + N - 1) % N;
  if (g_count && !strcmp(g_ring[last].name, name) && !strcmp(g_ring[last].text, text)) {
    // The same command with the same answer: count it on its line.
    memcpy(g_ring[last].time, t, sizeof(t));
    if (g_ring[last].repeat < 999) g_ring[last].repeat++;
  } else {
    Entry &e = g_ring[g_next];
    memcpy(e.time, t, sizeof(t));
    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.text, sizeof(e.text), "%s", text);
    e.repeat = 1;
    g_next = (g_next + 1) % N;
    if (g_count < N) g_count++;
  }
  g_gen = g_gen + 1;
  portEXIT_CRITICAL(&g_mux);
}

// Values never shown, wherever they sit in the request.
void mask(cJSON *o) {
  for (cJSON *k = o ? o->child : nullptr; k; k = k->next) {
    if (k->string && cJSON_IsString(k) &&
        (!strcmp(k->string, "pass") || !strcmp(k->string, "proof") || !strcmp(k->string, "key"))) {
      cJSON_SetValuestring(k, "***");
    } else if (cJSON_IsObject(k) || cJSON_IsArray(k)) {
      mask(k);
    }
  }
}

// JSON without its braces and quotes: the small font has no { } and the
// line is short.
void plain(const char *in, char *out, size_t n) {
  size_t k = 0;
  for (const char *p = in; *p && k + 1 < n; p++) {
    if (*p == '{' || *p == '}' || *p == '"') continue;
    out[k++] = *p == '|' ? '/' : *p;
  }
  out[k] = 0;
}

}  // namespace

void rxlog_note(const char *what) {
  if (!config().rx_show) return;
  add("", what);
}

void rxlog_rpc(const char *req, size_t n, const char *resp, const RpcSession *s) {
  if (!config().rx_show) return;
  cJSON *r = cJSON_ParseWithLength(req, n);
  // The whole exchange on the console too, for whoever is debugging the
  // app: the request as it came and the answer as it went, secrets masked.
  {
    if (r) mask(r);
    char *j = r ? cJSON_PrintUnformatted(r) : nullptr;
    printf("[rx] %s%s\n", s && s->console ? "(usb) " : "", j ? j : "(not JSON)");
    if (j) cJSON_free(j);
    cJSON *a = resp ? cJSON_Parse(resp) : nullptr;
    if (a) mask(a);
    char *k = a ? cJSON_PrintUnformatted(a) : nullptr;
    const char *out = k ? k : (resp ? resp : "");
    const size_t len = strlen(out);
    printf("[tx] %.*s%s\n", len > 600 ? 600 : (int)len, out, len > 600 ? " ..." : "");
    if (k) cJSON_free(k);
    cJSON_Delete(a);
    fflush(stdout);
  }
  const cJSON *jc = r ? cJSON_GetObjectItemCaseSensitive(r, "cmd") : nullptr;
  char name[24];
  snprintf(name, sizeof(name), "%s", cJSON_IsString(jc) ? jc->valuestring : "BAD_REQUEST");
  char params[TEXT] = "";
  if (r) {
    cJSON_DeleteItemFromObjectCaseSensitive(r, "id");
    cJSON_DeleteItemFromObjectCaseSensitive(r, "cmd");
    cJSON_DeleteItemFromObjectCaseSensitive(r, "proto");
    mask(r);
    if (r->child) {
      char *j = cJSON_PrintUnformatted(r);
      if (j) {
        plain(j, params, sizeof(params));
        cJSON_free(j);
      }
    }
    cJSON_Delete(r);
  }
  // The answer: ok, or the error code.
  char result[32] = "?";
  if (resp && strstr(resp, "\"ok\":true")) {
    snprintf(result, sizeof(result), "ok");
  } else if (const char *e = resp ? strstr(resp, "\"err\":\"") : nullptr) {
    e += 7;
    size_t k = 0;
    while (e[k] && e[k] != '"' && k + 1 < sizeof(result)) {
      result[k] = e[k];
      k++;
    }
    result[k] = 0;
  }
  char text[TEXT];
  snprintf(text, sizeof(text), "%s%s%s > %s%s", name, params[0] ? " " : "", params, result,
           s && s->console ? " (usb)" : "");
  add(name, text);
}

uint32_t rxlog_gen(void) { return g_gen; }

int rxlog_render(char out[][RXLOG_W + 1], int max) {
  // Static, not on the stack: the display task calls this, and these
  // 2.5 KB on its 4 KB restarted the box (0.7.0-dev.28, 2026-10-06).
  // One caller, so one copy is enough.
  static Entry e[N];
  int count, next;
  portENTER_CRITICAL(&g_mux);
  memcpy(e, g_ring, sizeof(e));
  count = g_count;
  next = g_next;
  portEXIT_CRITICAL(&g_mux);

  // Newest first, wrapped, until the lines run out; then turned round.
  static char lines[RXLOG_LINES * 4][RXLOG_W + 1];
  int n = 0;
  for (int i = 0; i < count && n < max; i++) {
    const Entry &x = e[(next + N - 1 - i) % N];
    char full[TEXT + 24];
    if (x.repeat > 1) {
      snprintf(full, sizeof(full), "%s %s x%u", x.time, x.text, x.repeat);
    } else {
      snprintf(full, sizeof(full), "%s %s", x.time, x.text);
    }
    // Wrapped at RXLOG_W, continuation lines indented under the text.
    char wrapped[4][RXLOG_W + 1];
    int w = 0;
    const char *p = full;
    while (*p && w < 4) {
      const int indent = w ? 9 : 0;
      const int room = RXLOG_W - indent;
      memset(wrapped[w], ' ', indent);
      int len = (int)strlen(p);
      if (len > room) len = room;
      memcpy(wrapped[w] + indent, p, len);
      wrapped[w][indent + len] = 0;
      p += len;
      w++;
    }
    // This entry's lines, kept in order, ahead of the older ones.
    for (int k = w - 1; k >= 0 && n < max; k--) memcpy(lines[n++], wrapped[k], RXLOG_W + 1);
  }
  for (int i = 0; i < n; i++) memcpy(out[i], lines[n - 1 - i], RXLOG_W + 1);
  return n;
}
