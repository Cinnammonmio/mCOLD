// Tap to authorize (decided 2026-10-02): whoever taps the box with a
// phone may command it, for one session.
//
// The box keeps a random 16-byte key and publishes it in its NFC tag,
// inside the JSON record the app reads anyway:
//
//   {"sn":"MCOLD-9A74","ble":"28:84:85:27:9A:76","key":"<32 hex>"}
//
// NFC reads at a few centimetres, so reading the key takes the box in
// hand -- the same proof a passkey on the display gave, without anyone
// typing anything, and without depending on a display that may take 25 s
// to show a code.
//
// The key itself never goes over BLE. Each connection gets a fresh
// random nonce (in INFO); the app proves it holds the key by sending
// HMAC-SHA256(key, nonce). Someone listening to the radio sees a nonce
// and a proof that are useless on any other connection.
//
// The key is replaced at every reset, and 2 minutes after a session that
// used it ends: one tap, one session. The new key is written to the tag
// first and used only once it has read back correctly, so the tag never
// holds a key the box would refuse.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void auth_init(const char *sn);

// The NFC side, called from the task that owns the tag: true when the
// tag needs (re)writing. auth_publish() does it; false if the tag
// refused (a phone holding it, say) -- try again later.
bool auth_needs_publish(void);
bool auth_publish(void);

// A fresh nonce for a new connection: 16 random bytes, and as hex.
void auth_new_nonce(uint8_t nonce[16]);

// True if `proof_hex` is HMAC-SHA256(key, nonce) in hex. Constant time.
bool auth_check(const uint8_t nonce[16], const char *proof_hex);

// A session that was authorized has ended: replace the key after the
// grace period (a dropped link may want to come straight back).
void auth_session_ended(void);

// For the console: the current key as hex, and whether the tag has it.
void auth_key_hex(char out[33]);
bool auth_published(void);

static const uint32_t AUTH_ROTATE_AFTER_MS = 120000;
