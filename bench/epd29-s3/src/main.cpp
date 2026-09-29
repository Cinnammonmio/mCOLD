// mCOLD display bench test -- ESP32-S3 devkit + 2.9" e-paper module.
//
// Draws the mCOLD screen layout on a 296x128 panel so the design can be
// judged on real e-paper ink, and times each refresh, since the refresh
// interval in docs/display-design.md is still an estimate.
//
// Wiring (from the board notes):
//   GPIO39 -> SCL/SCK      GPIO41 -> DC
//   GPIO40 -> SDA/MOSI     GPIO42 -> CS
//   GPIO38 -> BUSY         EN     -> RES
//
// RES is tied to the board's EN pin, so the panel resets only when the MCU
// does. The driver is told there is no reset pin (-1) and falls back to the
// software reset in the init sequence. If the panel ever hangs, press the
// board's reset button: that pulls EN low and hard-resets both.

#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#define PIN_SCK   39
#define PIN_MOSI  40
#define PIN_DC    41
#define PIN_CS    42
#define PIN_BUSY  38
#define PIN_RST   -1    // tied to EN, not driveable from firmware

// ---------------------------------------------------------------------
// Pick the panel class that matches the module. Start with the first one;
// if the screen stays blank or garbled, try the next. Wrong class is the
// usual cause of "nothing happens" with correct wiring.
//
//   GxEPD2_290_T94_V2  GDEM029T94 / Waveshare 2.9" V2, SSD1680   <- try first
//   GxEPD2_290_T94     GDEM029T94 V1
//   GxEPD2_290         GDEH029A1, SSD1608 (older modules)
//   GxEPD2_290_BS      DEPG0290BS
// Three-colour modules use GxEPD2_3C instead -- see USE_3C below.
// ---------------------------------------------------------------------
#define USE_3C 0        // set to 1 for a black/white/red module

#if USE_3C
  // Red-capable: GxEPD2_290_C90c (GDEM029C90, ~27 s full refresh)
  //           or GxEPD2_290_Z13c (GDEH029Z13, ~18 s)
  GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT> display(
      GxEPD2_290_C90c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define INK_ACCENT GxEPD_RED
#else
  GxEPD2_BW<GxEPD2_290_T94_V2, GxEPD2_290_T94_V2::HEIGHT> display(
      GxEPD2_290_T94_V2(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define INK_ACCENT GxEPD_BLACK
#endif

static const int BAR = 24;      // header bar height
static const int M = 8;         // side margin

// Draw `text` centred horizontally, returning nothing; y is the baseline.
static void centred(const char *text, int16_t y, const GFXfont *f,
                    uint16_t colour = GxEPD_BLACK) {
  int16_t bx, by;
  uint16_t bw, bh;
  display.setFont(f);
  display.setTextColor(colour);
  display.getTextBounds(text, 0, y, &bx, &by, &bw, &bh);
  display.setCursor((display.width() - bw) / 2 - bx, y);
  display.print(text);
}

static void rightAt(const char *text, int16_t x, int16_t y, const GFXfont *f,
                    uint16_t colour = GxEPD_BLACK) {
  int16_t bx, by;
  uint16_t bw, bh;
  display.setFont(f);
  display.setTextColor(colour);
  display.getTextBounds(text, 0, y, &bx, &by, &bw, &bh);
  display.setCursor(x - bw - bx, y);
  display.print(text);
}

// The monitor screen: inverted header, temperature centred and largest,
// min/max last, a divider and a status row.
static void drawMonitor(const char *temp, const char *lo, const char *hi,
                        bool alarm) {
  const int W = display.width(), H = display.height();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.fillRect(0, 0, W, BAR, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(M, 17);
    display.print("MCOLD-0117");
    rightAt("14:32", W - M, 17, &FreeSans9pt7b, GxEPD_WHITE);

    centred(temp, 82, &FreeSansBold24pt7b, alarm ? INK_ACCENT : GxEPD_BLACK);

    char row[40];
    snprintf(row, sizeof(row), "MIN %s   MAX %s", lo, hi);
    centred(row, 106, &FreeSans9pt7b);

    display.fillRect(0, 111, W, 1, GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(M, 126);
    display.print("TRIP 0142");
    rightAt("BAT 78%  MEM 6%", W - M, 126, &FreeSansBold9pt7b);

    if (alarm) {
      for (int i = 0; i < 3; i++) {
        display.drawRect(i, i, W - 2 * i, H - 2 * i, INK_ACCENT);
      }
    }
  } while (display.nextPage());
}

// Solid bars: confirms the ink can reach full black (and full red on a 3C
// panel) and shows up any banding from the boost circuit.
static void drawInkTest() {
  const int W = display.width(), H = display.height();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, W / 3, H, GxEPD_BLACK);
    display.fillRect(W / 3, 0, W / 3, H, INK_ACCENT);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(2 * W / 3 + 8, H / 2);
    display.print("INK TEST");
    for (int y = H / 2 + 10; y < H - 6; y += 4) {
      display.drawFastHLine(2 * W / 3 + 8, y, W / 3 - 16, GxEPD_BLACK);
    }
  } while (display.nextPage());
}

// Time one refresh so the estimates in docs/display-design.md can be
// replaced with a measured number.
static void timed(const char *label, void (*draw)()) {
  uint32_t t0 = millis();
  draw();
  Serial.printf("%-12s refresh %lu ms\n", label, (unsigned long)(millis() - t0));
}

static void pageNormal() { drawMonitor("4.2", "2.8", "6.1", false); }
static void pageAlarm()  { drawMonitor("11.6", "2.8", "11.6", true); }
static void pageInk()    { drawInkTest(); }

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);   // native USB enumerate

  Serial.println("\nmCOLD e-paper bench test");

  // Read BUSY before touching SPI: a floating or stuck line is the most
  // common wiring fault, and it looks identical to a dead panel later on.
  pinMode(PIN_BUSY, INPUT);
  Serial.printf("BUSY (GPIO%d) reads %s at boot\n", PIN_BUSY,
                digitalRead(PIN_BUSY) ? "HIGH" : "LOW");

  // Route the default SPI instance onto this board's pins. No MISO: the
  // panel is write-only.
  SPI.end();
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);

  display.init(115200, true, 2, false);   // serial diag, initial full refresh
  display.setRotation(1);                 // landscape, 296x128
  display.setTextWrap(false);
  Serial.printf("panel %dx%d, hasFastPartialUpdate=%d\n",
                display.width(), display.height(),
                display.epd2.hasFastPartialUpdate);

  timed("ink", pageInk);
  delay(3000);
}

void loop() {
  timed("normal", pageNormal);
  delay(8000);
  timed("alarm", pageAlarm);
  delay(8000);
}
