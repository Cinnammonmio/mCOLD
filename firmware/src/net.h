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
//
// The radio is up only while someone wants it (net_want). On USB power
// that is always; on battery it is the uplink, for one short session per
// upload period (P7). The driver itself is not even initialised until
// the first time it is wanted, so a wake with nothing to send costs no
// Wi-Fi at all. The network last joined is remembered through deep
// sleep and joined directly, without a scan: two seconds of radio saved
// on every session.
#pragma once

#include <stdbool.h>
#include <stdint.h>

static const int NET_MAX = 5;

void net_start(void);

// Radio on (join a known network) or off. Safe to call every pass.
void net_want(bool on);
// Before deep sleep: the radio off cleanly.
void net_stop(void);

// Adds a network, or changes the password of one already known. False
// if it does not fit (SSID 1-32 bytes, password empty or 8-63) or the
// list is full.
bool net_add(const char *ssid, const char *pass);
bool net_remove(const char *ssid);

// The list is five fixed slots, 0..4 here (1..5 on the wire): the app
// edits or clears a slot by its place. An empty slot has no ssid.
// net_slot_put: the ssid and its password in that slot; pass null keeps
// the password, which only an unchanged ssid has (a new name needs one).
// A changed ssid starts on DHCP again; no two slots hold one network.
bool net_slot_ssid(int slot, char *ssid, int n);
bool net_slot_put(int slot, const char *ssid, const char *pass);
bool net_slot_clear(int slot);

// How a network gives the box its address, per network as eTEMP has it
// (decided 2026-10-05): DHCP, or a fixed address with its gateway, subnet
// mask and DNS server. Dotted quads; dns may be empty (then the gateway).
struct NetIp {
  bool dhcp;
  char ip[16], gateway[16], subnet[16], dns[16];
};
// False for a network not in the list, or an address that does not parse
// (or a gateway outside the subnet). Takes effect at the next join.
bool net_set_ip(const char *ssid, const NetIp &ip);
bool net_get_ip(int slot, NetIp *out);    // by slot, 0..4; false if empty
int net_count(void);     // how many slots are in use
// The i-th SSID that is in use (never the password), for listing.
bool net_known(int i, char *ssid, int n);
bool net_configured(void);

struct NetStatus {
  bool configured;
  bool connected;          // associated and holding an IPv4 address
  char ssid[33];           // the network joined, when connected
  char ip[16];
  char gateway[16], subnet[16], dns[16];   // the lease or the fixed address, when connected
  char mac[18];                            // this box's Wi-Fi MAC, AA:BB:CC:DD:EE:FF
  bool dhcp;                               // the network gives the address (not a fixed one)
  int8_t rssi;
  uint32_t reconnects;
  uint32_t up_since_ms;    // 0 while down
  uint32_t last_up_ms;     // mono_ms() the last session got an address; 0: never
  int known;               // networks in the list
};
void net_status(NetStatus *out);
