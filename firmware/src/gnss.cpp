#include "gnss.h"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "health.h"
#include "rails.h"

namespace {

const uart_port_t PORT = UART_NUM_1;
const int RX_BUF = 2048;

bool g_on = false;
char g_line[100];
int g_len = 0;
bool g_overlong = false;

GnssFix g_fix = {};
bool g_have_fix = false;
uint32_t g_last_sentence = 0;
uint32_t g_on_at = 0;
GnssStats g_stats = {};
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// "$...*hh" with the XOR of everything between $ and * equal to hh.
// The only test that separates the module talking from line noise that
// happens to contain a dollar sign.
bool checksum_ok(const char *s, int n) {
  if (n < 4 || s[0] != '$') return false;
  uint8_t x = 0;
  int i = 1;
  for (; i < n && s[i] != '*'; i++) x ^= (uint8_t)s[i];
  if (i + 2 >= n) return false;      // no '*', or no two digits after it
  const int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
  return hi >= 0 && lo >= 0 && (uint8_t)(hi << 4 | lo) == x;
}

// Field `k` of a comma-separated sentence, field 0 being "$GNRMC".
// Empty fields are real -- a module with no fix sends ",,,," -- so this
// cannot be strtok, which would silently shift every field after one.
bool field(const char *s, int k, char *out, size_t cap) {
  int f = 0;
  const char *p = s;
  while (f < k) {
    p = strchr(p, ',');
    if (!p) return false;
    p++;
    f++;
  }
  size_t n = 0;
  while (p[n] && p[n] != ',' && p[n] != '*') n++;
  if (n >= cap) return false;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

// ddmm.mmmm plus a hemisphere letter, to signed degrees.
bool coord(const char *v, const char *hemi, double *out) {
  if (!*v || !*hemi) return false;
  const double raw = atof(v);
  const int deg = (int)(raw / 100);
  double d = deg + (raw - deg * 100) / 60.0;
  if (*hemi == 'S' || *hemi == 'W') d = -d;
  *out = d;
  return true;
}

int two(const char *p) { return (p[0] - '0') * 10 + (p[1] - '0'); }

void parse_rmc(const char *s, uint32_t t) {
  char tm_s[16], st[4], la[16], ns[4], lo[16], ew[4], sp[12], co[12], dt[10];
  if (!field(s, 1, tm_s, sizeof tm_s) || !field(s, 2, st, sizeof st) ||
      !field(s, 3, la, sizeof la) || !field(s, 4, ns, sizeof ns) ||
      !field(s, 5, lo, sizeof lo) || !field(s, 6, ew, sizeof ew) ||
      !field(s, 7, sp, sizeof sp) || !field(s, 8, co, sizeof co) ||
      !field(s, 9, dt, sizeof dt)) {
    return;
  }

  GnssFix f;
  portENTER_CRITICAL(&g_mux);
  f = g_fix;
  portEXIT_CRITICAL(&g_mux);

  f.valid = (st[0] == 'A');
  if (f.valid) {
    double lat, lon;
    if (!coord(la, ns, &lat) || !coord(lo, ew, &lon)) {
      f.valid = false;
    } else {
      f.lat_deg = lat;
      f.lon_deg = lon;
      f.speed_kmh = *sp ? (float)atof(sp) * 1.852f : 0;
      f.course_deg = *co ? (float)atof(co) : 0;
    }
  }

  // The time is taken only with status A. Before a fix, the module can
  // report a time recovered from its backup domain, or none dressed up
  // as midnight -- and this time may go on to set the RTC, from which
  // every record is stamped.
  f.time_valid = false;
  if (f.valid && strlen(tm_s) >= 6 && strlen(dt) == 6) {
    memset(&f.utc, 0, sizeof(f.utc));
    f.utc.tm_hour = two(tm_s);
    f.utc.tm_min = two(tm_s + 2);
    f.utc.tm_sec = two(tm_s + 4);
    f.utc.tm_mday = two(dt);
    f.utc.tm_mon = two(dt + 2) - 1;
    f.utc.tm_year = two(dt + 4) + 100;
    // A module that has never seen a satellite reports 2080 or 1980
    // depending on its firmware. Either is a guess, not a time.
    f.time_valid = f.utc.tm_year >= 124 && f.utc.tm_year < 200 &&
                   f.utc.tm_mon >= 0 && f.utc.tm_mon < 12 && f.utc.tm_mday >= 1;
  }

  if (f.valid) {
    f.at_ms = t;
    g_stats.fixes++;
  }
  portENTER_CRITICAL(&g_mux);
  g_fix = f;
  if (f.valid) g_have_fix = true;
  portEXIT_CRITICAL(&g_mux);
}

// One GSV sequence per constellation, each split over several sentences
// of up to four satellites. Totals are kept per talker and summed when
// read, so GPS and BeiDou do not overwrite each other.
const int TALKERS = 5;
uint8_t g_view[TALKERS], g_heard[TALKERS], g_best[TALKERS];
uint32_t g_sky_at = 0;

int talker_index(const char *s) {
  if (!strncmp(s + 1, "GP", 2)) return 0;
  if (!strncmp(s + 1, "BD", 2) || !strncmp(s + 1, "GB", 2)) return 1;
  if (!strncmp(s + 1, "GL", 2)) return 2;
  if (!strncmp(s + 1, "GA", 2)) return 3;
  return 4;
}

void parse_gsv(const char *s, uint32_t t) {
  char num[4], view[4], snr[6];
  if (!field(s, 2, num, sizeof num) || !field(s, 3, view, sizeof view)) return;
  const int k = talker_index(s);
  portENTER_CRITICAL(&g_mux);
  if (atoi(num) == 1) {
    g_heard[k] = 0;
    g_best[k] = 0;
  }
  g_view[k] = (uint8_t)atoi(view);
  portEXIT_CRITICAL(&g_mux);

  // Groups of four fields from field 4: PRN, elevation, azimuth, SNR.
  // An empty SNR is a satellite expected but not heard.
  for (int g = 0; g < 4; g++) {
    if (!field(s, 4 + g * 4 + 3, snr, sizeof snr)) break;
    if (!*snr) continue;
    const int v = atoi(snr);
    if (v <= 0) continue;
    portENTER_CRITICAL(&g_mux);
    g_heard[k]++;
    if (v > g_best[k]) g_best[k] = (uint8_t)v;
    portEXIT_CRITICAL(&g_mux);
  }
  g_sky_at = t;
}

void parse_gga(const char *s) {
  char q[4], ns[6], hd[10], al[12];
  if (!field(s, 6, q, sizeof q) || !field(s, 7, ns, sizeof ns) ||
      !field(s, 8, hd, sizeof hd) || !field(s, 9, al, sizeof al)) {
    return;
  }
  portENTER_CRITICAL(&g_mux);
  g_fix.quality = (uint8_t)atoi(q);
  g_fix.sats = (uint8_t)atoi(ns);
  g_fix.hdop = *hd ? (float)atof(hd) : 0;
  g_fix.alt_valid = g_fix.quality > 0 && *al;
  g_fix.alt_m = g_fix.alt_valid ? (float)atof(al) : 0;
  portEXIT_CRITICAL(&g_mux);
}

// Returns true for a sentence with a good checksum.
bool handle_line(const char *s, int n) {
  if (!checksum_ok(s, n)) {
    g_stats.bad_checksum++;
    return false;
  }
  g_stats.sentences++;
  const uint32_t t = now_ms();
  g_last_sentence = t;

  // Talker is two letters (GP, GN, BD, GL, GA) and varies with which
  // constellations the module is tracking; only the sentence type
  // matters here.
  if (n > 6 && !strncmp(s + 3, "RMC,", 4)) parse_rmc(s, t);
  else if (n > 6 && !strncmp(s + 3, "GGA,", 4)) parse_gga(s);
  else if (n > 6 && !strncmp(s + 3, "GSV,", 4)) parse_gsv(s, t);
  return true;
}

void release_pins(void) {
  // Input with no pulls: an output or a pull-up here would feed the
  // unpowered module through its UART pins.
  gpio_config_t c = {};
  c.pin_bit_mask = (1ULL << PIN_GNSS_TX) | (1ULL << PIN_GNSS_RX);
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&c);
}

}  // namespace

bool gnss_power_on(void) {
  if (g_on) return true;
  rail_acquire(Rail::SdGnss);

  uart_config_t uc = {};
  uc.baud_rate = (int)GNSS_BAUD;
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_DEFAULT;
  if (uart_driver_install(PORT, RX_BUF, 0, 0, nullptr, 0) != ESP_OK ||
      uart_param_config(PORT, &uc) != ESP_OK ||
      uart_set_pin(PORT, PIN_GNSS_TX, PIN_GNSS_RX, UART_PIN_NO_CHANGE,
                   UART_PIN_NO_CHANGE) != ESP_OK) {
    uart_driver_delete(PORT);
    release_pins();
    rail_release(Rail::SdGnss);
    health_fail(Dev::Gnss, -400);
    return false;
  }
  g_len = 0;
  g_overlong = false;
  g_on = true;
  g_on_at = now_ms();
  g_stats.sessions++;
  return true;
}

void gnss_power_off(void) {
  if (!g_on) return;
  uart_driver_delete(PORT);
  release_pins();
  rail_release(Rail::SdGnss);
  g_on = false;
}

bool gnss_is_on(void) { return g_on; }

int gnss_pump(uint32_t wait_ms) {
  if (!g_on) return 0;
  const uint32_t start = now_ms();
  int good = 0;
  uint8_t buf[128];

  for (;;) {
    const uint32_t spent = now_ms() - start;
    if (spent >= wait_ms) break;
    const uint32_t left = wait_ms - spent;
    const int n = uart_read_bytes(PORT, buf, sizeof(buf),
                                  pdMS_TO_TICKS(left < 100 ? left : 100));
    if (n <= 0) continue;
    g_stats.bytes += (uint32_t)n;
    for (int i = 0; i < n; i++) {
      const char c = (char)buf[i];
      if (c == '$') {
        g_len = 0;
        g_overlong = false;
      }
      if (c == '\r' || c == '\n') {
        if (g_len > 0 && !g_overlong) {
          g_line[g_len] = 0;
          if (handle_line(g_line, g_len)) good++;
        }
        g_len = 0;
        g_overlong = false;
      } else if (g_len < (int)sizeof(g_line) - 1) {
        g_line[g_len++] = c;
      } else if (!g_overlong) {
        g_overlong = true;
        g_stats.overlong++;
      }
    }
  }

  // Health is about whether the module talks, not whether it can see
  // the sky. Hearing it is Ok; silence past the alive window is a fault.
  if (good > 0) {
    health_ok(Dev::Gnss);
  } else if (now_ms() - g_on_at >= GNSS_ALIVE_MS &&
             now_ms() - g_last_sentence >= GNSS_ALIVE_MS) {
    health_fail(Dev::Gnss, g_stats.bad_checksum ? -401 : -402);
  }
  return good;
}

bool gnss_last_fix(GnssFix *out) {
  portENTER_CRITICAL(&g_mux);
  const bool have = g_have_fix;
  if (out) *out = g_fix;
  portEXIT_CRITICAL(&g_mux);
  return have;
}

uint32_t gnss_last_sentence_ms(void) { return g_last_sentence; }

void gnss_stats(GnssStats *out) {
  if (out) *out = g_stats;
}

void gnss_sky(GnssSky *out) {
  if (!out) return;
  GnssSky k = {};
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < TALKERS; i++) {
    k.in_view += g_view[i];
    k.heard += g_heard[i];
    if (g_best[i] > k.best_snr) k.best_snr = g_best[i];
  }
  portEXIT_CRITICAL(&g_mux);
  k.at_ms = g_sky_at;
  *out = k;
}
