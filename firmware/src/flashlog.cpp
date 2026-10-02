#include "flashlog.h"

#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---- on-flash format, little-endian, never a C struct (§9.4) --------

const uint32_t MAGIC = 0x314C434D;     // "MCL1"
const uint16_t FORMAT = 1;
const uint8_t FRAME_FORMAT = 1;

// Header: magic(4) format(2) rsvd(2) sector_seq(4) trip(4) rsvd(44) crc(4)
const uint32_t H_MAGIC = 0, H_FORMAT = 4, H_SEQ = 8, H_TRIP = 12,
               H_CRC = LOG_HEADER - 4;

// Frame: type(1) len(1) format(1) rsvd(1) trip(4) seq(4) payload(112) crc(4)
const uint32_t F_TYPE = 0, F_LEN = 1, F_FORMAT = 2, F_TRIP = 4, F_SEQ = 8,
               F_PAYLOAD = 12, F_CRC = LOG_FRAME - 4;
const uint8_t TYPE_EMPTY = 0xFF;

static_assert(LOG_HEADER + LOG_SLOTS * LOG_FRAME <= LOG_SECTOR, "geometry");
static_assert(F_PAYLOAD + LOG_PAYLOAD_MAX == F_CRC, "frame layout");
static_assert(LOG_HEADER % 16 == 0 && LOG_FRAME % 16 == 0,
              "every write 16-byte aligned, for flash encryption");

void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
uint32_t crc(const uint8_t *p, uint32_t n) { return esp_rom_crc32_le(0, p, n); }

bool all_ff(const uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (p[i] != 0xFF) return false;
  }
  return true;
}

// ---- in-RAM index: one entry per sector ------------------------------

enum class Sec : uint8_t { Free, Used, Dirty };

struct SecInfo {
  uint32_t seq;
  uint32_t trip;
  Sec state;
};

const FlashIf *g_flash = nullptr;
SecInfo *g_sec = nullptr;
uint32_t g_count = 0;
uint32_t g_next_sector_seq = 1;
int32_t g_head = -1;              // sector being written, -1: none
uint32_t g_head_slot = 0;         // next slot to write in it
int32_t g_newest = -1;            // highest sector sequence: rotation anchor

// Next sequence for the trip last appended to, so the common case does
// not rescan flash for every record.
uint32_t g_seq_trip = LOG_TRIP_NONE;
uint32_t g_seq_next = 0;

SemaphoreHandle_t g_lock = nullptr;
uint8_t *g_buf = nullptr;         // one sector, for scans

struct Lock {
  Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_lock); }
};

uint32_t sector_off(uint32_t i) { return i * LOG_SECTOR; }
uint32_t slot_off(uint32_t i, uint32_t s) {
  return sector_off(i) + LOG_HEADER + s * LOG_FRAME;
}

bool valid_trip(uint32_t t) {
  return t != LOG_TRIP_NONE && t != LOG_TRIP_INVALID;
}

Sec classify_header(const uint8_t *h, uint32_t *seq, uint32_t *trip) {
  if (all_ff(h, LOG_HEADER)) return Sec::Free;
  if (get32(h + H_MAGIC) != MAGIC) return Sec::Dirty;
  if ((uint16_t)(h[H_FORMAT] | h[H_FORMAT + 1] << 8) != FORMAT) return Sec::Dirty;
  if (get32(h + H_CRC) != crc(h, H_CRC)) return Sec::Dirty;
  *seq = get32(h + H_SEQ);
  *trip = get32(h + H_TRIP);
  return valid_trip(*trip) ? Sec::Used : Sec::Dirty;
}

enum class Slot : uint8_t { Empty, Valid, Torn };

Slot classify_frame(const uint8_t *f, uint32_t trip) {
  if (all_ff(f, LOG_FRAME)) return Slot::Empty;
  if (get32(f + F_CRC) != crc(f, F_CRC)) return Slot::Torn;
  // A good CRC on a frame naming another trip, or an impossible length,
  // is still not a record of this trip. It cannot happen through this
  // code; it is checked so that a bug elsewhere cannot leak records
  // from one trip into another's upload.
  if (f[F_FORMAT] != FRAME_FORMAT || f[F_TYPE] == TYPE_EMPTY ||
      f[F_LEN] > LOG_PAYLOAD_MAX || get32(f + F_TRIP) != trip) {
    return Slot::Torn;
  }
  return Slot::Valid;
}

// Reads a whole sector into g_buf.
bool load_sector(uint32_t i) {
  return g_flash->read(g_flash->ctx, sector_off(i), g_buf, LOG_SECTOR);
}

// The first never-written slot of a used sector. Writes go in slot
// order, so everything after the last written slot is empty.
uint32_t first_empty_slot(uint32_t i) {
  if (!load_sector(i)) return LOG_SLOTS;          // unreadable: treat as full
  for (int s = (int)LOG_SLOTS - 1; s >= 0; s--) {
    if (!all_ff(g_buf + LOG_HEADER + s * LOG_FRAME, LOG_FRAME)) {
      return (uint32_t)s + 1;
    }
  }
  return 0;
}

// The newest sector of `trip`, or of any trip for LOG_TRIP_NONE.
int32_t newest_of(uint32_t trip) {
  int32_t pick = -1;
  for (uint32_t i = 0; i < g_count; i++) {
    if (g_sec[i].state != Sec::Used) continue;
    if (trip != LOG_TRIP_NONE && g_sec[i].trip != trip) continue;
    if (pick < 0 || g_sec[i].seq > g_sec[pick].seq) pick = (int32_t)i;
  }
  return pick;
}

// After boot or an erase: writing resumes in the newest sector.
void find_head(void) {
  g_newest = newest_of(LOG_TRIP_NONE);
  g_head = g_newest;
  g_head_slot = g_head >= 0 ? first_empty_slot((uint32_t)g_head) : 0;
}

// Point the head at `trip`'s newest sector if it still has room. A trip
// that is written again after another one has had the head -- a summary
// record after the next trip has begun -- carries on where it left off
// instead of starting a sector for one record each time.
bool resume(uint32_t trip) {
  const int32_t i = newest_of(trip);
  if (i < 0) return false;
  const uint32_t slot = first_empty_slot((uint32_t)i);
  if (slot >= LOG_SLOTS) return false;
  g_head = i;
  g_head_slot = slot;
  return true;
}

// Sectors of `trip`, oldest first. Returns count; `out` from malloc.
uint32_t sectors_of(uint32_t trip, uint16_t **out) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < g_count; i++) {
    if (g_sec[i].state == Sec::Used && g_sec[i].trip == trip) n++;
  }
  *out = nullptr;
  if (!n) return 0;
  uint16_t *v = (uint16_t *)malloc(n * sizeof(uint16_t));
  if (!v) return 0;
  uint32_t k = 0;
  for (uint32_t i = 0; i < g_count; i++) {
    if (g_sec[i].state == Sec::Used && g_sec[i].trip == trip) v[k++] = (uint16_t)i;
  }
  // Insertion sort by sector sequence: n is at most a few thousand and
  // usually already nearly in order.
  for (uint32_t a = 1; a < n; a++) {
    const uint16_t x = v[a];
    uint32_t b = a;
    while (b > 0 && g_sec[v[b - 1]].seq > g_sec[x].seq) {
      v[b] = v[b - 1];
      b--;
    }
    v[b] = x;
  }
  *out = v;
  return n;
}

bool last_seq_locked(uint32_t trip, uint32_t *seq) {
  uint16_t *v;
  const uint32_t n = sectors_of(trip, &v);
  bool found = false;
  // Newest sector first; one with no committed frame (power cut right
  // after its header) sends us back to the one before.
  for (int32_t k = (int32_t)n - 1; k >= 0 && !found; k--) {
    if (!load_sector(v[k])) continue;
    for (int s = (int)LOG_SLOTS - 1; s >= 0; s--) {
      const uint8_t *f = g_buf + LOG_HEADER + s * LOG_FRAME;
      if (classify_frame(f, trip) == Slot::Valid) {
        *seq = get32(f + F_SEQ);
        found = true;
        break;
      }
    }
  }
  free(v);
  return found;
}

// Erase a free or dirty sector and claim it for `trip`.
LogErr open_sector(uint32_t trip) {
  // Rotation: start after the current head, so erases walk the whole
  // partition instead of reusing the first free sector every time.
  const uint32_t start = g_newest >= 0 ? (uint32_t)g_newest + 1 : 0;
  int32_t pick = -1;
  for (uint32_t k = 0; k < g_count; k++) {
    const uint32_t i = (start + k) % g_count;
    if (g_sec[i].state != Sec::Used) {
      pick = (int32_t)i;
      break;
    }
  }
  if (pick < 0) return LogErr::Full;
  const uint32_t i = (uint32_t)pick;

  if (!g_flash->erase(g_flash->ctx, sector_off(i), LOG_SECTOR)) {
    g_sec[i].state = Sec::Dirty;
    return LogErr::Flash;
  }
  uint8_t h[LOG_HEADER];
  memset(h, 0, sizeof(h));
  put32(h + H_MAGIC, MAGIC);
  h[H_FORMAT] = (uint8_t)FORMAT;
  h[H_FORMAT + 1] = (uint8_t)(FORMAT >> 8);
  put32(h + H_SEQ, g_next_sector_seq);
  put32(h + H_TRIP, trip);
  put32(h + H_CRC, crc(h, H_CRC));
  if (!g_flash->write(g_flash->ctx, sector_off(i), h, LOG_HEADER)) {
    g_sec[i].state = Sec::Dirty;
    return LogErr::Flash;
  }
  g_sec[i] = {g_next_sector_seq, trip, Sec::Used};
  g_next_sector_seq++;
  g_newest = g_head = (int32_t)i;
  g_head_slot = 0;
  return LogErr::Ok;
}

}  // namespace

bool flashlog_init(const FlashIf *flash) {
  if (!g_lock) g_lock = xSemaphoreCreateMutex();
  Lock l;
  g_flash = nullptr;
  free(g_sec);
  g_sec = nullptr;
  if (!flash || flash->size < LOG_SECTOR) return false;

  g_count = flash->size / LOG_SECTOR;
  // PSRAM if there is any: the index is 12 bytes a sector, 30 KiB for
  // the real partition, and internal RAM is the scarcer of the two.
  g_sec = (SecInfo *)heap_caps_malloc(g_count * sizeof(SecInfo),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_sec) g_sec = (SecInfo *)malloc(g_count * sizeof(SecInfo));
  if (!g_buf) g_buf = (uint8_t *)malloc(LOG_SECTOR);
  if (!g_sec || !g_buf) return false;
  g_flash = flash;

  uint32_t max_seq = 0;
  uint8_t h[LOG_HEADER];
  void *mh = nullptr;
  const uint8_t *mapped = flash->map ? flash->map(flash->ctx, &mh) : nullptr;
  for (uint32_t i = 0; i < g_count; i++) {
    uint32_t seq = 0, trip = 0;
    Sec st = Sec::Dirty;
    if (mapped) {
      memcpy(h, mapped + sector_off(i), LOG_HEADER);
      st = classify_header(h, &seq, &trip);
    } else if (flash->read(flash->ctx, sector_off(i), h, LOG_HEADER)) {
      st = classify_header(h, &seq, &trip);
    }
    g_sec[i] = {seq, trip, st};
    if (st == Sec::Used && seq > max_seq) max_seq = seq;
  }
  if (mapped) flash->unmap(mh);
  g_next_sector_seq = max_seq + 1;
  g_seq_trip = LOG_TRIP_NONE;
  find_head();
  return true;
}

LogErr flashlog_append(uint32_t trip, uint8_t type, const void *payload,
                       size_t n, uint32_t *seq_out) {
  if (!g_lock) return LogErr::NotReady;
  Lock l;
  if (!g_flash) return LogErr::NotReady;
  if (!valid_trip(trip) || type == TYPE_EMPTY) return LogErr::BadTrip;
  if (n > LOG_PAYLOAD_MAX) return LogErr::TooBig;

  if (g_seq_trip != trip) {
    uint32_t last;
    g_seq_next = last_seq_locked(trip, &last) ? last + 1 : 0;
    g_seq_trip = trip;
  }

  const bool head_fits =
      g_head >= 0 && g_sec[g_head].trip == trip && g_head_slot < LOG_SLOTS;
  if (!head_fits && !resume(trip)) {
    const LogErr e = open_sector(trip);
    if (e != LogErr::Ok) return e;
  }

  uint8_t f[LOG_FRAME];
  memset(f, 0, sizeof(f));
  f[F_TYPE] = type;
  f[F_LEN] = (uint8_t)n;
  f[F_FORMAT] = FRAME_FORMAT;
  put32(f + F_TRIP, trip);
  put32(f + F_SEQ, g_seq_next);
  if (n) memcpy(f + F_PAYLOAD, payload, n);
  put32(f + F_CRC, crc(f, F_CRC));

  const uint32_t at = slot_off((uint32_t)g_head, g_head_slot);
  // The slot is spent whether or not the write succeeded: a failed
  // write may have programmed part of it, and NOR flash cannot be
  // programmed again without an erase.
  g_head_slot++;
  if (!g_flash->write(g_flash->ctx, at, f, LOG_FRAME)) return LogErr::Flash;

  if (seq_out) *seq_out = g_seq_next;
  g_seq_next++;
  return LogErr::Ok;
}

uint32_t flashlog_read(uint32_t trip, uint32_t from_seq, LogVisitor v,
                       void *ctx, uint32_t *torn) {
  if (torn) *torn = 0;
  if (!g_lock) return 0;
  Lock l;
  if (!g_flash) return 0;

  uint16_t *secs;
  const uint32_t n = sectors_of(trip, &secs);
  uint32_t visited = 0;
  bool stop = false;
  for (uint32_t k = 0; k < n && !stop; k++) {
    if (!load_sector(secs[k])) continue;
    for (uint32_t s = 0; s < LOG_SLOTS && !stop; s++) {
      const uint8_t *f = g_buf + LOG_HEADER + s * LOG_FRAME;
      switch (classify_frame(f, trip)) {
        case Slot::Empty:
          break;
        case Slot::Torn:
          if (torn) (*torn)++;
          break;
        case Slot::Valid: {
          const uint32_t seq = get32(f + F_SEQ);
          if (seq < from_seq) break;
          LogRecord r = {trip, seq, f[F_TYPE], f[F_LEN], f + F_PAYLOAD};
          visited++;
          if (v && !v(r, ctx)) stop = true;
          break;
        }
      }
    }
  }
  free(secs);
  return visited;
}

bool flashlog_last_seq(uint32_t trip, uint32_t *seq) {
  if (!g_lock) return false;
  Lock l;
  if (!g_flash) return false;
  return last_seq_locked(trip, seq);
}

int flashlog_trips(uint32_t *out, int max) {
  if (!g_lock) return 0;
  Lock l;
  if (!g_flash) return 0;
  // (trip, oldest sector seq) pairs, built by a linear merge per sector.
  struct T { uint32_t trip, first; };
  T *t = (T *)malloc(g_count * sizeof(T));
  if (!t) return 0;
  int n = 0;
  for (uint32_t i = 0; i < g_count; i++) {
    if (g_sec[i].state != Sec::Used) continue;
    int j = 0;
    while (j < n && t[j].trip != g_sec[i].trip) j++;
    if (j == n) t[n++] = {g_sec[i].trip, g_sec[i].seq};
    else if (g_sec[i].seq < t[j].first) t[j].first = g_sec[i].seq;
  }
  for (int a = 1; a < n; a++) {
    const T x = t[a];
    int b = a;
    while (b > 0 && t[b - 1].first > x.first) {
      t[b] = t[b - 1];
      b--;
    }
    t[b] = x;
  }
  const int k = n < max ? n : max;
  for (int i = 0; i < k; i++) out[i] = t[i].trip;
  free(t);
  return n;
}

LogErr flashlog_erase_trip(uint32_t trip) {
  if (!g_lock) return LogErr::NotReady;
  Lock l;
  if (!g_flash) return LogErr::NotReady;
  if (!valid_trip(trip)) return LogErr::BadTrip;

  uint16_t *secs;
  const uint32_t n = sectors_of(trip, &secs);
  LogErr e = LogErr::Ok;
  // Oldest first. Cut short, what is left is the trip's newest part,
  // still in order, still readable.
  for (uint32_t k = 0; k < n; k++) {
    if (!g_flash->erase(g_flash->ctx, sector_off(secs[k]), LOG_SECTOR)) {
      g_sec[secs[k]].state = Sec::Dirty;
      e = LogErr::Flash;
      continue;
    }
    g_sec[secs[k]].state = Sec::Free;
  }
  free(secs);
  if (g_seq_trip == trip) g_seq_trip = LOG_TRIP_NONE;
  find_head();
  return e;
}

void flashlog_stats(LogStats *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!g_lock) return;
  Lock l;
  if (!g_flash) return;
  out->sectors = g_count;
  for (uint32_t i = 0; i < g_count; i++) {
    switch (g_sec[i].state) {
      case Sec::Free:  out->free++; break;
      case Sec::Used:  out->used++; break;
      case Sec::Dirty: out->dirty++; break;
    }
  }
  // Distinct trips, counted at the first sector of each.
  for (uint32_t i = 0; i < g_count; i++) {
    if (g_sec[i].state != Sec::Used) continue;
    bool seen = false;
    for (uint32_t j = 0; j < i && !seen; j++) {
      seen = g_sec[j].state == Sec::Used && g_sec[j].trip == g_sec[i].trip;
    }
    if (!seen) out->trips++;
  }
  out->head_trip = g_head >= 0 ? g_sec[g_head].trip : LOG_TRIP_NONE;
  out->next_sector_seq = g_next_sector_seq;
}

const char *log_err_name(LogErr e) {
  switch (e) {
    case LogErr::Ok:       return "ok";
    case LogErr::NotReady: return "not ready";
    case LogErr::BadTrip:  return "bad trip id";
    case LogErr::TooBig:   return "payload too big";
    case LogErr::Full:     return "log full";
    case LogErr::Flash:    return "flash error";
  }
  return "?";
}

// ---- the real partition ------------------------------------------------

namespace {

bool p_read(void *c, uint32_t off, void *buf, uint32_t n) {
  return esp_partition_read((const esp_partition_t *)c, off, buf, n) == ESP_OK;
}
bool p_write(void *c, uint32_t off, const void *buf, uint32_t n) {
  return esp_partition_write((const esp_partition_t *)c, off, buf, n) == ESP_OK;
}
bool p_erase(void *c, uint32_t off, uint32_t n) {
  return esp_partition_erase_range((const esp_partition_t *)c, off, n) == ESP_OK;
}
const uint8_t *p_map(void *c, void **handle) {
  const esp_partition_t *p = (const esp_partition_t *)c;
  const void *ptr = nullptr;
  esp_partition_mmap_handle_t h;
  if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &ptr, &h) != ESP_OK) {
    return nullptr;
  }
  *handle = (void *)(uintptr_t)h;
  return (const uint8_t *)ptr;
}
void p_unmap(void *handle) {
  esp_partition_munmap((esp_partition_mmap_handle_t)(uintptr_t)handle);
}

}  // namespace

bool flashlog_partition(FlashIf *out) {
  const esp_partition_t *p = esp_partition_find_first(
      (esp_partition_type_t)0x40, (esp_partition_subtype_t)0x01, "trip_log");
  if (!p || !out) return false;
  out->size = p->size;
  out->read = p_read;
  out->write = p_write;
  out->erase = p_erase;
  out->map = p_map;
  out->unmap = p_unmap;
  out->ctx = (void *)p;
  return true;
}
