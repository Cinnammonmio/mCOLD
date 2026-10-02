// Wi-Fi station: the box's way to the server (§10.4).
//
// The box knows up to NET_MAX networks -- a warehouse, a vehicle's
// hotspot, an office -- and joins the strongest one it can see. If that
// one refuses (a changed password, say), the next attempt tries another
// before coming back to it, so one bad entry cannot keep the box off the
// air while a good network is in range.
//
// Credentials live in NVS (namespace "net"), set from the console or by
// the app over an authorized BLE session -- never in the firmware image
// or the repository. With none set, the radio stays off.
//
// It says plainly what it has: joined and holding an address is not the
// same as reaching the server, and nothing here pretends otherwise.
#pragma once

#include <stdbool.h>
#include <stdint.h>

static const int NET_MAX = 5;

void net_start(void);

// Adds a network, or changes the password of one already known. False
// if it does not fit (SSID 1-32 bytes, password empty or 8-63) or the
// list is full.
bool net_add(const char *ssid, const char *pass);
bool net_remove(const char *ssid);
int net_count(void);
// The i-th known SSID (never the password), for listing.
bool net_known(int i, char *ssid, int n);
bool net_configured(void);

struct NetStatus {
  bool configured;
  bool connected;          // associated and holding an IPv4 address
  char ssid[33];           // the network joined, when connected
  char ip[16];
  int8_t rssi;
  uint32_t reconnects;
  uint32_t up_since_ms;    // 0 while down
  int known;               // networks in the list
};
void net_status(NetStatus *out);
