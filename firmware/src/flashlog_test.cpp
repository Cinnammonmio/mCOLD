// Power-cut tests for the trip log, run on the device itself.
//
// The log is only as good as its behaviour when the power goes at the
// worst moment, and that cannot be tested by pulling a cable a few
// hundred times. So the log is run against a RAM image that behaves as
// NOR flash does -- programming only clears bits, erase sets a sector
// to 0xFF -- and that can "lose power" after any given number of bytes.
// A test then "reboots" by scanning the same image again, exactly as
// the firmware does at power-up, and checks what survived.
//
// The log module is a single instance. These tests take it over, so
// flashlog_selftest() puts the real partition back when it finishes;
// nothing else may be writing to the log while they run.
#include "flashlog_test.h"

#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flashlog.h"

namespace {

// ---- a NOR flash in RAM that can lose power ------------------------

struct RamFlash {
  uint8_t *mem;
  uint32_t size;
  int64_t budget;      // bytes that may still be written; -1: unlimited
  bool dead;           // power is off: every operation fails
  bool tear_erase;     // the next erase stops halfway
  uint32_t writes, erases;
};

bool r_read(void *c, uint32_t off, void *buf, uint32_t n) {
  RamFlash *f = (RamFlash *)c;
  if (f->dead || off + n > f->size) return false;
  memcpy(buf, f->mem + off, n);
  return true;
}

bool r_write(void *c, uint32_t off, const void *buf, uint32_t n) {
  RamFlash *f = (RamFlash *)c;
  if (f->dead || off + n > f->size) return false;
  uint32_t k = n;
  if (f->budget >= 0 && (int64_t)n > f->budget) {
    k = (uint32_t)f->budget;     // the power goes partway through
    f->dead = true;
  }
  const uint8_t *s = (const uint8_t *)buf;
  for (uint32_t i = 0; i < k; i++) f->mem[off + i] &= s[i];   // NOR: 1 -> 0 only
  if (f->budget >= 0) f->budget -= k;
  f->writes++;
  return !f->dead;
}

bool r_erase(void *c, uint32_t off, uint32_t n) {
  RamFlash *f = (RamFlash *)c;
  if (f->dead || off % LOG_SECTOR || n % LOG_SECTOR || off + n > f->size) {
    return false;
  }
  f->erases++;
  if (f->tear_erase) {
    // An erase cut short leaves the sector neither old nor erased. Here:
    // header untouched, the back half of the frames wiped.
    memset(f->mem + off + LOG_SECTOR / 2, 0xFF, LOG_SECTOR / 2);
    f->dead = true;
    f->tear_erase = false;
    return false;
  }
  memset(f->mem + off, 0xFF, n);
  return true;
}

RamFlash g_ram;
FlashIf g_if;

void power_on(void) {          // "reboot": same image, power restored
  g_ram.dead = false;
  g_ram.budget = -1;
  g_ram.tear_erase = false;
  flashlog_init(&g_if);
}

void blank(uint32_t sectors) {
  g_ram.size = sectors * LOG_SECTOR;
  memset(g_ram.mem, 0xFF, g_ram.size);
  g_if.size = g_ram.size;
  power_on();
}

// ---- helpers ---------------------------------------------------------

int g_fail = 0;
const char *g_test = "";

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      printf("    FAIL %s: %s (line %d)\n", g_test, #cond, __LINE__);    \
      g_fail++;                                                          \
      return;                                                            \
    }                                                                    \
  } while (0)

// The payload of record `seq` of `trip`: recognisable, and different for
// every record, so a record read back from the wrong place shows.
void make(uint32_t trip, uint32_t seq, uint8_t *p, uint8_t *n) {
  *n = (uint8_t)(8 + (seq * 7) % (LOG_PAYLOAD_MAX - 8));
  for (uint8_t i = 0; i < *n; i++) p[i] = (uint8_t)(trip * 31 + seq * 13 + i);
}

LogErr add(uint32_t trip, uint32_t *seq) {
  uint32_t expect = 0;
  uint32_t last;
  if (flashlog_last_seq(trip, &last)) expect = last + 1;
  uint8_t p[LOG_PAYLOAD_MAX], n;
  make(trip, expect, p, &n);
  return flashlog_append(trip, 0x10, p, n, seq);
}

struct Check {
  uint32_t trip;
  uint32_t next;       // the sequence the next record must have
  bool ok;
};

bool verify_visit(const LogRecord &r, void *ctx) {
  Check *c = (Check *)ctx;
  uint8_t p[LOG_PAYLOAD_MAX], n;
  make(r.trip, r.seq, p, &n);
  if (r.seq != c->next || r.len != n || memcmp(r.payload, p, n) || r.type != 0x10) {
    c->ok = false;
    return false;
  }
  c->next++;
  return true;
}

// Content only, gaps allowed.
bool content_visit(const LogRecord &r, void *ctx) {
  uint8_t p[LOG_PAYLOAD_MAX], n;
  make(r.trip, r.seq, p, &n);
  if (r.len != n || memcmp(r.payload, p, n)) *(bool *)ctx = false;
  return true;
}

// Every record 0..count-1 of `trip`, in order, gapless, intact.
bool intact(uint32_t trip, uint32_t count) {
  Check c = {trip, 0, true};
  const uint32_t n = flashlog_read(trip, 0, verify_visit, &c, nullptr);
  return c.ok && n == count && c.next == count;
}

uint32_t torn_of(uint32_t trip) {
  uint32_t t = 0;
  flashlog_read(trip, 0, nullptr, nullptr, &t);
  return t;
}

// ---- the tests ---------------------------------------------------------

const uint32_t A = 0x1001, B = 0x2002, C = 0x3003;

void t_empty(void) {
  g_test = "empty";
  blank(16);
  LogStats s;
  flashlog_stats(&s);
  CHECK(s.sectors == 16 && s.free == 16 && s.used == 0 && s.trips == 0);
  uint32_t last;
  CHECK(!flashlog_last_seq(A, &last));
  CHECK(flashlog_read(A, 0, nullptr, nullptr, nullptr) == 0);
}

void t_roundtrip(void) {
  g_test = "roundtrip across sectors";
  blank(16);
  for (uint32_t i = 0; i < 100; i++) {
    uint32_t seq;
    CHECK(add(A, &seq) == LogErr::Ok);
    CHECK(seq == i);
  }
  CHECK(intact(A, 100));
  LogStats s;
  flashlog_stats(&s);
  CHECK(s.used == 4);                       // 100 records / 31 per sector
}

void t_reboot_resumes(void) {
  g_test = "reboot resumes";
  blank(16);
  for (int i = 0; i < 40; i++) CHECK(add(A, nullptr) == LogErr::Ok);
  power_on();
  uint32_t seq;
  CHECK(add(A, &seq) == LogErr::Ok);
  CHECK(seq == 40);
  CHECK(intact(A, 41));
  LogStats s;
  flashlog_stats(&s);
  CHECK(s.used == 2);                       // 41 records still fit in 2
}

void t_from_seq(void) {
  g_test = "read from a sequence";
  blank(16);
  for (int i = 0; i < 70; i++) CHECK(add(A, nullptr) == LogErr::Ok);
  CHECK(flashlog_read(A, 50, nullptr, nullptr, nullptr) == 20);
  CHECK(flashlog_read(A, 70, nullptr, nullptr, nullptr) == 0);
}

// Power fails at every byte of one record write in turn. Whatever the
// byte, after the reboot: everything before is intact, the torn record
// is not a record, and the next write gets the sequence it should.
void t_torn_record(void) {
  g_test = "torn record, every byte";
  for (uint32_t cut = 0; cut < LOG_FRAME; cut++) {
    blank(8);
    for (int i = 0; i < 10; i++) CHECK(add(A, nullptr) == LogErr::Ok);
    g_ram.budget = cut;
    CHECK(add(A, nullptr) != LogErr::Ok);
    power_on();
    CHECK(intact(A, 10));
    uint32_t seq;
    CHECK(add(A, &seq) == LogErr::Ok);
    CHECK(seq == 10);
    CHECK(intact(A, 11));
    CHECK(torn_of(A) == (cut ? 1u : 0u));   // cut at 0 wrote nothing
  }
}

// Power fails while a new sector's header is being written, at every
// byte. The sector must come back as free or dirty, never as a sector
// of the trip, and the trip must carry on.
void t_torn_header(void) {
  g_test = "torn sector header, every byte";
  for (uint32_t cut = 0; cut < LOG_HEADER; cut++) {
    blank(8);
    for (uint32_t i = 0; i < LOG_SLOTS; i++) CHECK(add(A, nullptr) == LogErr::Ok);
    g_ram.budget = cut;                       // next add opens a sector
    CHECK(add(A, nullptr) != LogErr::Ok);
    power_on();
    LogStats s;
    flashlog_stats(&s);
    CHECK(s.used == 1);
    CHECK(intact(A, LOG_SLOTS));
    uint32_t seq;
    CHECK(add(A, &seq) == LogErr::Ok);
    CHECK(seq == LOG_SLOTS);
    CHECK(intact(A, LOG_SLOTS + 1));
  }
}

// Power fails after a sector's header is down but before its first
// record. The empty sector belongs to the trip and must not confuse the
// next sequence number.
void t_empty_sector_after_cut(void) {
  g_test = "power cut between header and first record";
  blank(8);
  for (uint32_t i = 0; i < LOG_SLOTS; i++) CHECK(add(A, nullptr) == LogErr::Ok);
  g_ram.budget = LOG_HEADER;                  // header fits, frame does not
  CHECK(add(A, nullptr) != LogErr::Ok);
  power_on();
  uint32_t seq;
  CHECK(add(A, &seq) == LogErr::Ok);
  CHECK(seq == LOG_SLOTS);
  CHECK(intact(A, LOG_SLOTS + 1));
}

void t_two_trips(void) {
  g_test = "two trips interleaved";
  blank(16);
  for (int i = 0; i < 20; i++) {
    CHECK(add(A, nullptr) == LogErr::Ok);
    CHECK(add(B, nullptr) == LogErr::Ok);
  }
  CHECK(intact(A, 20));
  CHECK(intact(B, 20));
  LogStats s;
  flashlog_stats(&s);
  CHECK(s.used == 2);              // each trip resumed its own sector
  power_on();
  CHECK(add(A, nullptr) == LogErr::Ok);
  CHECK(intact(A, 21));
  CHECK(intact(B, 20));
  uint32_t t[4];
  CHECK(flashlog_trips(t, 4) == 2);
  CHECK(t[0] == A && t[1] == B);
}

void t_erase_trip(void) {
  g_test = "erase one trip";
  blank(16);
  for (int i = 0; i < 50; i++) CHECK(add(A, nullptr) == LogErr::Ok);
  for (int i = 0; i < 50; i++) CHECK(add(B, nullptr) == LogErr::Ok);
  CHECK(flashlog_erase_trip(A) == LogErr::Ok);
  CHECK(flashlog_read(A, 0, nullptr, nullptr, nullptr) == 0);
  CHECK(intact(B, 50));
  power_on();
  CHECK(flashlog_read(A, 0, nullptr, nullptr, nullptr) == 0);
  CHECK(intact(B, 50));
  uint32_t seq;
  CHECK(add(B, &seq) == LogErr::Ok);
  CHECK(seq == 50);
}

// An erase cut short in the middle of deleting a trip: the other trip
// is untouched, and nothing presents itself as a record it is not.
void t_torn_erase(void) {
  g_test = "power cut while erasing a trip";
  blank(16);
  for (int i = 0; i < 80; i++) CHECK(add(A, nullptr) == LogErr::Ok);
  for (int i = 0; i < 30; i++) CHECK(add(B, nullptr) == LogErr::Ok);
  g_ram.tear_erase = true;
  flashlog_erase_trip(A);
  power_on();
  CHECK(intact(B, 30));
  // What remains of A has gaps where the erase got to, but every record
  // still presented is exactly the one written under that sequence.
  bool all_right = true;
  flashlog_read(A, 0, content_visit, &all_right, nullptr);
  CHECK(all_right);
  uint32_t seq;
  CHECK(add(B, &seq) == LogErr::Ok && seq == 30);
}

void t_full(void) {
  g_test = "full, then space reclaimed";
  blank(4);
  for (uint32_t i = 0; i < 2 * LOG_SLOTS; i++) CHECK(add(A, nullptr) == LogErr::Ok);
  for (uint32_t i = 0; i < 2 * LOG_SLOTS; i++) CHECK(add(B, nullptr) == LogErr::Ok);
  CHECK(add(C, nullptr) == LogErr::Full);     // no sector left for C
  CHECK(add(B, nullptr) == LogErr::Full);     // nor for more of B
  CHECK(intact(A, 2 * LOG_SLOTS));
  CHECK(flashlog_erase_trip(A) == LogErr::Ok);
  uint32_t seq;
  CHECK(add(C, &seq) == LogErr::Ok && seq == 0);
  CHECK(intact(B, 2 * LOG_SLOTS));
}

// Many trips through a small log: sectors are reused in rotation and the
// order of a trip survives the wrap from the last sector to the first.
void t_wrap(void) {
  g_test = "wrap-around and wear rotation";
  blank(6);
  uint32_t trip = 0x5000;
  for (int round = 0; round < 10; round++) {
    trip++;
    for (uint32_t i = 0; i < 3 * LOG_SLOTS; i++) {
      CHECK(add(trip, nullptr) == LogErr::Ok);
    }
    CHECK(intact(trip, 3 * LOG_SLOTS));
    if (round > 0) CHECK(flashlog_erase_trip(trip - 1) == LogErr::Ok);
  }
  power_on();
  CHECK(intact(trip, 3 * LOG_SLOTS));
  // 10 trips x 3 sectors over 6 sectors: every sector erased several
  // times, none hammered. (Each erase_trip erases too.)
  CHECK(g_ram.erases >= 30);
}

void t_garbage(void) {
  g_test = "garbage sector";
  blank(8);
  for (int i = 0; i < 10; i++) CHECK(add(A, nullptr) == LogErr::Ok);
  // Random bytes over a sector the log never wrote: an interrupted
  // erase from some earlier life, or anything else.
  uint32_t x = 12345;
  for (uint32_t i = 0; i < LOG_SECTOR; i++) {
    x = x * 1103515245 + 12345;
    g_ram.mem[5 * LOG_SECTOR + i] = (uint8_t)(x >> 16);
  }
  power_on();
  LogStats s;
  flashlog_stats(&s);
  CHECK(s.dirty == 1 && s.used == 1);
  CHECK(intact(A, 10));
  // And it is reused like any free sector.
  for (uint32_t i = 0; i < 7 * LOG_SLOTS - 10; i++) {
    CHECK(add(A, nullptr) == LogErr::Ok);
  }
  CHECK(intact(A, 7 * LOG_SLOTS));
}

void t_rejects(void) {
  g_test = "bad arguments";
  blank(4);
  uint8_t big[LOG_PAYLOAD_MAX + 1] = {0};
  CHECK(flashlog_append(LOG_TRIP_NONE, 1, big, 1, nullptr) == LogErr::BadTrip);
  CHECK(flashlog_append(LOG_TRIP_INVALID, 1, big, 1, nullptr) == LogErr::BadTrip);
  CHECK(flashlog_append(A, 1, big, sizeof(big), nullptr) == LogErr::TooBig);
  CHECK(flashlog_append(A, 0xFF, big, 1, nullptr) == LogErr::BadTrip);
  CHECK(flashlog_append(A, 1, big, LOG_PAYLOAD_MAX, nullptr) == LogErr::Ok);
}

}  // namespace

int flashlog_selftest(void) {
  g_fail = 0;
  memset(&g_ram, 0, sizeof(g_ram));
  g_ram.mem = (uint8_t *)heap_caps_malloc(16 * LOG_SECTOR, MALLOC_CAP_SPIRAM);
  if (!g_ram.mem) {
    printf("    no memory for the test image\n");
    return -1;
  }
  g_if = {0, r_read, r_write, r_erase, &g_ram, nullptr, nullptr};

  struct { void (*fn)(void); } tests[] = {
      {t_empty}, {t_roundtrip}, {t_reboot_resumes}, {t_from_seq},
      {t_torn_record}, {t_torn_header}, {t_empty_sector_after_cut},
      {t_two_trips}, {t_erase_trip}, {t_torn_erase}, {t_full},
      {t_wrap}, {t_garbage}, {t_rejects},
  };
  for (auto &t : tests) {
    const int before = g_fail;
    t.fn();
    printf("  %-44s %s\n", g_test, g_fail == before ? "pass" : "FAIL");
  }
  free(g_ram.mem);

  // Hand the log back to the real partition.
  FlashIf real;
  if (flashlog_partition(&real)) {
    static FlashIf s_real;
    s_real = real;
    flashlog_init(&s_real);
  }
  return g_fail;
}
