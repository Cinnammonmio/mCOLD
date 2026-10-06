// What came in from a phone, for the glass (config rx_show, a bench aid,
// never in the product; 2026-10-06).
//
// Testing tap-then-connect with the app, the screen shows the last few
// events -- the NFC tap, BLE connecting and going, every command with
// what it carried and how it was answered -- so whoever holds the phone
// sees what the box received without a cable or a console. Secrets are
// masked: a Wi-Fi password, the AUTH proof, the key.
//
// A command the app sends again and again (GET_STATUS, READ_LOG_CHUNK)
// is one line with a count, not a screen full of the same line.
#pragma once

#include <stdint.h>

#include "rpc.h"

static const int RXLOG_LINES = 7;    // what fits under the header
static const int RXLOG_W = 40;       // characters a line holds, at most

void rxlog_note(const char *what);   // "NFC tap", "BLE connected", ...

// One command: the request as received and the response as sent.
void rxlog_rpc(const char *req, size_t n, const char *resp, const RpcSession *s);

// Changes with every entry, so the display knows when to redraw.
uint32_t rxlog_gen(void);

// The newest entries that fit in `max` lines, wrapped, oldest first.
int rxlog_render(char out[][RXLOG_W + 1], int max);
