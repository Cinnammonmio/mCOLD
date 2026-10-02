// The device protocol (PROTOCOL.md): JSON requests in, JSON responses
// out, the same over every transport.
//
// A transport hands over one complete request and says whether the link
// it came in on is authorized -- for BLE, encrypted and authenticated by
// pairing with the passkey shown on the e-paper; for the USB console,
// always, since whoever has the cable has the box. Commands that change
// anything refuse an unauthorized link.
//
// Commands that change anything are idempotent by request id: the same
// id again returns the first answer and does nothing else, so a client
// that lost a response retries safely. START_TRIP's id survives a reset.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "power.h"

void rpc_init(const char *sn);

// Handles one request. Returns a malloc'd, NUL-terminated JSON response,
// never null; the caller frees it.
char *rpc_handle(const char *req, size_t n, bool authorized);

// Latest power reading, from the power task.
void rpc_note_power(const PowerStatus &ps);

// Protocol version this firmware speaks.
static const int RPC_PROTO = 1;
