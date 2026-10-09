#include "usbdrive.h"

#include <driver/usb_serial_jtag.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/rtc_cntl_reg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "config.h"
#include "flashlog.h"
#include "logrow.h"
#include "power.h"
#include "timekeep.h"
#include "tinyusb.h"
#include "temp.h"
#include "trip.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "uplink.h"

namespace {

volatile bool g_active = false;
volatile bool g_ready = false;          // a snapshot stands behind the sectors
volatile bool g_resnap = false;         // the PC (re)mounted: a new snapshot is due
char g_sn[SN_LEN] = "";
char g_label[12] = "";

// ---- the virtual drive: FAT16, 64 MiB, read-only ---------------------------
//
// Big enough to be FAT16 (at least 4085 clusters), which every host reads
// without asking; the size costs nothing, the sectors are made up on
// request. 4 KB clusters.
const uint32_t SECTOR = 512;
const uint32_t SPC = 8;                 // sectors per cluster
const uint32_t CLUSTER = SECTOR * SPC;
const uint32_t TOTAL = 131072;          // sectors: 64 MiB
const uint32_t FATSZ = 64;              // sectors per FAT: 16384 entries
const uint32_t ROOT_ENTRIES = 512;
const uint32_t FAT1 = 1;
const uint32_t FAT2 = FAT1 + FATSZ;
const uint32_t ROOT = FAT2 + FATSZ;
const uint32_t DATA = ROOT + ROOT_ENTRIES * 32 / SECTOR;
const uint32_t MAX_CLUSTER = 2 + (TOTAL - DATA) / SPC;

// ---- one trip as a CSV file ---------------------------------------------------
//
// The file is the trip as `trip csv` prints it (logrow.h columns), CRLF
// line ends. Its size has to be known when the PC reads the directory, so
// mounting walks each trip once and notes where every 64th row starts;
// a read anywhere then starts from the nearest such point and generates
// forward. The PC reads files front to back, so the generator usually
// just carries on from where the last read stopped.
const uint32_t HEADER_SEQ = UINT32_MAX;
const uint32_t CK_EVERY = 64;

struct Ck {
  uint32_t seq;     // the row starting here; HEADER_SEQ: the header line
  uint32_t off;
};

struct Csv {
  uint32_t trip;
  uint32_t last;    // the snapshot's last row: a running trip grows after it
  RowHeader h;
  Ck *ck;
  uint32_t nck, cap;
};

struct VFile {
  char name[11];          // 8.3, space padded
  char lfn[64];           // long name, "" for none
  uint32_t size;
  uint16_t first;         // first cluster
  uint16_t clusters;
  uint16_t date, time;
  const uint8_t *data;    // a text file, or
  int csv;                // a trip: index into g_csv; -1 for text
};

const int MAX_FILES = 64;
VFile g_files[MAX_FILES];
int g_nfiles = 0;
Csv g_csv[MAX_FILES];
int g_ncsv = 0;
int g_tz = 0;                  // the time zone the files are written in, fixed at the snapshot
uint16_t g_fat_date = 0, g_fat_time = 0;
uint32_t g_volume_id = 0;
uint8_t *g_root = nullptr;     // the root directory, built at the snapshot

// DEVICE.TXT: what the box is, and what is on the drive.
char g_device_txt[1024];

void *psram(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}

void fat_stamp(uint32_t utc, uint16_t *date, uint16_t *time) {
  *date = g_fat_date;
  *time = g_fat_time;
  if (!utc) return;
  const time_t local = (time_t)utc + g_tz * 60;
  struct tm tm;
  gmtime_r(&local, &tm);
  *date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
  *time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

bool add_file(const char *short83, const char *lfn, uint32_t size, const uint8_t *data, int csv,
              uint32_t utc) {
  if (g_nfiles >= MAX_FILES) return false;
  const uint16_t first =
      g_nfiles ? (uint16_t)(g_files[g_nfiles - 1].first + g_files[g_nfiles - 1].clusters) : 2;
  uint32_t clusters = (size + CLUSTER - 1) / CLUSTER;
  if (!clusters) clusters = 1;
  if (first + clusters > MAX_CLUSTER) return false;     // the drive is full
  VFile &f = g_files[g_nfiles];
  memset(&f, 0, sizeof(f));
  memset(f.name, ' ', sizeof(f.name));
  const char *dot = strchr(short83, '.');
  const size_t base = dot ? (size_t)(dot - short83) : strlen(short83);
  memcpy(f.name, short83, base > 8 ? 8 : base);
  if (dot) memcpy(f.name + 8, dot + 1, strlen(dot + 1) > 3 ? 3 : strlen(dot + 1));
  snprintf(f.lfn, sizeof(f.lfn), "%s", lfn ? lfn : "");
  f.size = size;
  f.first = first;
  f.clusters = (uint16_t)clusters;
  f.data = data;
  f.csv = csv;
  fat_stamp(utc, &f.date, &f.time);
  g_nfiles++;
  return true;
}

// ---- walking a trip --------------------------------------------------------------

struct Walk {
  Csv *c;
  uint32_t off;            // bytes so far
  uint32_t rows;
  LogRow start;            // the first row: the file's name and date
  bool have_start;
};

bool size_visit(const LogRecord &r, void *ctx) {
  Walk *w = (Walk *)ctx;
  if (r.seq > w->c->last) return false;
  LogRow row;
  if (!row_decode(r.type, r.payload, r.len, &row, nullptr, nullptr)) return true;
  if (!w->have_start) {
    w->start = row;
    w->have_start = true;
  }
  if (w->rows % CK_EVERY == 0) {
    Csv &c = *w->c;
    if (c.nck == c.cap) {
      const uint32_t cap = c.cap ? c.cap * 2 : 64;
      Ck *n = (Ck *)psram(cap * sizeof(Ck));
      if (!n) return false;
      if (c.ck) {
        memcpy(n, c.ck, c.nck * sizeof(Ck));
        free(c.ck);
      }
      c.ck = n;
      c.cap = cap;
    }
    c.ck[c.nck++] = {r.seq, w->off};
  }
  char line[400];
  row_csv(row, r.seq, g_sn, w->c->h, g_tz, line, sizeof(line));
  w->off += strlen(line) + 2;
  w->rows++;
  return true;
}

struct First {
  bool row;
  RowHeader h;
};
bool first_visit(const LogRecord &r, void *ctx) {
  First *f = (First *)ctx;
  LogRow row;
  f->row = row_decode(r.type, r.payload, r.len, &row, &f->h, nullptr) && f->h.valid;
  return false;
}

void free_csv(void) {
  for (int i = 0; i < g_ncsv; i++) free(g_csv[i].ck);
  memset(g_csv, 0, sizeof(g_csv));
  g_ncsv = 0;
}

// Adds every trip in the row format as a file, oldest first, until the
// directory or the drive is full. Returns how many.
int add_trips(void) {
  uint32_t trips[MAX_FILES];
  const int n = flashlog_trips(trips, MAX_FILES - 1);
  int added = 0;
  char head[256];
  row_csv_header(head, sizeof(head));
  for (int i = 0; i < n; i++) {
    if (trips[i] > TRIP_ID_REAL_MAX) continue;
    First first = {};
    flashlog_read(trips[i], 0, first_visit, &first, nullptr);
    if (!first.row || !first.h.has_id) continue;   // from before trip_id: no file
    Csv &c = g_csv[g_ncsv];
    memset(&c, 0, sizeof(c));
    c.trip = trips[i];
    c.h = first.h;
    if (!flashlog_last_seq(c.trip, &c.last)) continue;
    // The header line is the first point; the rows follow.
    c.cap = 64;
    c.ck = (Ck *)psram(c.cap * sizeof(Ck));
    if (!c.ck) break;
    c.ck[c.nck++] = {HEADER_SEQ, 0};
    Walk w = {&c, (uint32_t)strlen(head) + 2, 0, {}, false};
    flashlog_read(c.trip, 0, size_visit, &w, nullptr);
    char name[64], short83[13];
    row_file_name(g_sn, w.start, c.trip, g_tz, name, sizeof(name));
    snprintf(short83, sizeof(short83), "T%07lu.CSV", (unsigned long)(c.trip % 10000000));
    if (!add_file(short83, name, w.off, nullptr, g_ncsv, w.have_start ? w.start.utc : 0)) {
      free(c.ck);
      memset(&c, 0, sizeof(c));
      break;
    }
    g_ncsv++;
    added++;
  }
  return added;
}

// ---- the CSV generator ---------------------------------------------------------------

struct Gen {
  int csv = -1;
  uint32_t pos = 0;            // file offset of the next byte it gives
  bool header = false;         // the header line is next
  uint32_t next_seq = 0;
  char line[402];
  uint16_t len = 0, used = 0;  // the current line, and how much of it is given
  LogRow rows[16];
  uint32_t seqs[16];
  int nrows = 0, irow = 0;
  bool eof = false;
};
Gen g_gen;

struct Batch {
  Gen *g;
  uint32_t last;
};
bool batch_visit(const LogRecord &r, void *ctx) {
  Batch *b = (Batch *)ctx;
  if (r.seq > b->last || b->g->nrows >= 16) return false;
  LogRow row;
  if (!row_decode(r.type, r.payload, r.len, &row, nullptr, nullptr)) return true;
  b->g->rows[b->g->nrows] = row;
  b->g->seqs[b->g->nrows] = r.seq;
  b->g->nrows++;
  return b->g->nrows < 16;
}

// The next line into g.line, with its CRLF. False at the end of the file.
bool next_line(Gen &g) {
  const Csv &c = g_csv[g.csv];
  if (g.header) {
    row_csv_header(g.line, sizeof(g.line) - 2);
    g.header = false;
  } else {
    if (g.irow >= g.nrows) {
      if (g.eof || g.next_seq > c.last) return false;
      g.nrows = g.irow = 0;
      Batch b = {&g, c.last};
      flashlog_read(c.trip, g.next_seq, batch_visit, &b, nullptr);
      if (!g.nrows) {
        g.eof = true;
        return false;
      }
    }
    const uint32_t seq = g.seqs[g.irow];
    row_csv(g.rows[g.irow], seq, g_sn, c.h, g_tz, g.line, sizeof(g.line) - 2);
    g.next_seq = seq + 1;
    g.irow++;
  }
  g.len = (uint16_t)strlen(g.line);
  g.line[g.len++] = '\r';
  g.line[g.len++] = '\n';
  g.used = 0;
  return true;
}

void seek(Gen &g, int csv, uint32_t offset) {
  const Csv &c = g_csv[csv];
  uint32_t k = 0;
  while (k + 1 < c.nck && c.ck[k + 1].off <= offset) k++;
  g.csv = csv;
  g.pos = c.ck[k].off;
  g.header = c.ck[k].seq == HEADER_SEQ;
  g.next_seq = g.header ? 0 : c.ck[k].seq;
  g.len = g.used = 0;
  g.nrows = g.irow = 0;
  g.eof = false;
  // Forward to the offset asked for.
  while (g.pos < offset) {
    if (g.used == g.len && !next_line(g)) return;
    const uint32_t skip = offset - g.pos < (uint32_t)(g.len - g.used) ? offset - g.pos
                                                                       : (uint32_t)(g.len - g.used);
    g.used += skip;
    g.pos += skip;
  }
}

void csv_read(int csv, uint32_t offset, uint8_t *dst, uint32_t n) {
  Gen &g = g_gen;
  if (g.csv != csv || g.pos != offset) seek(g, csv, offset);
  while (n) {
    if (g.used == g.len && !next_line(g)) return;      // past the end: zeros
    const uint32_t k = n < (uint32_t)(g.len - g.used) ? n : (uint32_t)(g.len - g.used);
    memcpy(dst, g.line + g.used, k);
    g.used += k;
    g.pos += k;
    dst += k;
    n -= k;
  }
}

// ---- the directory ---------------------------------------------------------------------

void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

void dir_entry(uint8_t *e, const char name[11], uint8_t attr, uint16_t first, uint32_t size,
               uint16_t date, uint16_t time) {
  memcpy(e, name, 11);
  e[11] = attr;
  put16(e + 14, time);    // created
  put16(e + 16, date);
  put16(e + 18, date);    // accessed
  put16(e + 22, time);    // written
  put16(e + 24, date);
  put16(e + 26, first);
  put32(e + 28, size);
}

uint8_t lfn_checksum(const char name[11]) {
  uint8_t s = 0;
  for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + (uint8_t)name[i]);
  return s;
}

// The long-name entries for `lfn`, in the order they go on disk (last
// part first), then nothing else. Returns how many.
int lfn_entries(uint8_t *out, const char *lfn, const char name[11]) {
  const int len = (int)strlen(lfn);
  const int parts = (len + 12) / 13;
  const uint8_t sum = lfn_checksum(name);
  static const int POS[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  for (int p = parts; p >= 1; p--) {
    uint8_t *e = out + (parts - p) * 32;
    memset(e, 0, 32);
    e[0] = (uint8_t)(p | (p == parts ? 0x40 : 0));
    e[11] = 0x0F;
    e[13] = sum;
    for (int k = 0; k < 13; k++) {
      const int i = (p - 1) * 13 + k;
      uint16_t ch = i < len ? (uint8_t)lfn[i] : (i == len ? 0x0000 : 0xFFFF);
      put16(e + POS[k], ch);
    }
  }
  return parts;
}

void build_root(void) {
  if (!g_root) g_root = (uint8_t *)psram(ROOT_ENTRIES * 32);
  if (!g_root) return;
  memset(g_root, 0, ROOT_ENTRIES * 32);
  uint32_t at = 0;
  char lab[11];
  memset(lab, ' ', sizeof(lab));
  memcpy(lab, g_label, strlen(g_label));
  dir_entry(g_root, lab, 0x08, 0, 0, g_fat_date, g_fat_time);
  at++;
  for (int i = 0; i < g_nfiles; i++) {
    const VFile &f = g_files[i];
    const int need = (f.lfn[0] ? ((int)strlen(f.lfn) + 12) / 13 : 0) + 1;
    if (at + need > ROOT_ENTRIES) break;
    if (f.lfn[0]) at += lfn_entries(g_root + at * 32, f.lfn, f.name);
    dir_entry(g_root + at * 32, f.name, 0x01, f.first, f.size, f.date, f.time);   // read-only
    at++;
  }
}

// ---- the snapshot ------------------------------------------------------------------------

void snapshot(void) {
  g_ready = false;
  g_tz = config().tz_offset_min;
  TimeStamp ts;
  time_now(&ts);
  char when[24] = "unknown";
  g_fat_date = (uint16_t)((46 << 9) | (1 << 5) | 1);   // 2026-01-01 with no time known
  g_fat_time = 0;
  if (ts.quality != TimeSource::None) {
    const time_t local = (time_t)(ts.utc_ms / 1000) + g_tz * 60;
    struct tm tm;
    gmtime_r(&local, &tm);
    strftime(when, sizeof(when), "%H:%M:%S %d/%m/%Y", &tm);
    g_fat_date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    g_fat_time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  }
  free_csv();
  g_gen = Gen();
  g_nfiles = 0;
  // DEVICE.TXT first (its text needs the trip count, so it is filled after
  // the trips are walked; its size is fixed by then).
  add_file("DEVICE.TXT", nullptr, 0, (const uint8_t *)g_device_txt, -1, 0);
  const int trips = add_trips();
  uint32_t all[64];
  const int in_log = flashlog_trips(all, 64);
  UplinkStatus us;
  uplink_status(&us);
  TripStatus st;
  trip_status(&st);
  // The running trip by number, file and rows, so nobody has to guess
  // which file stops at the snapshot.
  char running[160] = "no trip";
  if (st.active) {
    snprintf(running, sizeof(running), "trip %s (not in the log yet)", st.trip_id);
    for (int i = 0; i < g_nfiles; i++) {
      const VFile &f = g_files[i];
      if (f.csv < 0 || g_csv[f.csv].trip != st.id) continue;
      snprintf(running, sizeof(running),
               "trip %08lu #%u, %lu rows up to the snapshot\r\n"
               "            %s",
               (unsigned long)st.trip_date, (unsigned)st.trip_number,
               (unsigned long)(g_csv[f.csv].last + 1), f.lfn);
      break;
    }
  }
  snprintf(g_device_txt, sizeof(g_device_txt),
           "mCOLD Foam V1\r\n"
           "SN          %s\r\n"
           "Firmware    %s\r\n"
           "Drive       %s (read-only)\r\n"
           "Sensor      %s\r\n"
           "Snapshot    %s\r\n"
           "Trips       %d as CSV files, of %d in the log%s\r\n"
           "Running     %s\r\n"
           "Not sent    %lu rows\r\n"
           "\r\n"
           "The files are a snapshot taken when the cable went in: a trip still\r\n"
           "running stops at that moment. Unplug and plug in again for a newer one.\r\n"
           "Trips recorded before firmware 0.7.0-dev.3 are not shown.\r\n",
           g_sn, esp_app_get_description()->version, g_label,
           temp_sensor_name(temp_sensor()), when, trips, in_log,
           in_log > trips ? " (older format not shown)" : "",
           running,
           (unsigned long)us.records_pending);
  g_files[0].size = strlen(g_device_txt);
  build_root();
  g_ready = true;
  printf("[usb] drive %s: %d trip files\n", g_label, trips);
}

// ---- sectors ------------------------------------------------------------------------------

void boot_sector(uint8_t *b) {
  const uint8_t jmp[3] = {0xEB, 0x3C, 0x90};
  memcpy(b, jmp, 3);
  memcpy(b + 3, "MSWIN4.1", 8);
  put16(b + 11, SECTOR);
  b[13] = SPC;
  put16(b + 14, FAT1);          // reserved sectors
  b[16] = 2;                    // FATs
  put16(b + 17, ROOT_ENTRIES);
  put16(b + 19, 0);             // total in the 32-bit field
  b[21] = 0xF8;                 // fixed disk
  put16(b + 22, FATSZ);
  put16(b + 24, 32);            // sectors per track
  put16(b + 26, 64);            // heads
  put32(b + 28, 0);             // hidden
  put32(b + 32, TOTAL);
  b[36] = 0x80;                 // drive number
  b[38] = 0x29;                 // extended boot signature
  put32(b + 39, g_volume_id);
  memset(b + 43, ' ', 11);
  memcpy(b + 43, g_label, strlen(g_label));
  memcpy(b + 54, "FAT16   ", 8);
  b[510] = 0x55;
  b[511] = 0xAA;
}

void fat_sector(uint32_t idx, uint8_t *b) {
  for (uint32_t k = 0; k < SECTOR / 2; k++) {
    const uint32_t e = idx * (SECTOR / 2) + k;
    uint16_t v = 0;
    if (e == 0) v = 0xFFF8;
    else if (e == 1) v = 0xFFFF;
    else {
      for (int i = 0; i < g_nfiles; i++) {
        const VFile &f = g_files[i];
        if (e >= f.first && e < (uint32_t)f.first + f.clusters) {
          v = e + 1 < (uint32_t)f.first + f.clusters ? (uint16_t)(e + 1) : 0xFFFF;
          break;
        }
      }
    }
    put16(b + k * 2, v);
  }
}

void data_sector(uint32_t lba, uint8_t *b) {
  const uint32_t rel = lba - DATA;
  const uint32_t cl = rel / SPC + 2;
  for (int i = 0; i < g_nfiles; i++) {
    const VFile &f = g_files[i];
    if (cl < f.first || cl >= (uint32_t)f.first + f.clusters) continue;
    const uint32_t off = (cl - f.first) * CLUSTER + (rel % SPC) * SECTOR;
    if (off >= f.size) return;
    const uint32_t n = f.size - off < SECTOR ? f.size - off : SECTOR;
    if (f.csv >= 0) csv_read(f.csv, off, b, n);
    else memcpy(b, f.data + off, n);
    return;
  }
}

void sector(uint32_t lba, uint8_t *b) {
  memset(b, 0, SECTOR);
  if (lba == 0) boot_sector(b);
  else if (lba >= FAT1 && lba < ROOT) fat_sector((lba - FAT1) % FATSZ, b);
  else if (lba >= ROOT && lba < DATA) {
    if (g_root) memcpy(b, g_root + (lba - ROOT) * SECTOR, SECTOR);
  } else if (lba < TOTAL) data_sector(lba, b);
}

}  // namespace

// ---- TinyUSB's callbacks: mounting, and the drive, read-only ----------------------------

extern "C" {

// The PC has configured the device (the cable went in, or the PC woke):
// a fresh snapshot for what it is about to read -- taken by the console
// task (usbdrive_console_read), never here: walking the log and printing
// CSV overflowed TinyUSB's 4 KB task stack, and every plug-in reset the
// box (2026-10-05). The drive says "not ready" until it is done.
void tud_mount_cb(void) {
  if (!g_active) return;
  g_ready = false;
  g_resnap = true;
}

void tud_msc_inquiry_cb(uint8_t, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
  memcpy(vendor_id, "mCOLD   ", 8);
  memcpy(product_id, "Trip drive      ", 16);
  memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  if (g_active && g_ready) return true;
  tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x04, 0x01);   // becoming ready
  return false;
}

void tud_msc_capacity_cb(uint8_t, uint32_t *block_count, uint16_t *block_size) {
  *block_count = TOTAL;
  *block_size = SECTOR;
}

bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool, bool) { return true; }

bool tud_msc_is_writable_cb(uint8_t) { return false; }

int32_t tud_msc_read10_cb(uint8_t, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize) {
  uint8_t *out = (uint8_t *)buffer;
  uint8_t s[SECTOR];
  uint32_t done = 0;
  lba += offset / SECTOR;
  uint32_t within = offset % SECTOR;
  while (done < bufsize) {
    sector(lba, s);
    const uint32_t n = bufsize - done < SECTOR - within ? bufsize - done : SECTOR - within;
    memcpy(out + done, s + within, n);
    done += n;
    within = 0;
    lba++;
  }
  return (int32_t)done;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t, uint32_t, uint8_t *, uint32_t) {
  // Read-only: the host is told so (is_writable), and a write is refused.
  tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
  return -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *, uint16_t) {
  (void)scsi_cmd;
  tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
  return -1;
}

}  // extern "C"

// ---- start, stop, console -------------------------------------------------------

void usbdrive_start(const char *sn) {
  if (g_active) return;
  if (sn) snprintf(g_sn, sizeof(g_sn), "%s", sn);
  sn_usb_label(g_sn, g_label, sizeof(g_label));
  g_volume_id = 0x6D434F4C;   // "mCOL"
  for (const char *p = g_sn; *p; p++) g_volume_id = g_volume_id * 31 + (uint8_t)*p;
  snapshot();

  static const char lang[] = {0x09, 0x04};
  static const char *strs[6];
  strs[0] = lang;
  strs[1] = "mCOLD";
  strs[2] = "mCOLD Foam V1";
  strs[3] = g_sn;               // the SN as the USB serial number
  strs[4] = "mCOLD console";
  strs[5] = "mCOLD drive";
  tinyusb_config_t cfg = {};
  cfg.string_descriptor = strs;
  cfg.string_descriptor_count = 6;
  cfg.external_phy = false;
  if (tinyusb_driver_install(&cfg) != ESP_OK) {
    printf("[usb] TinyUSB did not start; the port stays as it was\n");
    return;
  }
  tinyusb_config_cdcacm_t acm = {};
  acm.usb_dev = TINYUSB_USBDEV_0;
  acm.cdc_port = TINYUSB_CDC_ACM_0;
  tusb_cdc_acm_init(&acm);
  g_active = true;
  printf("[usb] the port is now a serial port and the drive %s; the console moves there\n",
         g_label);
  fflush(stdout);
  esp_tusb_init_console(TINYUSB_CDC_ACM_0);
}

bool usbdrive_active(void) { return g_active; }

bool usbdrive_console_read(uint8_t *c, uint32_t ms) {
  if (!g_active) return usb_serial_jtag_read_bytes(c, 1, pdMS_TO_TICKS(ms)) == 1;
  if (g_resnap) {
    g_resnap = false;
    snapshot();
  }
  for (uint32_t waited = 0;; waited += 5) {
    size_t n = 0;
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, c, 1, &n) == ESP_OK && n == 1) return true;
    if (waited >= ms) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void usbdrive_phy_to_usj(void) {
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_SW_HW_USB_PHY_SEL);
}

void usbdrive_stop(void) {
  if (g_active) {
    g_active = false;
    fflush(stdout);
    esp_tusb_deinit_console(TINYUSB_CDC_ACM_0);
    tusb_cdc_acm_deinit(TINYUSB_CDC_ACM_0);
    tinyusb_driver_uninstall();
  }
  usbdrive_phy_to_usj();
}
