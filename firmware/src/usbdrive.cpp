#include "usbdrive.h"

#include <driver/usb_serial_jtag.h>
#include <soc/rtc_cntl_reg.h>
#include <esp_app_desc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "config.h"
#include "flashlog.h"
#include "logrow.h"
#include "power.h"
#include "timekeep.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "uplink.h"

namespace {

volatile bool g_active = false;
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

struct VFile {
  char name[11];          // 8.3, space padded
  const uint8_t *data;
  uint32_t size;
  uint16_t first;         // first cluster
  uint16_t clusters;
};

const int MAX_FILES = 8;
VFile g_files[MAX_FILES];
int g_nfiles = 0;
uint16_t g_fat_date = 0, g_fat_time = 0;
uint32_t g_volume_id = 0;

// DEVICE.TXT, written once when the drive starts: the PC reads the
// directory when it mounts and holds on to it, so the drive is a snapshot
// of that moment (the B2 screen says so).
char g_device_txt[1024];

void add_file(const char *name83, const uint8_t *data, uint32_t size) {
  if (g_nfiles >= MAX_FILES) return;
  VFile &f = g_files[g_nfiles];
  memset(f.name, ' ', sizeof(f.name));
  // "DEVICE.TXT" -> "DEVICE  TXT"
  const char *dot = strchr(name83, '.');
  const size_t base = dot ? (size_t)(dot - name83) : strlen(name83);
  memcpy(f.name, name83, base > 8 ? 8 : base);
  if (dot) memcpy(f.name + 8, dot + 1, strlen(dot + 1) > 3 ? 3 : strlen(dot + 1));
  f.data = data;
  f.size = size;
  f.first = g_nfiles ? (uint16_t)(g_files[g_nfiles - 1].first + g_files[g_nfiles - 1].clusters) : 2;
  f.clusters = (uint16_t)((size + CLUSTER - 1) / CLUSTER);
  if (!f.clusters) f.clusters = 1;
  g_nfiles++;
}

void snapshot(void) {
  TimeStamp ts;
  time_now(&ts);
  char when[24] = "unknown";
  if (ts.quality != TimeSource::None) {
    const time_t local = (time_t)(ts.utc_ms / 1000) + config().tz_offset_min * 60;
    struct tm tm;
    gmtime_r(&local, &tm);
    strftime(when, sizeof(when), "%H:%M:%S %d/%m/%Y", &tm);
    g_fat_date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    g_fat_time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  }
  uint32_t trips[64];
  const int n = flashlog_trips(trips, 64);
  UplinkStatus us;
  uplink_status(&us);
  snprintf(g_device_txt, sizeof(g_device_txt),
           "mCOLD Foam V1\r\n"
           "SN          %s\r\n"
           "Firmware    %s\r\n"
           "Drive       %s (read-only)\r\n"
           "Snapshot    %s\r\n"
           "Trips       %d in the log\r\n"
           "Not sent    %lu rows\r\n"
           "\r\n"
           "This drive is a snapshot taken when the cable went in. The trips\r\n"
           "as CSV files come with the next firmware (F3).\r\n",
           g_sn, esp_app_get_description()->version, g_label, when, n,
           (unsigned long)us.records_pending);
  g_nfiles = 0;
  add_file("DEVICE.TXT", (const uint8_t *)g_device_txt, strlen(g_device_txt));
}

void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

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

void dir_entry(uint8_t *e, const char name[11], uint8_t attr, uint16_t first, uint32_t size) {
  memcpy(e, name, 11);
  e[11] = attr;
  put16(e + 14, g_fat_time);    // created
  put16(e + 16, g_fat_date);
  put16(e + 18, g_fat_date);    // accessed
  put16(e + 22, g_fat_time);    // written
  put16(e + 24, g_fat_date);
  put16(e + 26, first);
  put32(e + 28, size);
}

void root_sector(uint32_t idx, uint8_t *b) {
  for (uint32_t k = 0; k < SECTOR / 32; k++) {
    const uint32_t i = idx * (SECTOR / 32) + k;
    uint8_t *e = b + k * 32;
    if (i == 0) {
      char lab[11];
      memset(lab, ' ', sizeof(lab));
      memcpy(lab, g_label, strlen(g_label));
      dir_entry(e, lab, 0x08, 0, 0);          // the volume label
    } else if ((int)i - 1 < g_nfiles) {
      const VFile &f = g_files[i - 1];
      dir_entry(e, f.name, 0x01, f.first, f.size);   // read-only
    }
  }
}

void data_sector(uint32_t lba, uint8_t *b) {
  const uint32_t rel = lba - DATA;
  const uint32_t cl = rel / SPC + 2;
  for (int i = 0; i < g_nfiles; i++) {
    const VFile &f = g_files[i];
    if (cl < f.first || cl >= (uint32_t)f.first + f.clusters) continue;
    const uint32_t off = (cl - f.first) * CLUSTER + (rel % SPC) * SECTOR;
    if (off < f.size) {
      const uint32_t n = f.size - off < SECTOR ? f.size - off : SECTOR;
      memcpy(b, f.data + off, n);
    }
    return;
  }
}

void sector(uint32_t lba, uint8_t *b) {
  memset(b, 0, SECTOR);
  if (lba == 0) boot_sector(b);
  else if (lba >= FAT1 && lba < ROOT) fat_sector((lba - FAT1) % FATSZ, b);
  else if (lba >= ROOT && lba < DATA) root_sector(lba - ROOT, b);
  else if (lba < TOTAL) data_sector(lba, b);
}

}  // namespace

// ---- TinyUSB's SCSI callbacks: the drive, read-only ----------------------------

extern "C" {

void tud_msc_inquiry_cb(uint8_t, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
  memcpy(vendor_id, "mCOLD   ", 8);
  memcpy(product_id, "Trip drive      ", 16);
  memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t) { return g_active; }

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
