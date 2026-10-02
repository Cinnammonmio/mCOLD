#include "auth.h"

#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>

#include "nfc.h"

namespace {

char g_sn[16] = "";
uint8_t g_key[16];            // the key in force: what auth_check() accepts
uint8_t g_next[16];           // the key being written to the tag
bool g_have_key = false;      // false until a key has reached the tag
volatile bool g_publish = true;
volatile uint32_t g_rotate_at = 0;   // 0: no rotation pending
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void hex(const uint8_t *b, size_t n, char *out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = H[b[i] >> 4];
    out[2 * i + 1] = H[b[i] & 15];
  }
  out[2 * n] = 0;
}

int unhex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// The record the app reads: an application/json MIME record, the format
// bring-up wrote and the app team was given, with "key" added. JSON
// readers ignore keys they do not know, so this does not break them.
size_t ndef_json(const char *json, uint8_t *out, size_t cap) {
  static const char MIME[] = "application/json";
  const size_t ml = sizeof(MIME) - 1, jl = strlen(json);
  if (jl > 255 || 3 + ml + jl > cap) return 0;
  size_t i = 0;
  out[i++] = 0xD2;            // MB | ME | SR | TNF 2 (MIME)
  out[i++] = (uint8_t)ml;
  out[i++] = (uint8_t)jl;
  memcpy(out + i, MIME, ml);
  i += ml;
  memcpy(out + i, json, jl);
  return i + jl;
}

}  // namespace

void auth_init(const char *sn) {
  snprintf(g_sn, sizeof(g_sn), "%s", sn ? sn : "");
  // A new key at every boot: a key read before the reset is spent.
  esp_fill_random(g_next, sizeof(g_next));
  g_have_key = false;
  g_publish = true;
}

bool auth_needs_publish(void) {
  if (g_publish) return true;
  const uint32_t at = g_rotate_at;
  if (at && (int32_t)(now_ms() - at) >= 0) {
    esp_fill_random(g_next, sizeof(g_next));
    g_rotate_at = 0;
    g_publish = true;
    return true;
  }
  return false;
}

bool auth_publish(void) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  char key[33];
  hex(g_next, 16, key);
  char json[160];
  snprintf(json, sizeof(json),
           "{\"sn\":\"%s\",\"ble\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"key\":\"%s\"}",
           g_sn, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], key);
  uint8_t rec[200];
  const size_t n = ndef_json(json, rec, sizeof(rec));
  // nfc_write_ndef() reads every block back. Only a key that is on the
  // tag, readable, becomes the key in force.
  if (!n || !nfc_write_ndef(rec, n)) return false;
  portENTER_CRITICAL(&g_mux);
  memcpy(g_key, g_next, 16);
  g_have_key = true;
  g_publish = false;
  portEXIT_CRITICAL(&g_mux);
  return true;
}

void auth_new_nonce(uint8_t nonce[16]) { esp_fill_random(nonce, 16); }

bool auth_check(const uint8_t nonce[16], const char *proof_hex) {
  if (!proof_hex || strlen(proof_hex) != 64) return false;
  uint8_t key[16];
  bool have;
  portENTER_CRITICAL(&g_mux);
  memcpy(key, g_key, 16);
  have = g_have_key;
  portEXIT_CRITICAL(&g_mux);
  if (!have) return false;

  uint8_t mac[32];
  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 16,
                      nonce, 16, mac) != 0) {
    return false;
  }
  // Every byte compared whatever the outcome: the time taken must not
  // say how much of a guess was right.
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) {
    const int hi = unhex(proof_hex[2 * i]), lo = unhex(proof_hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    diff |= (uint8_t)(((hi << 4) | lo) ^ mac[i]);
  }
  return diff == 0;
}

void auth_session_ended(void) {
  const uint32_t at = now_ms() + AUTH_ROTATE_AFTER_MS;
  g_rotate_at = at ? at : 1;
}

void auth_key_hex(char out[33]) {
  portENTER_CRITICAL(&g_mux);
  hex(g_key, 16, out);
  portEXIT_CRITICAL(&g_mux);
}

bool auth_published(void) { return g_have_key; }
