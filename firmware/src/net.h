// Wi-Fi station: the box's way to the server (§10.4).
//
// Credentials live in NVS (namespace "net"), set from the console or by
// the app over an authorized BLE session -- never in the firmware image
// or the repository. With none set, the radio stays off.
//
// It reconnects on its own with backoff, and says plainly what it has:
// connected to the access point and holding an address is not the same
// as reaching the server, and nothing here pretends otherwise.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void net_start(void);

// Stores new credentials and reconnects with them. False if they do not
// fit (SSID 1-32 bytes, password 0 or 8-63).
bool net_set(const char *ssid, const char *pass);
bool net_configured(void);

struct NetStatus {
  bool configured;
  bool connected;          // associated and holding an IPv4 address
  char ssid[33];
  char ip[16];
  int8_t rssi;
  uint32_t reconnects;
  uint32_t up_since_ms;    // 0 while down
};
void net_status(NetStatus *out);
