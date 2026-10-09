// The trip log: append-only records in the `trip_log` partition.
//
// Everything a trip measures goes through here before it goes anywhere
// else (§9.5: commit to flash first, then queue for upload). So the one
// property that matters above all others is that a power cut at any
// instant -- mid-record, mid-erase, mid-header -- loses at most the
// record being written, and never anything already committed.
//
// Layout. The partition is a ring of 4 KiB sectors (§9.3):
//
//   sector  = 64-byte header + 31 frames of 128 bytes + 64 bytes unused
//   header  = magic, format, sector sequence, trip id, CRC32
//   frame   = type, length, trip id, record sequence, up to 112 bytes of
//             payload, CRC32 over all of it
//
// How a write commits. A frame is programmed in one write and carries
// its own CRC. On the next boot a slot reads as one of three things:
// all 0xFF (never written), CRC good (committed), anything else (torn
// by a power cut). A torn slot is skipped, never reused, never
// reported as a record. There is deliberately no second "commit" write
// after the payload: with flash encryption on, the smallest write is 16
// bytes, so a one-byte marker cannot be programmed separately. Every
// write here is 16-byte aligned and a multiple of 16 long.
//
// Why one trip per sector. A trip is the unit that is deleted -- once
// the server has acknowledged it, or as a whole when space runs out
// (§9.6 forbids trimming records off several trips). A sector shared by
// two trips could not be erased for either. The cost is a part-filled
// sector at each trip boundary; §9.3 budgets for exactly that. A trip
// written to again after another has had the log carries on in its own
// last sector while that has room, so alternating costs nothing extra.
//
// Order. Each sector gets the next sector sequence number when it is
// opened, so the log's order survives wrap-around, and within a trip
// records carry their own sequence, which is what the server
// acknowledges against.
//
// Wear. Sectors are opened in rotation, so erases spread over the whole
// partition rather than hammering one spot.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The storage underneath: the real partition, or a RAM image in tests.
// Behaves like NOR flash -- writes can only clear bits, erase sets a
// whole sector to 0xFF.
struct FlashIf {
  uint32_t size;
  bool (*read)(void *ctx, uint32_t off, void *buf, uint32_t n);
  bool (*write)(void *ctx, uint32_t off, const void *buf, uint32_t n);
  bool (*erase)(void *ctx, uint32_t off, uint32_t n);   // whole sectors
  void *ctx;
  // Optional: the whole medium mapped into the address space, read-only,
  // for the boot scan. One read call per sector header costs 168 ms on
  // the real partition, which the box would pay on every wake from sleep;
  // through the cache the same scan is a few milliseconds. Returns null
  // if it cannot; `unmap` is then not called.
  const uint8_t *(*map)(void *ctx, void **handle);
  void (*unmap)(void *handle);
};

enum class LogErr : uint8_t {
  Ok = 0,
  NotReady,     // flashlog_init() not run, or it failed
  BadTrip,      // trip id 0 and 0xFFFFFFFF are reserved
  TooBig,       // payload over LOG_PAYLOAD_MAX
  Full,         // no free sector: something must be reclaimed first
  Flash,        // the flash refused a write or an erase
};

struct LogRecord {
  uint32_t trip;
  uint32_t seq;            // per trip, from 0, gapless unless a write tore
  uint8_t type;
  uint8_t len;
  const uint8_t *payload;  // valid only during the callback
};

// Return false to stop the walk.
typedef bool (*LogVisitor)(const LogRecord &r, void *ctx);

struct LogStats {
  uint32_t sectors;        // in the partition
  uint32_t free;           // erased or reusable
  uint32_t used;           // holding a trip's records
  uint32_t dirty;          // header unreadable: torn mid-open, reusable
  uint32_t trips;          // distinct trips with at least one sector
  uint32_t head_trip;      // trip of the sector being written, 0 if none
  uint32_t next_sector_seq;
};

// Scans every sector header, finds where writing left off, and is
// ready. The flash interface must outlive the log.
bool flashlog_init(const FlashIf *flash);

// Appends one record to `trip` and returns its sequence number. Opens a
// fresh sector when the current one is full or belongs to another trip.
// When this returns Ok the record is on flash; when it returns anything
// else, it is not, and the sequence number was not consumed.
LogErr flashlog_append(uint32_t trip, uint8_t type, const void *payload,
                       size_t n, uint32_t *seq_out);

// Every committed record of `trip` with seq >= from_seq, in order.
// Returns the number visited. Torn slots are counted into `torn` if
// given, never visited.
uint32_t flashlog_read(uint32_t trip, uint32_t from_seq, LogVisitor v,
                       void *ctx, uint32_t *torn);

// The highest committed sequence of `trip`. False if it has none.
bool flashlog_last_seq(uint32_t trip, uint32_t *seq);

// Trips present, oldest first (by their first sector). Returns count.
int flashlog_trips(uint32_t *out, int max);

// Erases every sector of `trip`. Deciding WHEN that is allowed -- the
// server has acknowledged it, or it is the oldest completed trip and
// space has run out -- is the caller's business (§9.6); this only does
// it. Interrupted, it leaves a newer contiguous part of the trip.
LogErr flashlog_erase_trip(uint32_t trip);

void flashlog_stats(LogStats *out);

const char *log_err_name(LogErr e);

// Geometry, fixed by the format.
static const uint32_t LOG_SECTOR = 4096;
static const uint32_t LOG_HEADER = 64;
static const uint32_t LOG_FRAME = 128;
static const uint32_t LOG_SLOTS = 31;
static const uint32_t LOG_PAYLOAD_MAX = LOG_FRAME - 16;   // 112

// Trip ids that never name a trip.
static const uint32_t LOG_TRIP_NONE = 0;
static const uint32_t LOG_TRIP_INVALID = 0xFFFFFFFF;

// The trip_log partition as a FlashIf. False if it is not in the
// partition table.
bool flashlog_partition(FlashIf *out);
