// BLE transport for the device protocol (PROTOCOL.md section 2).
//
// One GATT service: INFO and STATUS to read, COMMAND to write requests,
// RESPONSE, STATUS and EVENT notified back, every message cut into
// fragments that fit whatever MTU the phone negotiated. No pairing: a
// connection becomes authorized with AUTH, proving it read the key from
// the NFC tag (auth.h), and stays so until it closes.
//
// The radio advertises only while it has a reason to: for a window after
// a phone taps the NFC tag, and while the box is on external power. It
// does not start a trip, change a setting or wake anything by itself;
// it carries requests to rpc and answers back.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void ble_start(const char *sn);

// Advertise for `ms` (a phone tapped the tag, say). Extends a window
// already open; does nothing while connected.
void ble_window(uint32_t ms);

// Off: no advertising even on external power, and any link dropped.
// For the console and for power policy later.
void ble_enable(bool on);

struct BleStatus {
  bool enabled;
  bool advertising;
  bool connected;
  bool authorized;      // this connection passed AUTH
  uint16_t mtu;
  uint32_t requests;
};
void ble_status(BleStatus *out);

static const uint32_t BLE_TAP_WINDOW_MS = 60000;
