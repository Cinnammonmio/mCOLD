// The device protocol (PROTOCOL.md): JSON requests in, JSON responses
// out, the same over every transport.
//
// A transport hands over one complete request with the session it came
// in on. A session is authorized either by the transport -- the USB
// console always is, since whoever has the cable has the box -- or by
// AUTH: proof that the client read the key from the NFC tag (auth.h).
// Commands that change anything refuse a session that is not.
//
// Commands that change anything are idempotent by request id: the same
// id again returns the first answer and does nothing else, so a client
// that lost a response retries safely. START_TRIP's id survives a reset.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "power.h"

#include <stdint.h>

// One connection's worth of state. The transport creates it when the
// link opens (with a fresh nonce) and drops it when the link closes.
struct RpcSession {
  bool authorized;
  bool has_nonce;
  uint8_t nonce[16];
};

void rpc_init(const char *sn);

// Handles one request. Returns a malloc'd, NUL-terminated JSON response,
// never null; the caller frees it. AUTH may set s->authorized.
char *rpc_handle(const char *req, size_t n, RpcSession *s);

// Latest power reading, from the power task.
void rpc_note_power(const PowerStatus &ps);

// Protocol version this firmware speaks.
static const int RPC_PROTO = 1;
