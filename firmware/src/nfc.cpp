#include "nfc.h"

#include <esp_timer.h>
#include <string.h>

#include "board.h"
#include "bus.h"
#include "health.h"

namespace {

const uint16_t SYS_IC_REF = 0x0017;
const uint16_t SYS_UID = 0x0018;          // 8 bytes, least significant first
const uint16_t DYN_EH_CTRL = 0x2002;
const uint16_t DYN_IT_STS = 0x2005;

const uint8_t EH_FIELD_ON = 0x04;
const uint8_t IT_FIELD_FALLING = 0x08;
const uint8_t IT_FIELD_RISING = 0x10;

// Longer than any RF exchange a phone makes with a tag this size. A
// refusal that goes on past this is the part, not a phone.
const uint32_t RF_BUSY_LIMIT_MS = 10000;

// The EEPROM programs in four-byte blocks. Writing whole aligned blocks
// keeps every write inside one, so none can wrap.
const size_t BLOCK = 4;

uint8_t g_uid[8];
uint8_t g_ic_ref = 0;
bool g_have_uid = false;
uint32_t g_taps = 0;
uint32_t g_refused_since = 0;   // 0: not currently being refused
bool g_present = false;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool write_block(uint16_t at, const uint8_t *data) {
  uint8_t cur[BLOCK];
  if (i2c_read_mem16(Dev::Nfc, ADDR_ST25_USER, at, cur, BLOCK) == BusErr::Ok &&
      !memcmp(cur, data, BLOCK)) {
    return true;       // already right: no wear, no write cycle
  }
  if (i2c_write_mem16(Dev::Nfc, ADDR_ST25_USER, at, data, BLOCK) != BusErr::Ok) {
    return false;
  }
  if (i2c_read_mem16(Dev::Nfc, ADDR_ST25_USER, at, cur, BLOCK) != BusErr::Ok) {
    return false;
  }
  return !memcmp(cur, data, BLOCK);
}

}  // namespace

bool nfc_begin(void) {
  uint8_t ref = 0, uid[8];
  if (i2c_read_mem16(Dev::Nfc, ADDR_ST25_SYS, SYS_IC_REF, &ref, 1) != BusErr::Ok) {
    return false;
  }
  if (i2c_read_mem16(Dev::Nfc, ADDR_ST25_SYS, SYS_UID, uid, 8) != BusErr::Ok) {
    return false;
  }
  g_ic_ref = ref;
  for (int i = 0; i < 8; i++) g_uid[i] = uid[7 - i];
  g_have_uid = true;

  // Drop edges latched before this boot; a tap from minutes ago is not
  // a phone arriving now. Bring-up's first read found exactly that.
  uint8_t sts;
  i2c_read_mem16(Dev::Nfc, ADDR_ST25_USER, DYN_IT_STS, &sts, 1);
  return true;
}

bool nfc_uid(uint8_t out[8]) {
  if (!g_have_uid) return false;
  memcpy(out, g_uid, 8);
  return true;
}

uint8_t nfc_ic_ref(void) { return g_ic_ref; }

bool nfc_poll(NfcPoll *out) {
  NfcPoll p = {};
  uint8_t sts = 0, eh = 0;
  const bool ok =
      i2c_read_mem16(Dev::Untracked, ADDR_ST25_USER, DYN_IT_STS, &sts, 1) ==
          BusErr::Ok &&
      i2c_read_mem16(Dev::Untracked, ADDR_ST25_USER, DYN_EH_CTRL, &eh, 1) ==
          BusErr::Ok;
  const uint32_t t = now_ms();

  // A phone counts as one arrival however it shows up: as the field
  // bit, as a latched rising edge (a tap shorter than the poll), or as
  // the tag refusing I2C because the RF side has it. Tracking presence
  // as a state is what stops one tap being counted twice when it is
  // seen first as a refusal and then as the edge it latched.
  bool present;
  if (ok) {
    g_refused_since = 0;
    health_ok(Dev::Nfc);
    const bool on = (eh & EH_FIELD_ON) != 0;
    p.field = on ? NfcField::Present : NfcField::Absent;
    p.arrived = !g_present && (on || (sts & IT_FIELD_RISING));
    p.left = !on && (g_present || (sts & IT_FIELD_FALLING));
    present = on;
  } else {
    if (!g_refused_since) g_refused_since = t ? t : 1;
    p.field = NfcField::RfBusy;
    p.arrived = !g_present;
    present = true;
    if (t - g_refused_since >= RF_BUSY_LIMIT_MS) {
      health_fail(Dev::Nfc, -500);
      p.field = NfcField::Unknown;
    }
  }
  g_present = present;

  if (p.arrived) g_taps++;
  if (out) *out = p;
  return ok;
}

uint32_t nfc_tap_count(void) { return g_taps; }

bool nfc_read_user(uint16_t offset, uint8_t *buf, size_t n) {
  if (offset + n > NFC_USER_BYTES) return false;
  // Chunked: the bus layer's buffers are sized for registers, not for
  // the whole memory in one go.
  while (n) {
    const size_t k = n > 32 ? 32 : n;
    if (i2c_read_mem16(Dev::Nfc, ADDR_ST25_USER, offset, buf, k) != BusErr::Ok) {
      return false;
    }
    offset += k;
    buf += k;
    n -= k;
  }
  return true;
}

bool nfc_write_ndef(const uint8_t *ndef, size_t n) {
  uint8_t img[NFC_USER_BYTES];
  size_t i = 0;

  // Capability container: NDEF magic, version 1.0 with read and write
  // open, memory size in 8-byte units, read-multiple-block supported.
  img[i++] = 0xE1;
  img[i++] = 0x40;
  img[i++] = (uint8_t)(NFC_USER_BYTES / 8);
  img[i++] = 0x01;

  // NDEF TLV. One length byte up to 254, three bytes beyond.
  img[i++] = 0x03;
  if (n < 0xFF) {
    img[i++] = (uint8_t)n;
  } else {
    img[i++] = 0xFF;
    img[i++] = (uint8_t)(n >> 8);
    img[i++] = (uint8_t)(n & 0xFF);
  }
  if (i + n + 1 > sizeof(img)) return false;
  memcpy(img + i, ndef, n);
  i += n;
  img[i++] = 0xFE;     // terminator TLV

  // Pad to a whole block with zeros, which is what a reader expects
  // after the terminator anyway.
  while (i % BLOCK) img[i++] = 0;

  for (size_t off = 0; off < i; off += BLOCK) {
    if (!write_block((uint16_t)off, img + off)) return false;
  }
  return true;
}

size_t ndef_text_record(const char *text, uint8_t *out, size_t cap) {
  const size_t tl = strlen(text);
  const size_t payload = 3 + tl;          // status byte, "en", text
  if (payload > 255 || 4 + payload > cap) return 0;
  size_t i = 0;
  out[i++] = 0xD1;     // first and last record, short, well-known type
  out[i++] = 0x01;     // type length
  out[i++] = (uint8_t)payload;
  out[i++] = 'T';
  out[i++] = 0x02;     // UTF-8, two-letter language code follows
  out[i++] = 'e';
  out[i++] = 'n';
  memcpy(out + i, text, tl);
  return i + tl;
}
