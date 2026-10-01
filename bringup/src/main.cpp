// mCOLD Foam V.1 -- board bring-up.
//
// One command per line over USB serial, one module per command. This is
// deliberately not an application: it powers a single rail, talks to a
// single part, says what came back, and puts the rail away again. When a
// board is new the useful question is never "does the product work", it
// is "which of these forty connections is the broken one", and a firmware
// that starts everything at once cannot answer that.
//
// Order to work through:
//   info   the MCU itself -- is this the N16R8 module the design assumed
//   pins   the inputs, with nothing powered: are any stuck
//   scan   the I2C bus: seven parts answer or they do not
//   id     each part's identity register: wired is not the same as alive
//   then the rails, and the parts behind them, one at a time
//
// Type ? for the list.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <math.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <LIS2DW12Sensor.h>
#include <esp_mac.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_4C.h>
#include <BLEDevice.h>
#include <BLEServer.h>

#include "board.h"

static SPIClass spi3(HSPI);

// Which panel is on the flex right now.
//
// The board is designed around the 2.13" (G), the four-ink part, and
// that is what production will carry. Bench work is being done with
// whatever panel is to hand, and the three are NOT interchangeable in
// firmware: the (G) driver reports busy on a LOW line, the mono and
// three-ink parts on a HIGH one. Run a mono panel under the (G) driver
// and it waits out a fifty-second timeout on a panel that was ready the
// whole time -- which reads exactly like dead hardware, and cost this
// bring-up an afternoon of suspecting the boost circuit.
//
//   1 -> mono, GxEPD2_213_B74        122x250, busy HIGH, ~3.6 s
//   3 -> 3 ink, GxEPD2_213_Z98c      122x250, busy HIGH, ~15 s
//   4 -> 4 ink, GxEPD2_213c_GDEY0213F51  busy LOW, ~25 s  <- production
#define EPD_PANEL 1

#if EPD_PANEL == 1
  #define EPD_INKS 1
  static GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT>
      epd(GxEPD2_213_B74(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));
#elif EPD_PANEL == 3
  #define EPD_INKS 3
  static GxEPD2_3C<GxEPD2_213_Z98c, GxEPD2_213_Z98c::HEIGHT>
      epd(GxEPD2_213_Z98c(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));
#else
  #define EPD_INKS 4
  static GxEPD2_4C<GxEPD2_213c_GDEY0213F51, GxEPD2_213c_GDEY0213F51::HEIGHT>
      epd(GxEPD2_213c_GDEY0213F51(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST,
                                  PIN_EPD_BUSY));
#endif
static Adafruit_NeoPixel leds(LED_COUNT, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
static bool ledsBegun = false;

// ---- small helpers --------------------------------------------------

static void line() { Serial.println("  ------------------------------------------------"); }

static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// A scan built on zero-length writes can report an address that nothing
// occupies: some controllers resolve "write no bytes" oddly. Asking for
// one byte instead puts a real read on the wire, so an address that
// answers one way and not the other is the scan talking, not a part.
static bool i2cReadable(uint8_t addr) {
  if (Wire.requestFrom((int)addr, 1) != 1) return false;
  Wire.read();
  return true;
}

// Returns false when the device did not acknowledge or returned short, so
// a dead part reads as "no answer" rather than as the value 0xFF.
static bool i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool i2cRead8(uint8_t addr, uint8_t reg, uint8_t *v) {
  return i2cRead(addr, reg, v, 1);
}

static bool i2cRead16be(uint8_t addr, uint8_t reg, uint16_t *v) {
  uint8_t b[2];
  if (!i2cRead(addr, reg, b, 2)) return false;
  *v = (uint16_t)b[0] << 8 | b[1];
  return true;
}

static bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

// ---- rails ----------------------------------------------------------

static bool railEpd = false, railSd = false, railLed = false;

static void setRailEpd(bool on) {
  digitalWrite(PIN_EPD_PWR_EN, on ? HIGH : LOW);
  railEpd = on;
  if (on) delay(20);        // let the switch and the panel's boost settle
}

static void setRailSd(bool on) {
  digitalWrite(PIN_SD_PWR_EN, on ? HIGH : LOW);
  railSd = on;
  if (on) delay(20);
}

static void setRailLed(bool on) {
  digitalWrite(PIN_LED_PWR_EN, on ? LOW : HIGH);   // P-MOS: LOW turns it on
  railLed = on;
  if (on) delay(5);
}

// ---- commands -------------------------------------------------------

static void cmdInfo() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flash = 0;
  esp_flash_get_size(nullptr, &flash);
  size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

  line();
  Serial.printf("  chip        ESP32-S3 rev %d, %d core%s\n",
                chip.revision, chip.cores, chip.cores > 1 ? "s" : "");
  Serial.printf("  flash       %u MB\n", (unsigned)(flash / (1024 * 1024)));
  Serial.printf("  psram       %u MB %s\n", (unsigned)(psram / (1024 * 1024)),
                psram ? "" : "<- NOT FOUND, expected 8 MB on an N16R8");
  Serial.printf("  free heap   %u B internal, %u B psram\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("  mac         %012llX\n", ESP.getEfuseMac());
  Serial.printf("  reset       %d (1=power on, 3=sw, 4=panic, 5=int wdt,"
                " 6=task wdt, 9=brownout, 11=usb, 12=jtag)\n"
                "  wake cause %d (2=GPIO ext0, 4=timer, 0=not from sleep)\n",
                (int)esp_reset_reason(),
                (int)esp_sleep_get_wakeup_cause());
  Serial.printf("  sdk         %s\n", ESP.getSdkVersion());
  line();
  // The module part number is the one thing the chip cannot tell you.
  // 16 MB of flash and 8 MB of PSRAM together are what an N16R8 looks
  // like; anything else means the build configuration is wrong, or the
  // module fitted is not the one the design assumed.
}

struct PinRow { const char *name; int pin; const char *meaning; };

static const PinRow INPUTS[] = {
  {"DOOR_NC",      PIN_DOOR,       "polarity is a config item, not a given"},
  {"ACC_INT1",     PIN_ACC_INT1,   "LIS2DW12, idle until configured"},
  {"NFC_GPO",      PIN_NFC_GPO,    "ST25DV, idle until configured"},
  {"POWER_GOOD_N", PIN_PG_N,       "LOW = BQ25601 sees valid input"},
  {"PMIC_IRQ_N",   PIN_PMIC_IRQ_N, "shared BQ25601 INT# / HUSB238A INT_N"},
  {"EPD_BUSY",     PIN_EPD_BUSY,   "meaningless while the panel rail is off"},
};

static void cmdPins() {
  line();
  for (const PinRow &r : INPUTS) {
    // Only an RTC-capable pin can pull the chip out of deep sleep, so
    // whether a given interrupt can serve as a wake source is a
    // property of the pin number, decided when the board was laid out
    // and not something firmware can work around later.
    Serial.printf("  GPIO%-2d  %-13s %s  %s  %s\n", r.pin, r.name,
                  digitalRead(r.pin) ? "HIGH" : "LOW ",
                  rtc_gpio_is_valid_gpio((gpio_num_t)r.pin) ? "wake-capable"
                                                            : "no deep-sleep wake",
                  r.meaning);
  }
  Serial.printf("\n  rails: epd %s   sd/gnss %s   led %s\n",
                railEpd ? "ON " : "off", railSd ? "ON " : "off",
                railLed ? "ON " : "off");
  line();
}

struct Expect { uint8_t addr; const char *name; };

static const Expect EXPECTED[] = {
  {ADDR_LIS2DW12,  "LIS2DW12 accelerometer"},
  {ADDR_MAX17048,  "MAX17048 fuel gauge"},
  {ADDR_INA226,    "INA226 current monitor"},
  {ADDR_HUSB238A,  "HUSB238A PD sink"},
  {ADDR_ST25_USER, "ST25DV04KC user/dynamic"},
  {ADDR_ST25_SYS,  "ST25DV04KC system"},
  {ADDR_PCF8523,   "PCF8523 RTC"},
  {ADDR_BQ25601,   "BQ25601 charger"},
};

static void cmdScan() {
  line();
  int found = 0, extra = 0;
  bool seen[128] = {false}, readable[128] = {false};
  for (uint8_t a = 1; a < 127; a++) {
    readable[a] = i2cReadable(a);
    seen[a] = i2cPresent(a) || readable[a];
  }
  for (const Expect &e : EXPECTED) {
    bool ok = seen[e.addr];
    Serial.printf("  0x%02X  %-26s %s\n", e.addr, e.name,
                  ok ? (readable[e.addr] ? "ack (write + read)"
                                         : "ack (zero-length write only)")
                     : "NO ANSWER");
    if (ok) found++;
  }
  for (uint8_t a = 1; a < 127; a++) {
    bool expected = false;
    for (const Expect &e : EXPECTED) expected |= (e.addr == a);
    if (seen[a] && !expected) {
      Serial.printf("  0x%02X  unexpected: %s\n", a,
                    readable[a]
                        ? "answers a read too, so something is really there"
                        : "write-ack only, never a read: a scan artefact");
      extra++;
    }
  }
  Serial.printf("\n  %d of %d expected, %d unexpected\n",
                found, (int)(sizeof(EXPECTED) / sizeof(EXPECTED[0])), extra);
  if (found == 0) {
    Serial.println("  nothing at all answered: suspect SDA/SCL, the pull-ups,");
    Serial.println("  or 3V3_MAIN before suspecting any single part");
  }
  line();
}

// Presence only says the address decoder answered. These read the part's
// own identity register, which also proves the data path both ways.
static void cmdId() {
  uint8_t u8;
  uint16_t u16;
  line();

  if (i2cRead8(ADDR_LIS2DW12, 0x0F, &u8))
    Serial.printf("  LIS2DW12  WHO_AM_I   0x%02X   %s\n", u8,
                  u8 == 0x44 ? "correct" : "EXPECTED 0x44");
  else Serial.println("  LIS2DW12  no answer");

  if (i2cRead16be(ADDR_MAX17048, 0x08, &u16))
    Serial.printf("  MAX17048  VERSION    0x%04X\n", u16);
  else Serial.println("  MAX17048  no answer");

  if (i2cRead16be(ADDR_INA226, 0xFE, &u16))
    Serial.printf("  INA226    MANUF_ID   0x%04X   %s\n", u16,
                  u16 == 0x5449 ? "'TI'" : "EXPECTED 0x5449");
  else Serial.println("  INA226    no answer");
  if (i2cRead16be(ADDR_INA226, 0xFF, &u16))
    Serial.printf("  INA226    DIE_ID     0x%04X   %s\n", u16,
                  u16 == 0x2260 ? "correct" : "EXPECTED 0x2260");

  // The HUSB238A has no identity register the public datasheet pins down,
  // so this is presence plus whatever its first status byte says. Treat
  // the value as something to look up, not as a pass mark.
  if (i2cRead8(ADDR_HUSB238A, 0x00, &u8))
    Serial.printf("  HUSB238A  REG0x00    0x%02X   (no documented ID reg)\n", u8);
  else Serial.println("  HUSB238A  no answer");

  if (i2cPresent(ADDR_ST25_USER) || i2cPresent(ADDR_ST25_SYS))
    Serial.printf("  ST25DV    user 0x%02X %s   system 0x%02X %s\n",
                  ADDR_ST25_USER, i2cPresent(ADDR_ST25_USER) ? "ack" : "-- ",
                  ADDR_ST25_SYS, i2cPresent(ADDR_ST25_SYS) ? "ack" : "--");
  else Serial.println("  ST25DV    no answer at either address");

  // Seconds register bit 7 is the oscillator-stopped flag: set means the
  // crystal has not been running, so the time it holds is not time.
  if (i2cRead8(ADDR_PCF8523, 0x03, &u8))
    Serial.printf("  PCF8523   SECONDS    0x%02X   OS=%d %s\n", u8, u8 >> 7,
                  (u8 >> 7) ? "<- clock has stopped, time is not valid" : "running");
  else Serial.println("  PCF8523   no answer");

  if (i2cRead8(ADDR_BQ25601, 0x0B, &u8))
    Serial.printf("  BQ25601   REG0B      0x%02X   part no. and revision -- the charging state lives in REG08, see power\n", u8);
  else Serial.println("  BQ25601   no answer");
  line();
}

// MAX6675: 16 bits, MSB first, no command phase. D2 set means the
// thermocouple is open. A disconnected probe must never be reported as
// 0 degrees -- that is a reading, and this is the absence of one.
static uint16_t tcRead() {
  spi3.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TC_CS, LOW);
  delayMicroseconds(2);
  uint16_t raw = spi3.transfer16(0x0000);
  digitalWrite(PIN_TC_CS, HIGH);
  spi3.endTransaction();
  return raw;
}

static void cmdTemp() {
  bool was = railEpd;
  if (!was) { setRailEpd(true); delay(250); }   // the part needs ~220 ms

  // The conversion that is in progress when the part powers up is not a
  // measurement. On this board the first frame after the rail came on
  // read 175.5 C and the next three read 47, 46.75 and 46 -- so the first
  // one is thrown away and a fresh conversion waited for.
  if (!was) { tcRead(); delay(250); }
  uint16_t raw = tcRead();

  line();
  Serial.printf("  raw         0x%04X\n", raw);
  if (raw == 0x0000 || raw == 0xFFFF) {
    Serial.println("  no data: the bus read all-zeros or all-ones, which is");
    Serial.println("  what an unpowered part or a dead MISO line looks like");
  } else if (raw & 0x0004) {
    Serial.println("  THERMOCOUPLE OPEN -- probe not connected");
    Serial.println("  (this is a fault, not 0.0 C)");
  } else {
    Serial.printf("  temperature %.2f C\n", (raw >> 3) * 0.25f);
  }
  line();
  if (!was) setRailEpd(false);
}

// Measured on the board, not taken from the part number: this
// ATGM336H ships at 115200. At 9600 it still sends -- 488 bytes of
// repeating nonsense in twelve seconds -- which is the trap, because
// bytes arriving looks like the wiring question is answered.
static const int GNSS_BAUD = 115200;

static void cmdGnss(int seconds, int baud) {
  bool was = railSd;
  if (!was) { setRailSd(true); delay(300); }
  Serial1.begin(baud, SERIAL_8N1, PIN_GNSS_RX, PIN_GNSS_TX);

  line();
  Serial.printf("  listening on GPIO%d at %d for %d s\n", PIN_GNSS_RX, baud, seconds);
  Serial.println("  a cold module with no sky sends empty sentences: that is");
  Serial.println("  still a pass for wiring. No bytes at all is the failure.");
  uint32_t end = millis() + (uint32_t)seconds * 1000;
  size_t bytes = 0;
  while ((int32_t)(end - millis()) > 0) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      Serial.write(c);
      bytes++;
    }
    delay(5);
  }
  Serial1.end();
  Serial.printf("\n  %u bytes\n", (unsigned)bytes);
  if (bytes == 0) {
    Serial.println("  nothing received: check the rail, and check that MCU RX");
    Serial.printf("  is GPIO%d -- the net names in the schematic are written\n",
                  PIN_GNSS_RX);
    Serial.println("  from the module's side, which inverts the usual reading");
  }
  line();
  if (!was) setRailSd(false);
}

// ---- the four pixels ------------------------------------------------
//
// Three things can be wrong here and only one of them is "a dead LED".
//
// Order: the data line reaches LED4 first, so index 0 is the light on
// the side of the case and 1..3 are the three on the front. If that is
// the other way round, every rule in docs/led-design.md points at the
// wrong lamp and nothing in the firmware will ever notice.
//
// Colour order: LED4 is an XL-4020RGBC-2812B and the front three are
// SK6812MINI-E. Different parts, and a controller that wants its bytes
// as GRB will show green when asked for red if the library sends RGB.
// Mixed parts on one chain make it entirely possible for index 0 to
// disagree with 1..3, which is why each pixel is asked for each primary
// on its own rather than the whole chain being flashed white.
//
// Current: four pixels at full white is most of a watt, so the test
// runs at a fraction of that and says so.
static const uint8_t RGB_BRIGHT = 60;   // of 255, for the timed sequence

// A steady light that nothing else is allowed to overwrite. The tap
// watch also drives pixel 0, and a probe on a lamp that changes under
// you measures nothing.
static bool ledHold = false;

static const char *LED_WHO[LED_COUNT] = {
  "LED4  side of the case, charge status",
  "LED1  front left,  cargo",
  "LED2  front mid,   alive",
  "LED3  front right, device",
};

// Powering the rail and talking to the pixels are two different things,
// and the order matters. begin() only sets up the data line; a pixel
// that had no supply when the first frame arrived never latched it and
// sits dark while the firmware believes it is lit. So the rail comes up
// first, is given time to settle, and only then is the chain started.
static void ledsReady() {
  const bool wasDark = !railLed;
  if (wasDark) setRailLed(true);
  if (wasDark || !ledsBegun) {
    delay(50);               // rail rise, plus the pixels' own reset time
    leds.begin();
    ledsBegun = true;
    leds.clear();
    leds.show();             // a known state before anything is asked for
    delay(5);
  }
  leds.setBrightness(RGB_BRIGHT);
}

static void cmdRgb(int hold) {
  struct Colour { const char *name; uint8_t r, g, b; };
  static const Colour C[] = {
    {"RED", 255, 0, 0}, {"GREEN", 0, 255, 0}, {"BLUE", 0, 0, 255}};

  line();
  Serial.printf("  LED_PWR_EN is GPIO%d; LOW turns the P-MOS on\n", PIN_LED_PWR_EN);
  Serial.printf("  rail before  %s\n",
                digitalRead(PIN_LED_PWR_EN) ? "HIGH (off)" : "LOW (on)");
  ledsReady();
  Serial.printf("  rail after   %s\n",
                digitalRead(PIN_LED_PWR_EN) ? "HIGH (off)" : "LOW (on)");
  Serial.printf("  brightness %d/255, %d ms a step\n", RGB_BRIGHT, hold);
  Serial.println("  watch the board and check each line against what lights up");
  Serial.println();

  for (int i = 0; i < LED_COUNT; i++) {
    for (const Colour &c : C) {
      leds.clear();
      leds.setPixelColor(i, leds.Color(c.r, c.g, c.b));
      leds.show();
      Serial.printf("  index %d  %-38s %s\n", i, LED_WHO[i], c.name);
      Serial.flush();
      delay(hold);
    }
  }

  // All four together: this is the one that shows a chain break, because
  // a pixel that never got its bytes stays dark while its neighbours lit.
  leds.clear();
  for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, leds.Color(255, 255, 255));
  leds.show();
  Serial.println("\n  all four WHITE together");
  Serial.flush();
  delay(hold * 2);

  leds.clear();
  leds.show();
  Serial.println("  off");
  Serial.println();
  Serial.println("  what to look for:");
  Serial.println("   - index 0 is the side light, not a front one");
  Serial.println("   - every named colour matches what you saw; if index 0");
  Serial.println("     swaps two of them the two part numbers want their");
  Serial.println("     bytes in a different order and the chain needs");
  Serial.println("     splitting into two strips in the driver");
  Serial.println("   - nothing stays dark, and nothing flickers");
  line();
}

static void cmdLed(int r, int g, int b) {
  ledsReady();                    // bring-up only: four pixels at full
  for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, leds.Color(r, g, b));
  leds.show();                    // white is most of a watt
  Serial.printf("  all four pixels -> r%d g%d b%d (brightness capped at 40/255)\n",
                r, g, b);
}

static void cmdLedOne(int i, int r, int g, int b) {
  if (i < 0 || i >= LED_COUNT) { Serial.println("  index 0..3"); return; }
  ledsReady();
  leds.setPixelColor(i, leds.Color(r, g, b));
  leds.show();
  Serial.printf("  index %d  %s  ->  r%d g%d b%d\n", i, LED_WHO[i], r, g, b);
}

static void cmdHold(int r, int g, int b, int bright) {
  ledsReady();
  if (bright < 1) bright = 1;
  if (bright > 255) bright = 255;
  leds.setBrightness((uint8_t)bright);
  for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, leds.Color(r, g, b));
  leds.show();
  ledHold = (r || g || b);
  line();
  Serial.printf("  all four pixels -> r%d g%d b%d at brightness %d/255\n",
                r, g, b, bright);
  Serial.printf("  rail GPIO%d is %s\n", PIN_LED_PWR_EN,
                digitalRead(PIN_LED_PWR_EN) ? "HIGH (off)" : "LOW (on)");
  if (ledHold) {
    Serial.println("  holding. Nothing else will change these until you do;");
    Serial.println("  `hold 0 0 0` turns them off. Note that opening a serial");
    Serial.println("  monitor resets the board, which also puts them out.");
  } else {
    Serial.println("  off, and the hold is released");
  }
  line();
}

static void cmdLedWalk() {
  ledsReady();
  // Index 0 is LED4, the one on the side. If the side light is not the
  // first to come up, the chain is wired in the other order and every
  // LED rule in docs/led-design.md is off by one.
  const char *who[LED_COUNT] = {"LED4 side/charge", "LED1 cargo",
                                "LED2 alive", "LED3 device"};
  for (int i = 0; i < LED_COUNT; i++) {
    leds.clear();
    leds.setPixelColor(i, leds.Color(0, 60, 0));
    leds.show();
    Serial.printf("  index %d  %s\n", i, who[i]);
    delay(900);
  }
  leds.clear();
  leds.show();
}

static void cmdBuzz(int ms) {
  Serial.printf("  buzzer %d ms at 2.7 kHz\n", ms);
  tone(PIN_BUZZER, 2700, ms);
  delay(ms + 20);
  noTone(PIN_BUZZER);
  // tone() hands the pin to LEDC; until it is claimed back as a plain
  // GPIO, driving it low only logs "IO 6 is not set as GPIO".
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
}

static void cmdChg(bool allow) {
  digitalWrite(PIN_CHG_CE_N, allow ? LOW : HIGH);
  Serial.printf("  CHG_CE_N %s -- charging %s\n", allow ? "LOW" : "HIGH",
                allow ? "allowed" : "disabled");
}

// ---- PCF8523 --------------------------------------------------------
//
// Three separate questions, and only the third one is about timekeeping:
//
//  * does the part answer        -- already known, it acks at 0x68
//  * is the crystal oscillating  -- the OS flag answers this, and only
//                                   after being cleared and looked at
//                                   again. Clearing it on a dead
//                                   oscillator makes it come straight
//                                   back, which is the whole test.
//  * does it keep time           -- read, wait, read again
//
// The schematic note asks for CAP_SEL = 1, the 12.5 pF setting. Get it
// wrong and the oscillator still runs, just at the wrong rate -- a
// clock that drifts minutes a week and nothing anywhere reports a
// fault. It is set here every time the time is set.
#define PCF_CONTROL_1 0x00
#define PCF_CONTROL_3 0x02
#define PCF_SECONDS   0x03

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

struct RtcTime { int year, mon, day, hour, min, sec; bool osFlag; };

static bool rtcRead(RtcTime *t) {
  uint8_t b[7];
  if (!i2cRead(ADDR_PCF8523, PCF_SECONDS, b, 7)) return false;
  t->osFlag = (b[0] & 0x80) != 0;
  t->sec  = bcd2bin(b[0] & 0x7F);
  t->min  = bcd2bin(b[1] & 0x7F);
  t->hour = bcd2bin(b[2] & 0x3F);
  t->day  = bcd2bin(b[3] & 0x3F);
  t->mon  = bcd2bin(b[5] & 0x1F);
  t->year = 2000 + bcd2bin(b[6]);
  return true;
}

static void cmdRtc() {
  uint8_t c1 = 0, c3 = 0;
  RtcTime t;
  line();
  if (!i2cRead8(ADDR_PCF8523, PCF_CONTROL_1, &c1) ||
      !i2cRead8(ADDR_PCF8523, PCF_CONTROL_3, &c3) || !rtcRead(&t)) {
    Serial.println("  no answer from the RTC");
    line();
    return;
  }
  Serial.printf("  Control_1  0x%02X   CAP_SEL %s, STOP %d, 24h %s\n", c1,
                (c1 & 0x80) ? "12.5 pF (as the schematic asks)"
                            : "7 pF <- the schematic asks for 12.5",
                (c1 >> 5) & 1, (c1 & 0x08) ? "off (12h)" : "on");
  Serial.printf("  Control_3  0x%02X   power mode %d, battery low %d,"
                " battery switch-over %d\n",
                c3, (c3 >> 5) & 7, (c3 >> 2) & 1, (c3 >> 3) & 1);
  Serial.printf("  time       %04d-%02d-%02d %02d:%02d:%02d\n",
                t.year, t.mon, t.day, t.hour, t.min, t.sec);
  Serial.printf("  OS flag    %d  %s\n", t.osFlag,
                t.osFlag ? "<- the oscillator has stopped, so this time is"
                           " not time"
                         : "clear, so the oscillator has been running");
  line();
}

static void cmdRtcSet(int y, int mo, int d, int h, int mi, int sec) {
  line();
  // CAP_SEL first, because changing the load capacitance disturbs the
  // oscillator; doing it after setting the time would cost the seconds
  // it takes to settle.
  if (!i2cWrite8(ADDR_PCF8523, PCF_CONTROL_1, 0x80)) {
    Serial.println("  could not write Control_1");
    line();
    return;
  }
  uint8_t b[7] = {
      bin2bcd((uint8_t)sec),            // writing bit 7 as 0 clears OS
      bin2bcd((uint8_t)mi), bin2bcd((uint8_t)h), bin2bcd((uint8_t)d),
      0x00,                             // weekday, not used here
      bin2bcd((uint8_t)mo), bin2bcd((uint8_t)(y - 2000))};
  Wire.beginTransmission(ADDR_PCF8523);
  Wire.write(PCF_SECONDS);
  Wire.write(b, 7);
  if (Wire.endTransmission() != 0) {
    Serial.println("  the time write was not acknowledged");
    line();
    return;
  }
  Serial.printf("  set to %04d-%02d-%02d %02d:%02d:%02d, CAP_SEL 12.5 pF\n",
                y, mo, d, h, mi, sec);

  // OS was just cleared. If the crystal is not oscillating the part sets
  // it again within a second, so reading it back is the test.
  delay(1200);
  RtcTime t;
  if (rtcRead(&t)) {
    Serial.printf("  read back  %04d-%02d-%02d %02d:%02d:%02d, OS %d  %s\n",
                  t.year, t.mon, t.day, t.hour, t.min, t.sec, t.osFlag,
                  t.osFlag ? "<- OS came straight back: the 32.768 kHz crystal"
                             " is not oscillating"
                           : "OS stayed clear, so the crystal is running");
  }
  line();
}

// Keeping time is not the same as having been set. Two reads a known
// distance apart is the only way to see the difference.
static void cmdRtcTick(int seconds) {
  RtcTime a, b;
  line();
  if (!rtcRead(&a)) { Serial.println("  no answer"); line(); return; }
  Serial.printf("  %02d:%02d:%02d  waiting %d s\n", a.hour, a.min, a.sec, seconds);
  Serial.flush();
  const uint32_t t0 = millis();
  delay((uint32_t)seconds * 1000);
  const uint32_t elapsed = millis() - t0;
  if (!rtcRead(&b)) { Serial.println("  no answer"); line(); return; }

  const long before = a.hour * 3600L + a.min * 60L + a.sec;
  long after = b.hour * 3600L + b.min * 60L + b.sec;
  if (after < before) after += 86400L;        // rolled past midnight
  const long moved = after - before;
  Serial.printf("  %02d:%02d:%02d  moved %ld s while the MCU measured %lu ms\n",
                b.hour, b.min, b.sec, moved, (unsigned long)elapsed);
  if (moved == 0) {
    Serial.println("  the clock did not advance at all: the oscillator is dead");
  } else if (labs(moved - (long)seconds) <= 1) {
    Serial.println("  the RTC and the MCU agree, so it is keeping time");
  } else {
    Serial.println("  <- the two disagree by more than a second. Suspect the");
    Serial.println("     crystal load capacitance (CAP_SEL) before the part.");
  }
  line();
}

// ---- LIS2DW12 -------------------------------------------------------
//
// WHO_AM_I only proves the address decoder works. These read the axes,
// because the one measurement that can validate an accelerometer with
// no reference equipment is gravity: a part sitting still on a bench
// must report a vector of one g, whatever direction it points. A
// magnitude that is not 1000 mg means the scale, the data format or the
// register map is wrong, and no amount of staring at raw counts shows
// that.
#define LIS_CTRL1       0x20
#define LIS_CTRL2       0x21
#define LIS_CTRL3       0x22
#define LIS_CTRL4_INT1  0x23
#define LIS_CTRL6       0x25
#define LIS_OUT_X_L     0x28
#define LIS_WAKE_UP_THS 0x34
#define LIS_WAKE_UP_DUR 0x35
#define LIS_WAKE_UP_SRC 0x38
#define LIS_ALL_INT_SRC 0x3B
#define LIS_STATUS      0x27
#define LIS_CTRL7       0x3F

// At +-2 g in high-performance mode the output is 14 bits left-justified
// in 16, so the raw word is shifted down by two before scaling.
static const float LIS_MG_PER_LSB = 0.244f;

static bool accConfigure() {
  // BDU so a sample cannot be read half updated, and address
  // auto-increment so all six output bytes come from one burst.
  if (!i2cWrite8(ADDR_LIS2DW12, LIS_CTRL2, 0x0C)) return false;
  if (!i2cWrite8(ADDR_LIS2DW12, LIS_CTRL6, 0x00)) return false;   // +-2 g
  if (!i2cWrite8(ADDR_LIS2DW12, LIS_CTRL1, 0x54)) return false;   // 100 Hz, HP
  delay(20);
  return true;
}

static bool accRead(float *x, float *y, float *z) {
  uint8_t b[6];
  if (!i2cRead(ADDR_LIS2DW12, LIS_OUT_X_L, b, 6)) return false;
  const int16_t rx = (int16_t)((uint16_t)b[1] << 8 | b[0]);
  const int16_t ry = (int16_t)((uint16_t)b[3] << 8 | b[2]);
  const int16_t rz = (int16_t)((uint16_t)b[5] << 8 | b[4]);
  *x = (rx >> 2) * LIS_MG_PER_LSB;
  *y = (ry >> 2) * LIS_MG_PER_LSB;
  *z = (rz >> 2) * LIS_MG_PER_LSB;
  return true;
}

static void cmdAcc() {
  line();
  if (!accConfigure()) {
    Serial.println("  could not configure: the part did not accept a write");
    line();
    return;
  }
  float x, y, z, sx = 0, sy = 0, sz = 0;
  int n = 0;
  for (int i = 0; i < 16; i++) {
    if (accRead(&x, &y, &z)) { sx += x; sy += y; sz += z; n++; }
    delay(15);
  }
  if (!n) { Serial.println("  no samples came back"); line(); return; }
  x = sx / n; y = sy / n; z = sz / n;
  const float mag = sqrtf(x * x + y * y + z * z);

  Serial.printf("  X %+8.1f mg\n  Y %+8.1f mg\n  Z %+8.1f mg\n", x, y, z);
  Serial.printf("  magnitude %.1f mg over %d samples\n", mag, n);
  if (mag > 850 && mag < 1150) {
    Serial.println("  that is one g, so the scale and the data format are right");
  } else {
    Serial.println("  <- this should be about 1000 mg while the board sits still.");
    Serial.println("     Anything else means the range, the shift or the register");
    Serial.println("     map is wrong, not that the part is broken.");
  }
  // Which way is down tells the firmware how the part is mounted, which
  // every later orientation and shock rule depends on.
  const char *axis = (fabsf(z) > fabsf(x) && fabsf(z) > fabsf(y)) ? "Z"
                   : (fabsf(y) > fabsf(x)) ? "Y" : "X";
  const float v = (axis[0] == 'Z') ? z : (axis[0] == 'Y') ? y : x;
  Serial.printf("  gravity is on %s, %s -- note this against how the board sits\n",
                axis, v < 0 ? "negative" : "positive");
  line();
}

static void cmdAccLive(int seconds) {
  line();
  if (!accConfigure()) {
    Serial.println("  could not configure the part");
    line();
    return;
  }
  Serial.printf("  streaming for %d s -- tilt the board and watch which axis moves\n",
                seconds);
  Serial.println("        X       Y       Z     |v|");
  const uint32_t end = millis() + (uint32_t)seconds * 1000;
  while ((int32_t)(end - millis()) > 0) {
    float x, y, z;
    if (accRead(&x, &y, &z)) {
      Serial.printf("  %+7.0f %+7.0f %+7.0f  %6.0f\n", x, y, z,
                    sqrtf(x * x + y * y + z * z));
    } else {
      Serial.println("  read failed");
    }
    Serial.flush();
    delay(250);
  }
  line();
}

// The product needs motion to wake the device and shock to be recorded,
// and both are this one interrupt. INT1 goes to GPIO2.
// Proving the INT1 trace without depending on the wake-up state
// machine. Data-ready is the one interrupt with no threshold, no filter
// path and no sleep logic behind it: configure an output rate, read the
// samples, and the pin must toggle at that rate. If it does, the trace
// and the pad are good and any wake-up trouble is configuration. If it
// does not, no amount of threshold tuning will ever help.
//
// Worth doing because the wake-up registers have now been set correctly
// twice -- read back byte for byte -- and still produced nothing, which
// means the fault is in what those bytes are understood to mean, not in
// whether they arrived.
static void cmdAccInt(int seconds) {
  line();
  if (!accConfigure()) { Serial.println("  config failed"); line(); return; }
  i2cWrite8(ADDR_LIS2DW12, LIS_CTRL3, 0x00);       // not latched: pulse per sample
  i2cWrite8(ADDR_LIS2DW12, LIS_CTRL4_INT1, 0x01);  // INT1_DRDY
  i2cWrite8(ADDR_LIS2DW12, LIS_CTRL7, 0x20);       // interrupts enabled
  delay(20);

  uint8_t c4 = 0, c7 = 0;
  i2cRead8(ADDR_LIS2DW12, LIS_CTRL4_INT1, &c4);
  i2cRead8(ADDR_LIS2DW12, LIS_CTRL7, &c7);
  Serial.printf("  CTRL4_INT1=0x%02X CTRL7=0x%02X, output rate 100 Hz\n", c4, c7);
  Serial.printf("  counting edges on GPIO%d for %d s -- expect about 100 a\n",
                PIN_ACC_INT1, seconds);
  Serial.println("  second, and no need to touch the board");

  // Watch the pin first and only then read the sample. The other way
  // round clears data-ready before the pin is sampled, so most of the
  // pulses are gone by the time they are looked for -- which is why the
  // first attempt counted 18 a second on a part producing 100.
  // Data-ready latches high until the sample is read. Start counting
  // with it already high and there is never a rising edge to see --
  // which is how the corrected loop managed to report zero on a pin
  // that the previous one had just watched toggle 55 times.
  float x, y, z;
  accRead(&x, &y, &z);
  delay(2);

  const uint32_t t0 = millis();
  int rising = 0, last = digitalRead(PIN_ACC_INT1);
  while (millis() - t0 < (uint32_t)seconds * 1000) {
    const int now = digitalRead(PIN_ACC_INT1);
    if (now && !last) {
      rising++;
      accRead(&x, &y, &z);        // reading the sample is what clears DRDY
    }
    last = now;
  }
  const int edges = rising;
  const float perSec = edges / (float)seconds;
  Serial.printf("\n  %d rising edges, %.0f a second (the part is set to 100)\n",
                edges, perSec);
  // Any repeatable pulse train at all settles the wiring question. The
  // exact rate depends on how fast this loop can keep up, so it is not
  // the thing being measured.
  if (perSec > 2) {
    Serial.println("  INT1 reaches the MCU: the trace, the pad and the pin are");
    Serial.println("  all good, so the wake-up silence is a configuration");
    Serial.println("  question and not a wiring one");
  } else {
    Serial.printf("  the pin never moved while the part was producing samples."
                  " INT1 is not getting from the sensor to GPIO%d.\n",
                  PIN_ACC_INT1);
  }
  i2cWrite8(ADDR_LIS2DW12, LIS_CTRL4_INT1, 0x00);
  i2cWrite8(ADDR_LIS2DW12, LIS_CTRL7, 0x00);
  line();
}

static void cmdAccWake(int seconds, int thresh) {
  line();
  if (!accConfigure()) { Serial.println("  config failed"); line(); return; }
  if (thresh < 1) thresh = 1;
  if (thresh > 63) thresh = 63;

  // LIR latches the interrupt until its source register is read. Without
  // it the pin pulses for about one output period, and a polling loop
  // can step straight over it -- which looks exactly like a trace that
  // was never connected.
  i2cWrite8(ADDR_LIS2DW12, LIS_CTRL3, 0x10);       // LIR
  i2cWrite8(ADDR_LIS2DW12, LIS_WAKE_UP_DUR, 0x00);
  i2cWrite8(ADDR_LIS2DW12, LIS_WAKE_UP_THS, (uint8_t)thresh);
  i2cWrite8(ADDR_LIS2DW12, LIS_CTRL4_INT1, 0x20);  // wake-up drives INT1
  i2cWrite8(ADDR_LIS2DW12, LIS_CTRL7, 0x20);       // interrupts enabled
  delay(20);

  // Read the configuration back. A write that the part refused and a
  // part that simply never triggers produce the same silence.
  const uint8_t regs[] = {LIS_CTRL1, LIS_CTRL2, LIS_CTRL3, LIS_CTRL4_INT1,
                          LIS_CTRL6, LIS_CTRL7, LIS_WAKE_UP_THS, LIS_WAKE_UP_DUR};
  const char *names[] = {"CTRL1", "CTRL2", "CTRL3", "CTRL4_INT1",
                         "CTRL6", "CTRL7", "WAKE_UP_THS", "WAKE_UP_DUR"};
  Serial.print("  config read back: ");
  for (int i = 0; i < 8; i++) {
    uint8_t v = 0;
    i2cRead8(ADDR_LIS2DW12, regs[i], &v);
    Serial.printf("%s=0x%02X ", names[i], v);
  }
  Serial.println();

  Serial.printf("  threshold %d = %d mg, INT1 on GPIO%d, %d s\n",
                thresh, thresh * 31, PIN_ACC_INT1, seconds);
  Serial.println("  the source register is polled as well as the pin, so a");
  Serial.println("  trigger that never reaches the MCU still shows up here");
  Serial.println("  tap the board, or pick it up and put it down");

  uint8_t src = 0, all0 = 0;
  i2cRead8(ADDR_LIS2DW12, LIS_WAKE_UP_SRC, &src);   // clear anything stale
  i2cRead8(ADDR_LIS2DW12, LIS_ALL_INT_SRC, &all0);
  Serial.printf("  at rest: WAKE_UP_SRC 0x%02X  ALL_INT_SRC 0x%02X  INT1 pin %s\n",
                src, all0, digitalRead(PIN_ACC_INT1) ? "HIGH" : "LOW");

  const uint32_t t0 = millis();
  int pinEvents = 0, regEvents = 0, last = digitalRead(PIN_ACC_INT1);
  while (millis() - t0 < (uint32_t)seconds * 1000) {
    const int now = digitalRead(PIN_ACC_INT1);
    const bool pinRose = (now && !last);
    last = now;

    // Any non-zero source counts. Deciding in advance which bit means
    // wake-up is how the last run reported nothing while the part may
    // well have been firing -- the register layout is not worth
    // guessing at when the raw byte answers the question outright.
    uint8_t all = 0;
    const bool okS = i2cRead8(ADDR_LIS2DW12, LIS_WAKE_UP_SRC, &src);
    const bool okA = i2cRead8(ADDR_LIS2DW12, LIS_ALL_INT_SRC, &all);
    const bool regFired = (okS && src) || (okA && all);
    if (pinRose) pinEvents++;
    if (regFired) regEvents++;
    if (pinRose || regFired) {
      Serial.printf("  %6lu ms  %-9s  WAKE_UP_SRC 0x%02X  ALL_INT_SRC 0x%02X\n",
                    (unsigned long)(millis() - t0),
                    pinRose ? (regFired ? "pin + reg" : "pin only") : "reg only",
                    src, all);
      Serial.flush();
    }
    delay(5);
  }

  Serial.printf("\n  %d from the register, %d from the pin\n",
                regEvents, pinEvents);
  if (regEvents && !pinEvents) {
    Serial.println("  the part detects motion but GPIO2 never moves: the INT1");
    Serial.println("  trace or its pad is the problem, not the sensor");
  } else if (!regEvents) {
    Serial.printf("  nothing detected at all. Try a lower threshold:"
                  " acc wake %d 4\n", seconds);
  } else {
    Serial.println("  both agree, so the interrupt path is good end to end");
  }
  i2cWrite8(ADDR_LIS2DW12, LIS_CTRL7, 0x00);
  line();
}

// Wake-up through ST's own driver rather than hand-written registers.
//
// The hand-written version was not wrong -- every bit position in it
// matches lis2dw12_reg.h, and the sequence matches ST's
// Enable_Wake_Up_Detection step for step. Using the library anyway buys
// two things worth having: the configuration is the manufacturer's, so
// it stops being something to re-derive every time this is revisited,
// and Get_Event_Status reads the source registers through the same
// definitions, so there is no second place for a bit to be misread.
//
// The sensor runs this detector on its own. That is the whole point: it
// costs about a microamp, and the ESP32 can be in deep sleep the entire
// time, which is the only way the power budget in the requirements
// works out.
static LIS2DW12Sensor stAcc(&Wire, LIS2DW12_I2C_ADD_L);
static bool stAccUp = false;

// Armed once, then watched from the main loop for as long as the board
// is powered. Every test so far has needed the tap and the watching to
// happen in the same few seconds, which means a result of zero says
// nothing about the sensor and everything about the timing. This way
// the tap can happen whenever, the LED answers on the spot, and the
// count is still there to read afterwards.
static const uint8_t WAKE_THRESHOLD = 2;   // about 62 mg at +-2 g
static bool wakeWatch = false;
static uint32_t wakeCount = 0;
static uint32_t wakeLastMs = 0;

static bool stAccBegin(uint8_t thr) {
  if (!stAccUp) {
    if (stAcc.begin() != LIS2DW12_STATUS_OK) return false;
    if (stAcc.Enable_X() != LIS2DW12_STATUS_OK) return false;
    stAccUp = true;
  }
  if (stAcc.Enable_Wake_Up_Detection() != LIS2DW12_STATUS_OK) return false;
  if (stAcc.Set_Wake_Up_Threshold(thr) != LIS2DW12_STATUS_OK) return false;
  return true;
}

static void cmdStWake(int seconds, int thr) {
  line();
  if (thr < 1) thr = 1;
  if (thr > 63) thr = 63;
  if (!stAccBegin((uint8_t)thr)) {
    Serial.println("  the ST driver would not bring the part up");
    line();
    return;
  }

  uint8_t c4 = 0, c7 = 0, ths = 0;
  i2cRead8(ADDR_LIS2DW12, LIS_CTRL4_INT1, &c4);
  i2cRead8(ADDR_LIS2DW12, LIS_CTRL7, &c7);
  i2cRead8(ADDR_LIS2DW12, LIS_WAKE_UP_THS, &ths);
  Serial.printf("  CTRL4_INT1=0x%02X CTRL7=0x%02X WAKE_UP_THS=0x%02X\n", c4, c7, ths);
  Serial.printf("  threshold %d = about %d mg at +-2 g\n", thr, thr * 31);
  Serial.printf("\n  >>> TAP THE BOARD. Watching INT1 on GPIO%d for %d seconds.\n\n",
                PIN_ACC_INT1, seconds);
  Serial.flush();

  LIS2DW12_Event_Status_t st;
  stAcc.Get_Event_Status(&st);        // clear anything already pending

  const uint32_t t0 = millis();
  int pinEvents = 0, libEvents = 0, last = digitalRead(PIN_ACC_INT1);
  while (millis() - t0 < (uint32_t)seconds * 1000) {
    const int now = digitalRead(PIN_ACC_INT1);
    const bool pinRose = (now && !last);
    last = now;

    bool lib = false;
    if (stAcc.Get_Event_Status(&st) == LIS2DW12_STATUS_OK && st.WakeUpStatus) {
      lib = true;
    }
    if (pinRose) pinEvents++;
    if (lib) libEvents++;
    if (pinRose || lib) {
      Serial.printf("  %6lu ms  %s\n", (unsigned long)(millis() - t0),
                    (pinRose && lib) ? "pin + driver"
                                     : pinRose ? "pin only" : "driver only");
      Serial.flush();
    }
    delay(5);
  }

  Serial.printf("\n  %d from the driver, %d from the pin\n", libEvents, pinEvents);
  if (libEvents && pinEvents) {
    Serial.println("  motion detection works end to end. The sensor can now be");
    Serial.println("  left to watch on its own while the MCU sleeps.");
  } else if (libEvents) {
    Serial.println("  the sensor detects motion but the pin did not follow");
  } else {
    Serial.println("  nothing detected -- if the board really was tapped, try a");
    Serial.printf("  lower threshold: stwake %d 2\n", seconds);
  }
  line();
}

// The architecture test. Everything above says the parts work; this says
// the system works: the MCU stops, the sensor keeps watch alone, and a
// tap brings the MCU back. Wake reason 2 in the banner after this is the
// proof -- GPIO, not the power-on that a crash or a brownout would give.
static void cmdSleep(int thr) {
  if (thr < 1) thr = 1;
  if (thr > 63) thr = 63;
  line();
  if (!stAccBegin((uint8_t)thr)) {
    Serial.println("  cannot arm the sensor, so sleeping would be a one-way trip");
    line();
    return;
  }
  Serial.printf("  armed at threshold %d (about %d mg)\n", thr, thr * 31);

  // Everything off before stopping: a rail left on is current drawn for
  // no reason, and the whole point of this is the current.
  setRailEpd(false);
  setRailSd(false);
  leds.clear();
  if (ledsBegun) leds.show();
  setRailLed(false);

  Serial.println("  going into deep sleep now. USB will drop and the port will");
  Serial.println("  disappear -- that is expected. TAP THE BOARD to wake it.");
  Serial.println("  It will re-enumerate and print its banner with reset");
  Serial.println("  reason 2, which only a GPIO wake gives.");
  Serial.flush();
  delay(200);

  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_ACC_INT1, 1);   // wake on HIGH
  esp_deep_sleep_start();
}

// ---- e-paper --------------------------------------------------------
//
// This panel is a raw display: the boost that makes its gate voltages
// (L7, Q3, MBR0530, PREVGH/PREVGL) is on the main board, not on a driver
// module, and the controller brings it up itself during initialisation.
// So the order here is: rail, then reset, then look at BUSY -- and only
// once the controller has been seen to answer is a frame sent. A panel
// that is not talking should be found with a pin, not by waiting out a
// 25-second refresh that was never going to happen.
//
// BUSY polarity is written down as LOW = busy, HIGH = ready, with a note
// to confirm it on real hardware. The probe below reports the edges it
// actually sees rather than asserting which is which.

static bool epdBegun = false;

static void cmdEpdBusy(int seconds) {
  const bool was = railEpd;
  if (!was) setRailEpd(true);
  delay(60);

  line();
  Serial.printf("  rail GPIO%d %s\n", PIN_EPD_PWR_EN,
                digitalRead(PIN_EPD_PWR_EN) ? "HIGH (on)" : "LOW (off)");

  // RST has a 10k pull-up to the PANEL rail, so letting go of the pin
  // and reading it asks a question no other test does: does the rail
  // actually arrive at the flex? A LOW here means the panel end has no
  // supply, or the line is shorted, and nothing further is worth trying.
  pinMode(PIN_EPD_RST, INPUT);
  delay(5);
  const int rstIdle = digitalRead(PIN_EPD_RST);
  Serial.printf("  RST  GPIO%d floats %s  %s\n", PIN_EPD_RST,
                rstIdle ? "HIGH" : "LOW",
                rstIdle ? "(the 10k pull-up has the panel rail, so the flex"
                          " is powered)"
                        : "<- the pull-up is not seeing the rail: no supply at"
                          " the flex, or a short");

  // Is anything on the other end of BUSY at all? The internal pulls are
  // about 45k, so a controller driving the line wins easily and the
  // reading will not budge. A line that follows the pull instead has
  // nothing on it: an unseated flex, or a pad that is not soldered.
  // Without this, "stuck busy" and "not connected" look identical.
  pinMode(PIN_EPD_BUSY, INPUT_PULLUP);
  delay(5);
  const int withUp = digitalRead(PIN_EPD_BUSY);
  pinMode(PIN_EPD_BUSY, INPUT_PULLDOWN);
  delay(5);
  const int withDown = digitalRead(PIN_EPD_BUSY);
  pinMode(PIN_EPD_BUSY, INPUT);
  delay(5);
  Serial.printf("  BUSY GPIO%d  pull-up reads %s, pull-down reads %s  ->  %s\n",
                PIN_EPD_BUSY, withUp ? "HIGH" : "LOW",
                withDown ? "HIGH" : "LOW",
                (withUp != withDown)
                    ? "FLOATING, nothing is driving it"
                    : (withUp ? "held HIGH by the panel" : "held LOW by the panel"));

  pinMode(PIN_EPD_RST, OUTPUT);
  digitalWrite(PIN_EPD_RST, HIGH);
  delay(20);

  // Start sampling BEFORE the pulse. The first probe called the level it
  // found after releasing reset the baseline and so counted the one edge
  // that mattered as no change at all.
  int last = digitalRead(PIN_EPD_BUSY);
  uint32_t t0 = millis();
  Serial.printf("  %6lu ms  BUSY %s   (before the reset pulse)\n", 0UL,
                last ? "HIGH" : "LOW");

  digitalWrite(PIN_EPD_RST, LOW);
  delay(20);
  digitalWrite(PIN_EPD_RST, HIGH);
  Serial.printf("  %6lu ms  reset pulsed\n",
                (unsigned long)(millis() - t0));

  int edges = 0;
  while (millis() - t0 < (uint32_t)seconds * 1000) {
    int now = digitalRead(PIN_EPD_BUSY);
    if (now != last) {
      Serial.printf("  %6lu ms  BUSY -> %s\n",
                    (unsigned long)(millis() - t0), now ? "HIGH" : "LOW");
      Serial.flush();
      last = now;
      edges++;
    }
    delayMicroseconds(500);
  }

  Serial.printf("\n  %d edge%s in %d s, now %s\n", edges,
                edges == 1 ? "" : "s", seconds, last ? "HIGH" : "LOW");
  Serial.println("  the spec says LOW = busy, HIGH = ready, to be confirmed here");
  line();
  if (!was) setRailEpd(false);
}

// Two init parameters are worth varying before anyone reaches for a
// multimeter. The reset pulse defaults to 2 ms, which is shorter than
// most panel datasheets ask for; and RST on this board carries a 10k
// pull-up to the panel rail, which is the situation GxEPD2's pulldown
// reset mode exists for -- it drives the line low and then releases it
// rather than driving it high.
// Test patterns. Each one answers a question the others cannot.
//
//  1  inks      every colour the panel can make, side by side
//  2  geometry  where the edges really are, and which rows reach the glass
//  3  text      how small type survives on this panel at this size
//
// Pattern 2 earns its place: the controller has 128 rows of RAM and the
// 2.13" glass shows fewer, so a layout drawn to the full buffer loses
// its bottom silently. Ticks along the edges say exactly where the
// visible area stops instead of leaving it to be guessed at.
static void epdPaintInks() {
#if EPD_INKS == 1
  const uint16_t inks[] = {GxEPD_BLACK, GxEPD_WHITE};
#elif EPD_INKS == 3
  const uint16_t inks[] = {GxEPD_BLACK, GxEPD_RED, GxEPD_WHITE};
#else
  const uint16_t inks[] = {GxEPD_BLACK, GxEPD_RED, GxEPD_YELLOW, GxEPD_WHITE};
#endif
  const int n = sizeof(inks) / sizeof(inks[0]);
  const int w = epd.width() / n;
  for (int i = 0; i < n; i++) epd.fillRect(i * w, 0, w, epd.height() / 2, inks[i]);
  epd.drawRect(0, 0, epd.width(), epd.height(), GxEPD_BLACK);
  epd.setTextColor(GxEPD_BLACK);
  epd.setCursor(6, epd.height() - 10);
  epd.print("mCOLD bringup");
}

static void epdPaintGeometry() {
  const int w = epd.width(), h = epd.height();
  epd.drawRect(0, 0, w, h, GxEPD_BLACK);          // the very edge

  // Corner brackets: if one is missing, that corner is off the glass.
  for (int i = 0; i < 4; i++) {
    const int x = (i & 1) ? w - 1 : 0, y = (i & 2) ? h - 1 : 0;
    const int dx = (i & 1) ? -1 : 1, dy = (i & 2) ? -1 : 1;
    for (int k = 0; k < 12; k++) {
      epd.drawPixel(x + dx * k, y, GxEPD_BLACK);
      epd.drawPixel(x, y + dy * k, GxEPD_BLACK);
    }
  }

  // A tick every 10 px along the top, longer every 50, numbered.
  epd.setTextColor(GxEPD_BLACK);
  epd.setTextSize(1);
  for (int x = 0; x < w; x += 10) {
    const bool big = (x % 50 == 0);
    epd.drawFastVLine(x, 0, big ? 10 : 5, GxEPD_BLACK);
    if (big && x) { epd.setCursor(x + 2, 20); epd.print(x); }
  }
  // And down the left, which is the axis that gets clipped.
  for (int y = 0; y < h; y += 10) {
    const bool big = (y % 50 == 0);
    epd.drawFastHLine(0, y, big ? 10 : 5, GxEPD_BLACK);
    if (big && y) { epd.setCursor(13, y + 4); epd.print(y); }
  }

  // The last four rows the buffer has. Count what you can see: that is
  // how many of the controller's rows actually reach the glass.
  for (int k = 1; k <= 4; k++) {
    epd.drawFastHLine(w / 2, h - k, w / 2 - 4, GxEPD_BLACK);
  }
  epd.setCursor(w / 2 - 70, h - 12);
  epd.print("rows from bottom:");
  epd.drawLine(0, 0, w - 1, h - 1, GxEPD_BLACK);
}

static void epdPaintText() {
  epd.setTextColor(GxEPD_BLACK);
  epd.drawRect(0, 0, epd.width(), epd.height(), GxEPD_BLACK);
  int y = 14;
  for (int size = 1; size <= 3; size++) {
    epd.setTextSize(size);
    epd.setCursor(6, y);
    epd.printf("%dx MCOLD 4.2C", size);
    y += 10 * size + 6;
  }
  epd.setTextSize(1);
  // Reversed text is the hard case on e-paper: ink spreads into the
  // white, and thin strokes close up.
  epd.fillRect(6, y, epd.width() - 12, 16, GxEPD_BLACK);
  epd.setTextColor(GxEPD_WHITE);
  epd.setCursor(10, y + 11);
  epd.print("white on black  MCOLD-0117  14:32");
  epd.setTextColor(GxEPD_BLACK);
}

static void cmdEpdPattern(int which) {
  const bool was = railEpd;
  if (!was) { setRailEpd(true); delay(60); }
  if (!epdBegun) {
    epd.epd2.selectSPI(spi3, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    epd.init(115200, true, 20, false);
    epdBegun = true;
  }
  epd.setRotation(1);
  epd.setTextSize(1);

  const uint32_t t0 = millis();
  epd.setFullWindow();
  epd.firstPage();
  do {
    epd.fillScreen(GxEPD_WHITE);
    if (which == 2) epdPaintGeometry();
    else if (which == 3) epdPaintText();
    else epdPaintInks();
  } while (epd.nextPage());

  line();
  Serial.printf("  pattern %d on a %d x %d panel, %d ink%s, %lu ms\n", which,
                epd.width(), epd.height(), EPD_INKS, EPD_INKS == 1 ? "" : "s",
                (unsigned long)(millis() - t0));
  if (which == 2) {
    Serial.println("  edge-to-edge border, a bracket in each corner, ticks every");
    Serial.println("  10 px numbered every 50, and four rules stacked at the");
    Serial.println("  bottom right. Count the rules you can actually see: that");
    Serial.println("  is how much of the buffer reaches the glass.");
  } else if (which == 3) {
    Serial.println("  three type sizes, then white on black -- the case where");
    Serial.println("  ink spread closes up thin strokes");
  } else {
    Serial.println("  one band per ink, a border, and text at the bottom left");
  }
  line();
  if (!was) setRailEpd(false);
}

static void cmdEpdTest(int resetMs, bool pulldown) {
  const bool was = railEpd;
  if (!was) { setRailEpd(true); delay(60); }

  line();
  Serial.printf("  reset %d ms, %s reset mode\n", resetMs,
                pulldown ? "pulldown (release, do not drive high)" : "push-pull");
  epd.epd2.selectSPI(spi3, SPISettings(4000000, MSBFIRST, SPI_MODE0));
  epd.init(115200, true, (uint16_t)resetMs, pulldown);
  epdBegun = true;
  Serial.printf("  BUSY after init: %s\n",
                digitalRead(PIN_EPD_BUSY) ? "HIGH (ready)" : "LOW (still busy)");
  epd.setRotation(1);
  Serial.printf("  panel       %d x %d, %d inks, fast partial %d\n",
                epd.width(), epd.height(), EPD_INKS,
                epd.epd2.hasFastPartialUpdate);

  // Four bands, one per ink the panel can make, plus a label. If a band
  // is missing or two of them come out the same, the ink mapping is
  // wrong and no amount of layout work will fix it.
  uint32_t t0 = millis();
  epd.setFullWindow();
  epd.firstPage();
  do {
    epd.fillScreen(GxEPD_WHITE);
    // One band per ink the panel can actually make. Two bands that come
    // out the same colour means the mapping is wrong, and no amount of
    // layout work downstream will fix that.
#if EPD_INKS == 1
    const uint16_t inks[] = {GxEPD_BLACK, GxEPD_WHITE};
#elif EPD_INKS == 3
    const uint16_t inks[] = {GxEPD_BLACK, GxEPD_RED, GxEPD_WHITE};
#else
    const uint16_t inks[] = {GxEPD_BLACK, GxEPD_RED, GxEPD_YELLOW, GxEPD_WHITE};
#endif
    const int n = sizeof(inks) / sizeof(inks[0]);
    const int w = epd.width() / n;
    for (int i = 0; i < n; i++) {
      epd.fillRect(i * w, 0, w, epd.height() / 2, inks[i]);
    }
    epd.drawRect(0, 0, epd.width(), epd.height(), GxEPD_BLACK);
    epd.setTextColor(GxEPD_BLACK);
    epd.setCursor(6, epd.height() - 10);
    epd.print("mCOLD bringup");
  } while (epd.nextPage());
  uint32_t ms = millis() - t0;

  Serial.printf("  full refresh %lu ms\n", (unsigned long)ms);
#if EPD_INKS == 1
  Serial.println("  expect two bands across the top half:  black | white");
#elif EPD_INKS == 3
  Serial.println("  expect three bands across the top half:  black | red | white");
#else
  Serial.println("  expect four bands:  black | red | yellow | white");
#endif
  Serial.println("  a border all the way round, and text at the bottom left");
  line();
  if (!was) setRailEpd(false);
}

// ---- the power chain ------------------------------------------------
//
// Four parts have to agree about one battery: the fuel gauge says how
// full it is, the current monitor says which way charge is moving, the
// charger says what it is doing about it, and the PD sink says what the
// cable is willing to give. Reading them together is the only way to see
// when one of them is lying.

static const float INA_SHUNT_OHMS = 0.010f;   // R15
static const float INA_SHUNT_LSB_V = 2.5e-6f;
static const float INA_BUS_LSB_V = 1.25e-3f;

static void cmdPower() {
  uint8_t u8;
  uint16_t u16;
  line();

  // MAX17048: cell voltage and state of charge, straight from the part.
  if (i2cRead16be(ADDR_MAX17048, 0x02, &u16))
    Serial.printf("  cell        %.3f V\n", u16 * 78.125e-6f);
  else Serial.println("  cell        MAX17048 no answer");
  if (i2cRead16be(ADDR_MAX17048, 0x04, &u16))
    Serial.printf("  charge      %.1f %%\n", (u16 >> 8) + (u16 & 0xFF) / 256.0f);
  if (i2cRead16be(ADDR_MAX17048, 0x16, &u16))
    Serial.printf("  rate        %+.2f %%/hr\n", (int16_t)u16 * 0.208f);

  // INA226 across R15. The sign is the whole point: positive is charge
  // going in, negative is the board living off the cell.
  if (i2cRead16be(ADDR_INA226, 0x01, &u16)) {
    float v = (int16_t)u16 * INA_SHUNT_LSB_V;
    Serial.printf("  shunt       %+.3f mV  ->  %+.1f mA %s\n", v * 1000.0f,
                  v / INA_SHUNT_OHMS * 1000.0f,
                  v > 0 ? "(into the cell)" : "(out of the cell)");
  } else Serial.println("  shunt       INA226 no answer");
  if (i2cRead16be(ADDR_INA226, 0x02, &u16))
    Serial.printf("  bus         %.3f V\n", u16 * INA_BUS_LSB_V);

  // BQ25601 status. REG08 is the system status register; REG0B is part
  // information and says nothing about charging -- reading the second
  // one and calling it status is a mistake this tool made once already.
  static const char *VBUS[] = {"no input", "USB host SDP", "adapter",
                               "reserved", "reserved", "reserved",
                               "reserved", "OTG"};
  static const char *CHRG[] = {"not charging", "pre-charge",
                               "fast charging", "termination done"};
  if (i2cRead8(ADDR_BQ25601, 0x08, &u8)) {
    Serial.printf("  charger     REG08 0x%02X  input: %s  state: %s\n", u8,
                  VBUS[(u8 >> 5) & 7], CHRG[(u8 >> 3) & 3]);
    Serial.printf("              power-good %d  thermal-reg %d  vsysmin-reg %d\n",
                  (u8 >> 2) & 1, (u8 >> 1) & 1, u8 & 1);
  } else Serial.println("  charger     BQ25601 no answer");
  if (i2cRead8(ADDR_BQ25601, 0x09, &u8)) {
    Serial.printf("  faults      REG09 0x%02X%s\n", u8, u8 ? "" : "  (clear)");
    // The charger keeps an I2C watchdog of its own. Let it expire and
    // every register goes back to its reset value -- so a charge
    // current set at boot quietly becomes the default one later, and
    // nothing announces it but this bit.
    if (u8 & 0x80) Serial.println("              WATCHDOG expired: the charger has"
                                  " reset its own registers to defaults");
    if (u8 & 0x40) Serial.println("              BOOST fault");
    if ((u8 >> 4) & 3) Serial.printf("              CHARGE fault %d (1=input,"
                                     " 2=thermal shutdown, 3=safety timer)\n",
                                     (u8 >> 4) & 3);
    if (u8 & 0x08) Serial.println("              BATTERY over-voltage");
    if (u8 & 0x07) Serial.printf("              NTC fault %d\n", u8 & 7);
  }
  if (i2cRead8(ADDR_BQ25601, 0x0B, &u8))
    Serial.printf("  part        REG0B 0x%02X  (part number and revision)\n", u8);

  Serial.printf("  CHG_CE_N    %s -- charging %s\n",
                digitalRead(PIN_CHG_CE_N) ? "HIGH" : "LOW",
                digitalRead(PIN_CHG_CE_N) ? "disabled" : "allowed");
  Serial.printf("  POWER_GOOD  GPIO%d %s\n", PIN_PG_N,
                digitalRead(PIN_PG_N) ? "HIGH (no valid input)" : "LOW (valid)");
  line();
}

// ---- ST25DV NFC tag -------------------------------------------------
//
// The user EEPROM is addressed with SIXTEEN bits, not the usual eight.
// That is also why a one-byte-register probe of this part comes back
// empty at every address: the tag reads the register number as the high
// half of a memory address and waits for the other half.

static bool st25Write(uint16_t addr, const uint8_t *data, uint8_t n) {
  Wire.beginTransmission(ADDR_ST25_USER);
  Wire.write((uint8_t)(addr >> 8));
  Wire.write((uint8_t)(addr & 0xFF));
  Wire.write(data, n);
  if (Wire.endTransmission() != 0) return false;
  // The tag stops acknowledging its own address while the EEPROM write
  // cycle runs. Polling for the ack to come back is how you know it
  // finished -- more honest than guessing a delay, and quicker than a
  // safe one.
  uint32_t end = millis() + 50;
  while ((int32_t)(end - millis()) > 0) {
    Wire.beginTransmission(ADDR_ST25_USER);
    if (Wire.endTransmission() == 0) return true;
    delay(1);
  }
  return false;
}

static bool st25Read(uint8_t dev, uint16_t addr, uint8_t *buf, size_t n) {
  Wire.beginTransmission(dev);
  Wire.write((uint8_t)(addr >> 8));
  Wire.write((uint8_t)(addr & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)dev, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static void bleMacString(char *out, size_t n) {
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_BT);
  snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void defaultSn(char *out, size_t n) {
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(out, n, "MCOLD-%02X%02X", m[4], m[5]);
}

// Writes an NFC Forum Type 5 capability container followed by one NDEF
// Text record, so a phone can read it with no app installed. The product
// will want its own record type eventually; what this proves today is
// that the tag takes a write, keeps it, and hands it back unchanged.
static void cmdNfcWrite(const char *snArg) {
  char sn[24], mac[18];
  if (snArg && *snArg) snprintf(sn, sizeof(sn), "%s", snArg);
  else defaultSn(sn, sizeof(sn));
  bleMacString(mac, sizeof(mac));

  char text[64];
  int tlen = snprintf(text, sizeof(text), "%s;BLE=%s", sn, mac);

  uint8_t buf[96];
  int i = 0;
  buf[i++] = 0xE1;                  // CC magic: an NDEF tag
  buf[i++] = 0x40;                  // v1.0, read and write both unrestricted
  buf[i++] = 512 / 8;               // MLEN: the 04KC has 512 user bytes
  buf[i++] = 0x01;                  // supports read-multiple-block
  buf[i++] = 0x03;                  // TLV type: NDEF message
  int lenAt = i++;                  // TLV length, filled in below
  int ndefAt = i;
  buf[i++] = 0xD1;                  // MB+ME+SR, TNF 1 = NFC Forum well-known
  buf[i++] = 0x01;                  // type length
  buf[i++] = (uint8_t)(tlen + 3);   // payload: status byte + "en" + the text
  buf[i++] = 'T';                   // Text record
  buf[i++] = 0x02;                  // UTF-8, two-character language code
  buf[i++] = 'e';
  buf[i++] = 'n';
  memcpy(buf + i, text, tlen);
  i += tlen;
  buf[lenAt] = (uint8_t)(i - ndefAt);
  buf[i++] = 0xFE;                  // TLV terminator

  line();
  Serial.printf("  SN          %s\n", sn);
  Serial.printf("  BLE MAC     %s\n", mac);
  Serial.printf("  payload     %s\n", text);
  Serial.printf("  writing     %d bytes to user memory at 0x0000\n", i);

  // Four bytes at a time: inside any page boundary this part might have,
  // so a write cannot silently wrap around within a page.
  bool ok = true;
  for (int off = 0; off < i && ok; off += 4) {
    uint8_t n = (uint8_t)min(4, i - off);
    ok = st25Write((uint16_t)off, buf + off, n);
    if (!ok) Serial.printf("  FAILED at offset %d\n", off);
  }
  if (!ok) { line(); return; }

  uint8_t back[96];
  if (!st25Read(ADDR_ST25_USER, 0, back, i)) {
    Serial.println("  wrote, but the read back did not complete");
    line();
    return;
  }
  int bad = 0;
  for (int k = 0; k < i; k++) if (back[k] != buf[k]) bad++;
  Serial.printf("  verify      %s (%d of %d bytes differ)\n",
                bad ? "FAILED" : "matches", bad, i);
  line();
}

// Writes the tag as a URI record instead of text, so tapping the box
// opens a page rather than showing a string. The identity travels in the
// query, which means the same tap serves a phone with the app installed
// and one without: the app reads the parameters, anyone else gets a web
// page that can tell them what the box is and who to call.
//
// The URI record abbreviates its own scheme: one byte stands in for
// "https://" and friends. Worth doing on a tag with 512 bytes, and it is
// what a phone expects to find.
static const struct { uint8_t code; const char *prefix; } URI_PREFIX[] = {
  {0x02, "https://www."}, {0x01, "http://www."},
  {0x04, "https://"},     {0x03, "http://"},
};

static void cmdNfcUrl(const char *base, const char *snArg) {
  char sn[24], mac[18];
  if (snArg && *snArg) snprintf(sn, sizeof(sn), "%s", snArg);
  else defaultSn(sn, sizeof(sn));
  bleMacString(mac, sizeof(mac));

  char macPlain[13];
  int j = 0;
  for (const char *p = mac; *p && j < 12; p++) if (*p != ':') macPlain[j++] = *p;
  macPlain[j] = 0;

  char url[160];
  snprintf(url, sizeof(url), "%s?sn=%s&ble=%s", base, sn, macPlain);

  uint8_t code = 0x00;
  const char *rest = url;
  for (auto &p : URI_PREFIX) {
    size_t n = strlen(p.prefix);
    if (!strncmp(url, p.prefix, n)) { code = p.code; rest = url + n; break; }
  }
  int rlen = (int)strlen(rest);

  uint8_t buf[200];
  int i = 0;
  buf[i++] = 0xE1;                  // CC magic
  buf[i++] = 0x40;                  // v1.0, read and write unrestricted
  buf[i++] = 512 / 8;               // MLEN for the 04KC
  buf[i++] = 0x01;
  buf[i++] = 0x03;                  // NDEF message TLV
  int lenAt = i++;
  int ndefAt = i;
  buf[i++] = 0xD1;                  // MB+ME+SR, well-known type
  buf[i++] = 0x01;                  // type length
  buf[i++] = (uint8_t)(rlen + 1);   // payload: prefix code + the rest
  buf[i++] = 'U';                   // URI record
  buf[i++] = code;
  memcpy(buf + i, rest, rlen);
  i += rlen;
  buf[lenAt] = (uint8_t)(i - ndefAt);
  buf[i++] = 0xFE;                  // terminator

  line();
  Serial.printf("  url         %s\n", url);
  Serial.printf("  prefix      0x%02X, so %d bytes are stored, not %d\n",
                code, rlen, (int)strlen(url));
  Serial.printf("  writing     %d bytes to user memory at 0x0000\n", i);

  bool ok = true;
  for (int off = 0; off < i && ok; off += 4) {
    uint8_t n = (uint8_t)min(4, i - off);
    ok = st25Write((uint16_t)off, buf + off, n);
    if (!ok) Serial.printf("  FAILED at offset %d\n", off);
  }
  if (!ok) { line(); return; }

  // The old record was longer than some new ones will be. Whatever is
  // left behind the terminator is dead bytes the phone never reaches,
  // but a hex dump of the tag that still shows the previous payload
  // invites ten minutes of wondering which one is live.
  uint8_t blank[4] = {0, 0, 0, 0};
  for (int off = i; off < i + 48; off += 4) st25Write((uint16_t)off, blank, 4);

  uint8_t back[200];
  if (!st25Read(ADDR_ST25_USER, 0, back, i)) {
    Serial.println("  wrote, but the read back did not complete");
    line();
    return;
  }
  int bad = 0;
  for (int k = 0; k < i; k++) if (back[k] != buf[k]) bad++;
  Serial.printf("  verify      %s (%d of %d bytes differ)\n",
                bad ? "FAILED" : "matches", bad, i);
  line();
}

// Two records in one message: a URI first, then the JSON as its own MIME
// record.
//
// iOS only raises a notification for a record it knows how to act on,
// and in practice that means an http(s) URI. A Text record is read and
// then silently discarded, which is why the tag written earlier looked
// dead to a phone while being perfectly correct over I2C. A MIME record
// has the same problem on its own -- so the URI goes first to get the
// banner, and the structured copy rides along behind it where CoreNFC
// and Android both hand it to the app as its own record.
//
// The app therefore never has to parse a query string, and a phone with
// no app installed still has somewhere to land.
static void cmdNfcJson(const char *base, const char *snArg) {
  char sn[24], mac[18];
  if (snArg && *snArg) snprintf(sn, sizeof(sn), "%s", snArg);
  else defaultSn(sn, sizeof(sn));
  bleMacString(mac, sizeof(mac));

  char macPlain[13];
  int j = 0;
  for (const char *p = mac; *p && j < 12; p++) if (*p != ':') macPlain[j++] = *p;
  macPlain[j] = 0;

  char url[160];
  snprintf(url, sizeof(url), "%s?sn=%s&ble=%s", base, sn, macPlain);

  // Two fields, both of them asked for. The schema belongs to the app
  // team, so nothing is invented here on their behalf; JSON readers
  // ignore keys they do not know, so it extends without breaking them.
  char json[160];
  int jlen = snprintf(json, sizeof(json), "{\"sn\":\"%s\",\"ble\":\"%s\"}",
                      sn, mac);

  // A base of "-" means no URI record at all, just the JSON. Worth
  // having so the two layouts can be compared on the bench rather than
  // argued about: without the URI, iOS raises no banner.
  const bool withUri = !(base && base[0] == '-' && base[1] == 0);

  uint8_t code = 0x00;
  const char *rest = url;
  for (auto &p : URI_PREFIX) {
    size_t n = strlen(p.prefix);
    if (!strncmp(url, p.prefix, n)) { code = p.code; rest = url + n; break; }
  }
  int rlen = (int)strlen(rest);

  static const char MIME[] = "application/json";
  const int mlen = (int)sizeof(MIME) - 1;

  uint8_t buf[300];
  int i = 0;
  buf[i++] = 0xE1;                  // CC magic
  buf[i++] = 0x40;                  // v1.0, read and write unrestricted
  buf[i++] = 512 / 8;               // MLEN for the 04KC
  buf[i++] = 0x01;
  buf[i++] = 0x03;                  // NDEF message TLV
  int lenAt = i++;
  int ndefAt = i;

  // Record 1 -- URI. MB set, ME clear: more records follow.
  if (withUri) {
    buf[i++] = 0x91;                // MB | SR | TNF 1 (well-known)
    buf[i++] = 0x01;
    buf[i++] = (uint8_t)(rlen + 1);
    buf[i++] = 'U';
    buf[i++] = code;
    memcpy(buf + i, rest, rlen);
    i += rlen;
  }

  // The JSON. ME is always set here; MB as well when it stands alone.
  buf[i++] = withUri ? 0x52 : 0xD2; // (MB) | ME | SR | TNF 2 (MIME)
  buf[i++] = (uint8_t)mlen;
  buf[i++] = (uint8_t)jlen;
  memcpy(buf + i, MIME, mlen);
  i += mlen;
  memcpy(buf + i, json, jlen);
  i += jlen;

  buf[lenAt] = (uint8_t)(i - ndefAt);
  buf[i++] = 0xFE;                  // terminator

  line();
  if (withUri) Serial.printf("  url         %s\n", url);
  else Serial.println("  url         none -- JSON only, so no iOS banner");
  Serial.printf("  json        %s\n", json);
  Serial.printf("  records     %s, %d bytes of NDEF\n",
                withUri ? "2 (URI then application/json)"
                        : "1 (application/json)",
                i - ndefAt - 1);
  Serial.printf("  writing     %d bytes to user memory at 0x0000\n", i);

  bool ok = true;
  for (int off = 0; off < i && ok; off += 4) {
    uint8_t n = (uint8_t)min(4, i - off);
    ok = st25Write((uint16_t)off, buf + off, n);
    if (!ok) Serial.printf("  FAILED at offset %d\n", off);
  }
  if (!ok) { line(); return; }

  uint8_t blank[4] = {0, 0, 0, 0};
  for (int off = i; off < i + 32; off += 4) st25Write((uint16_t)off, blank, 4);

  uint8_t back[300];
  if (!st25Read(ADDR_ST25_USER, 0, back, i)) {
    Serial.println("  wrote, but the read back did not complete");
    line();
    return;
  }
  int bad = 0;
  for (int k = 0; k < i; k++) if (back[k] != buf[k]) bad++;
  Serial.printf("  verify      %s (%d of %d bytes differ)\n",
                bad ? "FAILED" : "matches", bad, i);
  line();
}

// The configuration area, read straight out rather than interpreted.
// RF_MNGT sits in here, and if the RF interface had been switched off in
// the factory config a phone would read nothing while I2C still worked
// perfectly -- which would send the search to the antenna, the wrong
// place entirely. Only IC_REF and the UID are labelled; the rest is for
// comparing against the datasheet, because asserting a register map from
// memory is how you end up confidently reading the wrong byte.
static void cmdNfcSys() {
  uint8_t b[32];
  line();
  if (!st25Read(ADDR_ST25_SYS, 0x0000, b, 32)) {
    Serial.println("  system area did not read");
    line();
    return;
  }
  for (int k = 0; k < 32; k++) {
    if (k % 16 == 0) Serial.printf("\n  %04X: ", k);
    Serial.printf("%02X ", b[k]);
  }
  Serial.printf("\n\n  IC_REF   0x%02X\n", b[0x17]);
  Serial.print("  UID      ");
  for (int k = 0x1F; k >= 0x18; k--) Serial.printf("%02X", b[k]);
  Serial.println();
  line();
}

static void cmdNfcRead(int n) {
  // Big enough to hold a two-record message whole: a dump that silently
  // stops at 64 bytes looks like a tag that was written short.
  uint8_t b[192];
  if (n > (int)sizeof(b)) n = sizeof(b);
  line();
  // IC_REF and the UID live in the system area at the other address.
  // Reading them proves both halves of the part answer.
  if (st25Read(ADDR_ST25_SYS, 0x0017, b, 1))
    Serial.printf("  IC_REF      0x%02X\n", b[0]);
  else
    Serial.println("  IC_REF      system area did not read");
  if (st25Read(ADDR_ST25_SYS, 0x0018, b, 8)) {
    Serial.print("  UID         ");
    for (int k = 7; k >= 0; k--) Serial.printf("%02X", b[k]);   // stored LSB first
    Serial.println();
  }
  if (!st25Read(ADDR_ST25_USER, 0, b, n)) {
    Serial.println("  user memory did not read");
    line();
    return;
  }
  for (int k = 0; k < n; k++) {
    if (k % 16 == 0) Serial.printf("\n  %04X: ", k);
    Serial.printf("%02X ", b[k]);
  }
  Serial.print("\n  text:       ");
  for (int k = 0; k < n; k++) {
    Serial.print(b[k] >= 32 && b[k] < 127 ? (char)b[k] : '.');
  }
  Serial.println();
  line();
}

// Watch every dynamic register at once and report whatever moves.
//
// FIELD_ON is documented as bit 2 of EH_CTRL_Dyn, but that is the ST25DV
// map read from memory, and this board carries the KC variant. Rather
// than keep guessing at one bit, this dumps the whole block and prints
// only the bytes that change -- so the phone itself shows us which
// register answers to it, and the detector can then be pointed at the
// right one instead of the remembered one.
static void cmdDyn(int seconds) {
  uint8_t prev[8] = {0}, cur[8];
  bool have = false;
  int changes = 0, naks = 0;
  uint32_t t0 = millis(), end = t0 + (uint32_t)seconds * 1000;

  line();
  Serial.printf("  polling dynamic registers 0x2000..0x2007 for %d s\n", seconds);
  Serial.println("  hold the phone on the antenna; anything that moves prints");
  Serial.println("       ms   2000 2001 2002 2003 2004 2005 2006 2007");

  while ((int32_t)(end - millis()) > 0) {
    if (!st25Read(ADDR_ST25_USER, 0x2000, cur, 8)) {
      // Being turned away is itself an event: the RF side holds the tag.
      if (++naks == 3) Serial.printf("  %7lu   I2C refused -- the RF side has the tag\n",
                                     (unsigned long)(millis() - t0));
      delay(40);
      continue;
    }
    if (naks >= 3) Serial.printf("  %7lu   I2C back\n", (unsigned long)(millis() - t0));
    naks = 0;

    if (!have || memcmp(prev, cur, 8)) {
      Serial.printf("  %7lu  ", (unsigned long)(millis() - t0));
      for (int k = 0; k < 8; k++) {
        bool moved = have && cur[k] != prev[k];
        Serial.printf(moved ? " [%02X]" : "  %02X ", cur[k]);
      }
      Serial.println(have ? "" : "   <- baseline");
      if (have) changes++;
      memcpy(prev, cur, 8);
      have = true;
    }
    delay(40);
  }
  Serial.printf("\n  %d change%s seen\n", changes, changes == 1 ? "" : "s");
  if (!changes) {
    Serial.println("  nothing moved. The phone's field never reached the tag,");
    Serial.println("  or the RF interface is off in the factory configuration.");
  }
  line();
}

// ---- tap the tag, wake the radio ------------------------------------
//
// The tag tells the MCU that a phone is near without anyone configuring
// anything: EH_CTRL_Dyn is a dynamic register in the user device, and
// bit 2 of it is FIELD_ON. No password, no system-area write, no risk of
// locking a configuration byte on the only board we have.
//
// The GPO pin on GPIO1 would do the same thing as an interrupt and
// without polling, but switching it on means opening an I2C security
// session and writing the system configuration area, where a wrong byte
// can disable the RF interface or make a setting permanent. Polling
// first establishes that the event is really there; the pin can come
// later, once it is worth the risk.
#define ST25_EH_CTRL_DYN 0x2002
#define ST25_FIELD_ON    0x04      // EH_CTRL_Dyn: a field is present now
#define ST25_IT_STS_DYN  0x2005
#define ST25_FIELD_EDGE  0x18      // IT_STS_Dyn: field rose, field fell

// IT_STS_Dyn latches and clears on read, which is what makes it the
// right register to watch. FIELD_ON is only true while the phone is
// actually there, so a quick tap can fall between two polls and leave
// nothing behind; the latched edges cannot be missed that way. The first
// read of this board returned 0x18 with no phone in sight -- both edges
// still set from a tap minutes earlier, which is how we know the RF
// side works at all.
static volatile bool autoTap = true;
static uint32_t tapCount = 0;

static bool bleUp = false;

static void bleStart(const char *name) {
  if (bleUp) return;
  BLEDevice::init(name);
  BLEDevice::createServer();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  // Fast advertising: the phone is in someone's hand right now, and a
  // slow interval is most of the delay between the tap and the connect.
  adv->setMinInterval(0x20);        // 20 ms
  adv->setMaxInterval(0x40);        // 40 ms
  adv->setScanResponse(true);
  adv->start();
  bleUp = true;
}

static void bleStop() {
  if (!bleUp) return;
  BLEDevice::getAdvertising()->stop();
  BLEDevice::deinit(true);
  bleUp = false;
}

static void cmdBle(bool on) {
  char sn[24];
  defaultSn(sn, sizeof(sn));
  if (on) {
    bleStart(sn);
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_BT);
    Serial.printf("  advertising as \"%s\"  (%02X:%02X:%02X:%02X:%02X:%02X)\n",
                  sn, m[0], m[1], m[2], m[3], m[4], m[5]);
    Serial.println("  the name is the match key: iOS never sees a BLE MAC,"
                   " so the app");
    Serial.println("  has to find the device by the SN it read off the tag");
  } else {
    bleStop();
    Serial.println("  advertising stopped");
  }
}

static void cmdTap(int seconds) {
  char sn[24];
  defaultSn(sn, sizeof(sn));
  bleStop();

  line();
  Serial.printf("  watching the tag for an RF field for %d s\n", seconds);
  Serial.println("  hold a phone against the antenna; BLE comes up the moment");
  Serial.println("  the field appears, and this reports how long that took");

  uint32_t t0 = millis();
  uint32_t end = t0 + (uint32_t)seconds * 1000;
  bool wasOn = false;
  int nakRun = 0, taps = 0;

  while ((int32_t)(end - millis()) > 0) {
    uint8_t v;
    bool ok = st25Read(ADDR_ST25_USER, ST25_EH_CTRL_DYN, &v, 1);
    bool on;
    if (ok) {
      nakRun = 0;
      on = (v & ST25_FIELD_ON) != 0;
    } else {
      // While the RF side holds the tag, I2C gets turned away. A run of
      // refusals is itself the phone being there, so it counts as the
      // field being on rather than as a bus fault.
      on = (++nakRun >= 3);
    }

    if (on && !wasOn) {
      uint32_t at = millis() - t0;
      taps++;
      if (!bleUp) bleStart(sn);
      if (!ledHold) {
        ledsReady();
        leds.setPixelColor(0, leds.Color(0, 70, 0));
        leds.show();
      }
      Serial.printf("\n  %6lu ms  FIELD ON  -> BLE advertising as \"%s\"%s\n",
                    (unsigned long)at, sn,
                    ok ? "" : "  (detected by I2C being refused)");
    } else if (!on && wasOn) {
      Serial.printf("  %6lu ms  field gone, still advertising\n",
                    (unsigned long)(millis() - t0));
    }
    wasOn = on;
    delay(20);
  }

  Serial.printf("\n  %d tap%s seen; BLE is %s\n", taps, taps == 1 ? "" : "s",
                bleUp ? "up -- 'ble off' to stop it" : "still off");
  if (!taps) {
    Serial.println("  no field detected. Either the phone never reached the");
    Serial.println("  antenna, or FIELD_ON is not where this expects it:");
    Serial.printf("  EH_CTRL_Dyn at 0x%04X, bit 2\n", ST25_EH_CTRL_DYN);
  }
  line();
}

// For an address that answers but is not in the schematic's list. A part
// that is really there usually has structure in its first registers --
// all 0xFF or all 0x00 across the whole page is what a bus artefact or a
// write-only device looks like, not an identity.
static void cmdProbe(uint8_t addr, int count) {
  line();
  Serial.printf("  probing 0x%02X, registers 0x00..0x%02X\n", addr, count - 1);
  if (!i2cPresent(addr)) {
    Serial.println("  does not acknowledge its address");
    line();
    return;
  }
  int ok = 0, ff = 0, zero = 0;
  for (int r = 0; r < count; r++) {
    if (r % 16 == 0) Serial.printf("\n  %02X: ", r);
    uint8_t v;
    if (i2cRead8(addr, (uint8_t)r, &v)) {
      Serial.printf("%02X ", v);
      ok++;
      if (v == 0xFF) ff++;
      if (v == 0x00) zero++;
    } else {
      Serial.print(".. ");
    }
  }
  Serial.printf("\n\n  %d of %d readable", ok, count);
  if (ok == 0) {
    Serial.print("\n  plain read, no register pointer: ");
    int got = Wire.requestFrom((int)addr, 8);
    if (got <= 0) Serial.print("nothing");
    for (int k = 0; k < got; k++) Serial.printf("%02X ", Wire.read());
  }
  if (ok && ff == ok) Serial.print("  -- every byte 0xFF, so probably nothing real");
  else if (ok && zero == ok) Serial.print("  -- every byte 0x00, same conclusion");
  Serial.println();
  line();
}

static void help() {
  Serial.println();
  Serial.println("  info          chip, flash, PSRAM, MAC, reset reason");
  Serial.println("  pins          read every input, and the rail states");
  Serial.println("  scan          I2C bus scan against the expected eight");
  Serial.println("  id            identity register of each I2C part");
  Serial.println("  temp          MAX6675 thermocouple (powers the EPD rail)");
  Serial.println("  rtc           read the clock, its config and the OS flag");
  Serial.println("  rtc set YYYY-MM-DD HH:MM:SS   set it and prove the crystal");
  Serial.println("  rtc tick [s]  read, wait, read again: is it keeping time");
  Serial.println("  acc           read the axes once, check it against gravity");
  Serial.println("  acc live [s]  stream the axes so you can tilt the board");
  Serial.println("  acc int [s]   prove the INT1 trace with data-ready pulses");
  Serial.println("  stwake [s] [thr]  motion detection through ST own driver");
  Serial.println("  arm [thr]     watch for motion from now on; LED shows each hit");
  Serial.println("  wakelog       how many motion events since arming");
  Serial.println("  sleep [thr]   arm the sensor and deep sleep; a tap wakes it");
  Serial.println("  acc wake [s] [thr]  watch INT1 for motion, thr 1-63");
  Serial.println("  epd           reset the panel and watch BUSY; no image sent");
  Serial.println("  epd test [ms] [pd]  init (reset ms, pd = pulldown) and draw");
  Serial.println("  epd pat n     1 inks, 2 geometry and ruler, 3 type sizes");
  Serial.println("  gnss [s] [bd] dump NMEA (default 10 s at 115200)");
  Serial.println("  rgb [ms]      every pixel through red, green, blue, then white");
  Serial.println("  hold r g b [n]  all four on and left on, nothing overwrites them");
  Serial.println("  led r g b     all four pixels to a colour, 0-255");
  Serial.println("  led i r g b   one pixel, index 0-3");
  Serial.println("  walk          light each pixel in turn, named");
  Serial.println("  buzz [ms]     buzzer at 2.7 kHz (default 200)");
  Serial.println("  power         cell, current, charger state, PD state");
  Serial.println("  tap [s]       watch for an NFC field, bring BLE up on it");
  Serial.println("  dyn [s]       dump the tag dynamic registers, print what moves");
  Serial.println("  auto [off]    the always-on tap watch (on by default)");
  Serial.println("  ble [off]     start or stop BLE advertising");
  Serial.println("  nfc [n]       read the tag: IC_REF, UID, n bytes of user memory");
  Serial.println("  nfc write [SN]  write SN + BLE MAC as an NDEF text record");
  Serial.println("  nfc url [base] [SN]  write a URI record: base?sn=..&ble=..");
  Serial.println("  nfc json [base|-] [SN]  URI + application/json ( - = JSON only)");
  Serial.println("  nfc sys       dump the tag configuration area");
  Serial.println("  probe AA [n]  dump n registers from I2C address AA (hex)");
  Serial.println("  rail epd|sd|led on|off");
  Serial.println("  chg on|off    allow or disable battery charging");
  Serial.println("  all           info + pins + scan + id, nothing powered");
  Serial.println("  ?             this list");
  Serial.println();
}

// ---- command parsing ------------------------------------------------

static void handle(char *s) {
  while (*s == ' ') s++;
  if (!*s) return;

  char *argv[6] = {nullptr};
  int argc = 0;
  for (char *tok = strtok(s, " "); tok && argc < 6; tok = strtok(nullptr, " ")) {
    argv[argc++] = tok;
  }
  for (char *p = argv[0]; *p; p++) *p = (char)tolower((unsigned char)*p);
  const char *c = argv[0];

  if (!strcmp(c, "?") || !strcmp(c, "help")) { help(); return; }
  if (!strcmp(c, "info")) { cmdInfo(); return; }
  if (!strcmp(c, "pins")) { cmdPins(); return; }
  if (!strcmp(c, "scan")) { cmdScan(); return; }
  if (!strcmp(c, "id"))   { cmdId(); return; }
  if (!strcmp(c, "temp")) { cmdTemp(); return; }

  if (!strcmp(c, "rtc")) {
    if (argc > 1 && !strcmp(argv[1], "tick")) {
      cmdRtcTick(argc > 2 ? atoi(argv[2]) : 10);
    } else if (argc > 3 && !strcmp(argv[1], "set")) {
      int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
      if (sscanf(argv[2], "%d-%d-%d", &y, &mo, &d) == 3 &&
          sscanf(argv[3], "%d:%d:%d", &h, &mi, &se) == 3) {
        cmdRtcSet(y, mo, d, h, mi, se);
      } else {
        Serial.println("  rtc set YYYY-MM-DD HH:MM:SS");
      }
    } else {
      cmdRtc();
    }
    return;
  }
  if (!strcmp(c, "stwake")) {
    cmdStWake(argc > 1 ? atoi(argv[1]) : 20, argc > 2 ? atoi(argv[2]) : 2);
    return;
  }
  if (!strcmp(c, "sleep")) { cmdSleep(argc > 1 ? atoi(argv[1]) : 2); return; }

  if (!strcmp(c, "arm")) {
    const int thr = argc > 1 ? atoi(argv[1]) : 2;
    if (!stAccBegin((uint8_t)(thr < 1 ? 1 : thr > 63 ? 63 : thr))) {
      Serial.println("  the ST driver would not arm the part");
      return;
    }
    wakeWatch = true;
    wakeCount = 0;
    Serial.printf("  armed at threshold %d (about %d mg). Tap whenever you\n",
                  thr, thr * 31);
    Serial.println("  like -- the first pixel turns red on every event and");
    Serial.println("  `wakelog` prints the count. No need to time anything.");
    return;
  }

  if (!strcmp(c, "wakelog")) {
    Serial.printf("  watch %s, %lu event%s so far",
                  wakeWatch ? "armed" : "off", (unsigned long)wakeCount,
                  wakeCount == 1 ? "" : "s");
    if (wakeCount) Serial.printf(", last at %lu ms", (unsigned long)wakeLastMs);
    Serial.println();
    return;
  }
  if (!strcmp(c, "acc")) {
    if (argc > 1 && !strcmp(argv[1], "live"))
      cmdAccLive(argc > 2 ? atoi(argv[2]) : 15);
    else if (argc > 1 && !strcmp(argv[1], "int"))
      cmdAccInt(argc > 2 ? atoi(argv[2]) : 3);
    else if (argc > 1 && !strcmp(argv[1], "wake"))
      cmdAccWake(argc > 2 ? atoi(argv[2]) : 15, argc > 3 ? atoi(argv[3]) : 16);
    else cmdAcc();
    return;
  }
  if (!strcmp(c, "epd")) {
    if (argc > 1 && !strcmp(argv[1], "test"))
      cmdEpdTest(argc > 2 ? atoi(argv[2]) : 20,
                 argc > 3 && !strcmp(argv[3], "pd"));
    else if (argc > 1 && !strcmp(argv[1], "pat"))
      cmdEpdPattern(argc > 2 ? atoi(argv[2]) : 2);
    else cmdEpdBusy(argc > 1 ? atoi(argv[1]) : 10);
    return;
  }
  if (!strcmp(c, "walk")) { cmdLedWalk(); return; }
  if (!strcmp(c, "all"))  { cmdInfo(); cmdPins(); cmdScan(); cmdId(); return; }
  if (!strcmp(c, "gnss")) {
    cmdGnss(argc > 1 ? atoi(argv[1]) : 10, argc > 2 ? atoi(argv[2]) : GNSS_BAUD);
    return;
  }
  if (!strcmp(c, "buzz")) { cmdBuzz(argc > 1 ? atoi(argv[1]) : 200); return; }

  if (!strcmp(c, "power")) { cmdPower(); return; }
  if (!strcmp(c, "tap")) { cmdTap(argc > 1 ? atoi(argv[1]) : 20); return; }
  if (!strcmp(c, "dyn")) { cmdDyn(argc > 1 ? atoi(argv[1]) : 20); return; }
  if (!strcmp(c, "auto")) {
    autoTap = !(argc > 1 && !strcmp(argv[1], "off"));
    Serial.printf("  tap watch %s  (%lu seen so far)\n",
                  autoTap ? "on" : "off", (unsigned long)tapCount);
    return;
  }
  if (!strcmp(c, "ble")) {
    cmdBle(!(argc > 1 && !strcmp(argv[1], "off")));
    return;
  }

  if (!strcmp(c, "nfc")) {
    if (argc > 1 && !strcmp(argv[1], "write")) cmdNfcWrite(argc > 2 ? argv[2] : nullptr);
    else if (argc > 1 && !strcmp(argv[1], "sys")) cmdNfcSys();
    // example.com is reserved for exactly this by RFC 2606. A real
    // domain invented here would be written to real hardware and
    // then believed by someone later.
    else if (argc > 1 && !strcmp(argv[1], "json"))
      cmdNfcJson(argc > 2 ? argv[2] : "https://example.com/mcold",
                 argc > 3 ? argv[3] : nullptr);
    else if (argc > 1 && !strcmp(argv[1], "url"))
      cmdNfcUrl(argc > 2 ? argv[2] : "https://example.com/mcold",
                argc > 3 ? argv[3] : nullptr);
    else cmdNfcRead(argc > 1 ? atoi(argv[1]) : 48);
    return;
  }

  if (!strcmp(c, "probe")) {
    if (argc < 2) { Serial.println("  probe <hex addr> [count]"); return; }
    cmdProbe((uint8_t)strtol(argv[1], nullptr, 16),
             argc > 2 ? atoi(argv[2]) : 32);
    return;
  }

  if (!strcmp(c, "rgb")) { cmdRgb(argc > 1 ? atoi(argv[1]) : 1200); return; }

  if (!strcmp(c, "hold")) {
    if (argc < 4) { Serial.println("  hold <r> <g> <b> [brightness]"); return; }
    cmdHold(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]),
            argc > 4 ? atoi(argv[4]) : 120);
    return;
  }

  if (!strcmp(c, "led")) {
    if (argc == 5) {
      cmdLedOne(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    } else if (argc == 4) {
      cmdLed(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]));
    } else {
      Serial.println("  led <r> <g> <b>   or   led <index> <r> <g> <b>");
    }
    return;
  }
  if (!strcmp(c, "chg")) {
    if (argc < 2) { Serial.println("  chg on|off"); return; }
    cmdChg(!strcmp(argv[1], "on"));
    return;
  }
  if (!strcmp(c, "rail")) {
    if (argc < 3) { Serial.println("  rail epd|sd|led on|off"); return; }
    bool on = !strcmp(argv[2], "on");
    if (!strcmp(argv[1], "epd")) setRailEpd(on);
    else if (!strcmp(argv[1], "sd")) setRailSd(on);
    else if (!strcmp(argv[1], "led")) setRailLed(on);
    else { Serial.println("  rail epd|sd|led on|off"); return; }
    Serial.printf("  rail %s %s\n", argv[1], on ? "on" : "off");
    return;
  }

  Serial.printf("  unknown: %s   -- type ? for the list\n", c);
}

// ---- setup / loop ---------------------------------------------------

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);

  // Rails and the charger first, before anything can draw through them.
  pinMode(PIN_EPD_PWR_EN, OUTPUT); digitalWrite(PIN_EPD_PWR_EN, LOW);
  pinMode(PIN_SD_PWR_EN, OUTPUT);  digitalWrite(PIN_SD_PWR_EN, LOW);
  pinMode(PIN_LED_PWR_EN, OUTPUT); digitalWrite(PIN_LED_PWR_EN, HIGH);  // off
  pinMode(PIN_BUZZER, OUTPUT);     digitalWrite(PIN_BUZZER, LOW);

  // Charging starts disabled. The charger and the current monitor have
  // not been checked yet on this board, and USB keeps the system powered
  // through the BQ25601 power path either way, so nothing is lost by
  // waiting until `chg on` is given deliberately.
  pinMode(PIN_CHG_CE_N, OUTPUT);   digitalWrite(PIN_CHG_CE_N, HIGH);

  for (const PinRow &r : INPUTS) pinMode(r.pin, INPUT);

  // Chip selects idle high before the bus exists: GxEPD2-style drivers
  // and the Arduino SPI driver both write the pin before configuring it,
  // and a CS that floats low during init talks to a part by accident.
  pinMode(PIN_EPD_CS, OUTPUT); digitalWrite(PIN_EPD_CS, HIGH);
  pinMode(PIN_TC_CS, OUTPUT);  digitalWrite(PIN_TC_CS, HIGH);
  pinMode(PIN_EPD_DC, OUTPUT); digitalWrite(PIN_EPD_DC, HIGH);

  Wire.begin(PIN_SDA, PIN_SCL, 100000);   // 100 kHz until the bus is trusted
  spi3.begin(PIN_SPI3_SCK, PIN_SPI3_MISO, PIN_SPI3_MOSI, -1);

  Serial.println("\n\nmCOLD Foam V.1 -- bring-up");
  Serial.println("every rail is off, charging is disabled, nothing is driven");

  // Armed here rather than from a command. Opening the serial port
  // resets the board and so does closing it, so anything a command
  // switches on is already gone by the time anyone can act on it --
  // which is why three motion tests in a row found nothing on a sensor
  // that works perfectly well.
  if (stAccBegin(WAKE_THRESHOLD)) {
    wakeWatch = true;
    Serial.printf("motion watch armed at threshold %d, about %d mg:"
                  " tap at any time\nand the first pixel turns red\n",
                  WAKE_THRESHOLD, WAKE_THRESHOLD * 31);
  } else {
    Serial.println("motion watch could NOT be armed");
  }

  help();
  cmdInfo();
}

void loop() {
  static char line_[128];
  static int n = 0;
  static char prev = 0;
  static uint32_t lastTapPoll = 0;
  static uint32_t tapQuietUntil = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' && prev == '\r') { prev = c; continue; }   // CRLF is one
    prev = c;

    if (c == '\r' || c == '\n') {
      Serial.println();
      line_[n] = 0;
      n = 0;
      handle(line_);
    } else if (c == 8 || c == 127) {
      if (n > 0) { n--; Serial.print("\b \b"); }
    } else if (c >= ' ' && n < (int)sizeof(line_) - 1) {
      line_[n++] = c;
      Serial.write(c);
    }
  }

  // RF and I2C take turns at this tag, and whoever asks second is
  // turned away. Polling every 150 ms while a phone is mid-read is a
  // good way to make the phone's read fail and then blame the
  // antenna, so once a field is seen the poller gets out of the way.
  if (wakeWatch) {
    LIS2DW12_Event_Status_t ws;
    const bool pin = digitalRead(PIN_ACC_INT1);
    if (stAcc.Get_Event_Status(&ws) == LIS2DW12_STATUS_OK
        && (ws.WakeUpStatus || pin)) {
      wakeCount++;
      wakeLastMs = millis();
      Serial.printf("[%8lu ms] MOTION %lu  pin %s" "\n",
                    (unsigned long)wakeLastMs, (unsigned long)wakeCount,
                    pin ? "HIGH" : "low");
      Serial.flush();
      if (!ledHold) {
        ledsReady();
        leds.setPixelColor(0, leds.Color(80, 0, 0));
        leds.show();
      }
    }
  }

  if (autoTap && (int32_t)(millis() - tapQuietUntil) > 0
      && millis() - lastTapPoll > 150) {
    lastTapPoll = millis();
    uint8_t its = 0, eh = 0;
    bool okI = st25Read(ADDR_ST25_USER, ST25_IT_STS_DYN, &its, 1);
    bool okE = st25Read(ADDR_ST25_USER, ST25_EH_CTRL_DYN, &eh, 1);
    bool edge = okI && (its & ST25_FIELD_EDGE);
    bool now = okE && (eh & ST25_FIELD_ON);
    bool refused = !okI && !okE;

    if (edge || now || refused) {
      char sn[24];
      defaultSn(sn, sizeof(sn));
      tapCount++;
      Serial.printf("\n[%8lu ms] TAP %lu  IT_STS=0x%02X EH_CTRL=0x%02X%s\n",
                    (unsigned long)millis(), (unsigned long)tapCount, its, eh,
                    refused ? "  (I2C refused: the RF side holds the tag)" : "");
      if (!bleUp) {
        bleStart(sn);
        Serial.printf("           BLE advertising as \"%s\"\n", sn);
      }
      if (!ledHold) {
        ledsReady();
        leds.setPixelColor(0, leds.Color(0, 70, 0));
        leds.show();
      }
      tapQuietUntil = millis() + 4000;
      Serial.println("           pausing the poll 4 s so the phone has"
                     " the tag to itself");
      Serial.flush();
    }
  }

  delay(20);
}
