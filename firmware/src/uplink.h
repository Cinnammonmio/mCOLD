// Uploading the trip log to the server over MQTT, and knowing what the
// server has really got (§9.5).
//
// Records go out in batches, oldest first, straight from the trip log --
// they were committed to flash before anything else saw them. A batch is
// delivered when the SERVER says so on the ack topic, not when the
// broker says so: an MQTT PUBACK only proves the broker took the message,
// and §9.5 forbids treating that as stored.
//
// The server's ACK is a high-water mark per trip: "every record of trip T
// up to and including seq S is stored". It must be contiguous -- no gaps
// -- and the device checks it against what is in its own log, so a
// malformed or misdirected ACK cannot mark records stored that were never
// sent. The mark lives in NVS and survives resets; a batch left without
// an answer is simply sent again, and the server must treat (trip, seq)
// as the idempotency key.
//
// Topics, with <sn> the device SN (PROTOCOL.md section 6):
//
//   mcold/<sn>/rec      device -> server   record batches, QoS 1
//   mcold/<sn>/ack      server -> device   {"trip":T,"upto":S}
//   mcold/<sn>/status   device -> server   GET_STATUS, retained
//   mcold/<sn>/online   device -> server   "1" / "0" (last will), retained
//
// Broker and login are in NVS (namespace "mqtt"), never in the image.
//
// On USB power the connection stays up. On battery (P7) it is a session:
// when records are waiting and upload_period_s has passed, Wi-Fi comes
// up, the status and as many batches as the server ACKs within the
// session go out, "0" is published on the online topic, and the radio
// goes off again until the next one. A session that gets no ACK --
// broker out of reach, or a server that does not answer -- doubles the
// wait before the next, up to four hours, so the battery is not spent
// on a conversation that is not happening.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void uplink_start(const char *sn);

bool uplink_set_server(const char *host, uint16_t port, const char *user,
                       const char *pass);
bool uplink_configured(void);

// Records of `trip` the server has confirmed: seq < this are stored.
uint32_t uplink_acked(uint32_t trip);

// True once the server has every record of `trip` that is in the log.
bool uplink_fully_acked(uint32_t trip);

// The trip was deleted from the log: drop its high-water mark.
void uplink_forget(uint32_t trip);

// Upload now rather than at the next pass.
void uplink_kick(void);

struct UplinkStatus {
  bool configured;
  bool connected;          // to the broker; says nothing about the server
  char host[64];
  uint16_t port;
  uint32_t batches_sent;
  uint32_t acks;           // application ACKs accepted
  uint32_t acks_rejected;  // malformed, unknown trip, or beyond the log
  uint32_t records_pending;
  uint32_t last_ack_ms;    // mono_ms(); 0: never
  // Battery sessions
  uint32_t sessions;
  bool last_session_ok;    // reached the broker
  uint32_t next_session_ms;   // mono_ms() the next is due; 0: none planned
};
void uplink_status(UplinkStatus *out);

static const int UPLINK_BATCH = 16;              // records per batch
static const uint32_t UPLINK_ACK_TIMEOUT_MS = 15000;     // first wait for an ACK
static const uint32_t UPLINK_ACK_WAIT_MAX_MS = 300000;   // the longest, when silent

// Hand an ACK in as though the server had sent it: for the bench, before
// the server side exists. It goes through every check a real one does.
void uplink_inject_ack(const char *json);
