// mCOLD display bench test -- ESP32-S3 devkit + 2.13" e-paper module.
//
// The 2.13" panel is 250x122, the same geometry as the real mCOLD display,
// so this reproduces the layout from docs/display-design.md at true size,
// in IBM Plex at the design's own sizes (see tools/genfont.py). Neither
// board nor panel is mCOLD hardware.
//
// Wiring:
//   GPIO39 -> SCL/SCK      GPIO41 -> DC
//   GPIO40 -> SDA/MOSI     GPIO42 -> CS
//   GPIO38 -> BUSY         EN     -> RES
//
// RES is tied to the board's EN pin, so firmware cannot reset the panel;
// the driver is passed -1 and GxEPD2 skips the reset pulse. A board reset
// resets both. Power the board from a normal USB port: the panel's boost
// converter draws a current pulse at refresh, and a current-limited supply
// such as a USB isolator browns the chip out mid-refresh.
//
// Measured on this panel: the controller holds 128 rows but only the top
// 122 reach the glass, which is the padding question the requirement doc
// says not to guess at.

#include <Arduino.h>
#include <SPI.h>
#include <esp_system.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_4C.h>
#include "fonts_mcold.h"

#define PIN_SCK   39
#define PIN_MOSI  40
#define PIN_DC    41
#define PIN_CS    42
#define PIN_BUSY  38
#define PIN_RST   -1    // tied to EN, not driveable from firmware

// ---------------------------------------------------------------------
// Which 2.13" module is connected.
//   4 -> GDEY0213F51      black/white/yellow/red, 25 s, no fast partial
//                         (this is the one that matches mCOLD)
//   3 -> GxEPD2_213_Z98c  black/white/red, 15 s
//   1 -> GxEPD2_213_B74   black/white, 3.6 s, fast partial 0.5 s
//        other mono variants: GxEPD2_213_BN, _B72, _B73, _T5D, _flex
// ---------------------------------------------------------------------
#define PANEL_INKS 1

#if PANEL_INKS == 4
  GxEPD2_4C<GxEPD2_213c_GDEY0213F51, GxEPD2_213c_GDEY0213F51::HEIGHT> display(
      GxEPD2_213c_GDEY0213F51(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define INK_ACCENT GxEPD_RED
#elif PANEL_INKS == 3
  GxEPD2_3C<GxEPD2_213_Z98c, GxEPD2_213_Z98c::HEIGHT> display(
      GxEPD2_213_Z98c(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define INK_ACCENT GxEPD_RED
#else
  GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
      GxEPD2_213_B74(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
  #define INK_ACCENT GxEPD_BLACK
#endif

// Layout grid, straight from docs/display-design.md. Every y is a baseline
// except the two bands, so no glyph crosses a rule.
static const int VIS_H  = 122;   // rows that actually reach the glass
static const int BAR    = 17;    // header bar height
static const int M      = 8;     // side margin
static const int B_BAR  = 12;    // header baseline
static const int B_HERO = 80;    // temperature baseline
// Cap height of mColdHero80: the numeral occupies B_HERO-HERO_CAP
// down to B_HERO, so the unit can be hung off the top of the digits
// instead of a second magic number that goes stale when the size does.
static const int HERO_CAP = 57;
static const int B_MM   = 96;    // min/max baseline
static const int RULE     = 100;
static const int B_FOOT   = 118;  // footer text baseline
static const int ICON_BOT = 120;  // footer icons sit on this line
static const int TRACK  = 1;     // caps letterspacing, whole pixels

// ---- text helpers ---------------------------------------------------

static uint16_t advance(const char *s, const GFXfont *f) {
  int16_t bx, by; uint16_t bw, bh;
  display.setFont(f);
  display.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  return bw;
}

// Caps are letterspaced like the mockup, so they are drawn per character.
static uint16_t trackedWidth(const char *s, const GFXfont *f) {
  uint16_t w = 0;
  int n = 0;
  char one[2] = {0, 0};
  for (const char *p = s; *p; p++, n++) {
    one[0] = *p;
    w += advance(one, f);
  }
  return w + (n > 1 ? (n - 1) * TRACK : 0);
}

static void drawTracked(int x, int y, const char *s, const GFXfont *f,
                        uint16_t colour) {
  display.setFont(f);
  display.setTextColor(colour);
  char one[2] = {0, 0};
  for (const char *p = s; *p; p++) {
    one[0] = *p;
    display.setCursor(x, y);
    display.print(one);
    x += advance(one, f) + TRACK;
  }
}

static void drawAt(int x, int y, const char *s, const GFXfont *f,
                   uint16_t colour) {
  display.setFont(f);
  display.setTextColor(colour);
  display.setCursor(x, y);
  display.print(s);
}

// ---- pieces ---------------------------------------------------------

// The degree sign is drawn, not set: the subset fonts carry only the
// characters the layout needs, and a ring is one call.
static uint16_t unitWidth() { return 13 + advance("C", &mColdUnit20); }

static void drawUnit(int x, int top, uint16_t colour) {
  // A disc with a smaller disc punched out of it. Two concentric
  // drawCircle calls looked like the same ring but left eight white
  // specks where the two Bresenham circles fail to meet, one at each
  // octant boundary; filling and punching cannot leave a gap.
  display.fillCircle(x + 5, top + 8, 5, colour);
  display.fillCircle(x + 5, top + 8, 3, GxEPD_WHITE);
  drawAt(x + 13, top + 24, "C", &mColdUnit20, colour);
}

static void drawHero(const char *temp, uint16_t colour) {
  uint16_t wNum = advance(temp, &mColdHero80);
  int x = (display.width() - (wNum + 5 + unitWidth())) / 2;
  drawAt(x, B_HERO, temp, &mColdHero80, colour);
  drawUnit(x + wNum + 5, B_HERO - HERO_CAP, colour);
}

// A label and the value it names belong together; the two pairs do not.
// Even gaps made the four items read as one run, so the gap inside a pair
// is tight, the gap between pairs is wide, and a dot sits in the middle
// of the wide one. A vertical rule was tried first and read as a digit
// with figures on both sides of it; a dot cannot be mistaken for one.
static void drawMinMax(const char *lo, const char *hi) {
  const int pair = 5;     // label to its own value
  const int split = 26;   // one reading to the next
  uint16_t wMin = trackedWidth("MIN", &mColdCaps11);
  uint16_t wMax = trackedWidth("MAX", &mColdCaps11);
  uint16_t wLo = advance(lo, &mColdRead15);
  uint16_t wHi = advance(hi, &mColdRead15);
  int x = (display.width()
           - (wMin + pair + wLo + split + wMax + pair + wHi)) / 2;

  drawTracked(x, B_MM, "MIN", &mColdCaps11, GxEPD_BLACK);
  x += wMin + pair;
  drawAt(x, B_MM, lo, &mColdRead15, GxEPD_BLACK);
  x += wLo;
  display.fillRect(x + split / 2, B_MM - 6, 2, 2, GxEPD_BLACK);
  x += split;
  drawTracked(x, B_MM, "MAX", &mColdCaps11, GxEPD_BLACK);
  x += wMax + pair;
  drawAt(x, B_MM, hi, &mColdRead15, GxEPD_BLACK);
}

// 1-bit icons, plotted on the pixel grid rather than scaled from vector
// art, the same shapes as display-mock/render.py.
struct Icon { uint8_t w, h; const char *rows[24]; };

// Three solid arcs over a dot, thick enough to hold up beside the filled
// cloud; a hairline arc reads as a scratch on a panel with no greys.
static const Icon ICON_WIFI = {20, 16, {"........####........",
                                        "....############....",
                                        "..################..",
                                        ".######......######.",
                                        "####............####",
                                        "###..##########..###",
                                        "....############....",
                                        "...#####....#####...",
                                        "...###........###...",
                                        ".......######.......",
                                        "......########......",
                                        "......########......",
                                        "....................",
                                        ".........##.........",
                                        "........####........",
                                        "........####........"}};

// Filled, not outlined. The reference's white highlight is left out: one
// pixel of white inside the blob reads as a defect at this size.
static const Icon ICON_CLOUD = {20, 16, {"....................",
                                         "....................",
                                         ".......######.......",
                                         "......########......",
                                         ".....##########.....",
                                         ".....##########.....",
                                         "....##############..",
                                         "..#################.",
                                         ".###################",
                                         "####################",
                                         "####################",
                                         "####################",
                                         "####################",
                                         ".##################.",
                                         "..################..",
                                         "....############...."}};

// A parcel with an impact star off its top-right corner. The two shapes
// are kept apart rather than overlapped: at this size an overlap turns
// both into one blob.
static const Icon ICON_SHOCK = {20, 16, {"..............#.....",
                                         "..............#.....",
                                         "..........##.##..#..",
                                         "...........######...",
                                         "............####....",
                                         "............#####...",
                                         "###########...#.....",
                                         "#...................",
                                         "##############......",
                                         "#....###.....#......",
                                         "#....###.....#......",
                                         "#............#......",
                                         "#............#......",
                                         "#............#......",
                                         "#............#......",
                                         "##############......"}};

// Shapes generated by tools/genicons.py -- every one 16 rows tall, so
// the footer reads as one set. Edit the parameters there, not these
// rows. Only the storage pictogram and the battery are plotted by
// hand here, and both were already on that height.
// A map pin, the shape the design asked for: a round head with a hole
// through it over a tail that tapers to a point. Generated by
// tools/genicon_gnss.py -- edit the parameters there, not these rows.
static const Icon ICON_GNSS = {12, 16, {"....####....",
                                        "..########..",
                                        ".##########.",
                                        ".####..####.",
                                        "####....####",
                                        "###......###",
                                        "###......###",
                                        "####....####",
                                        ".####..#####",
                                        ".##########.",
                                        "..########..",
                                        "..########..",
                                        "...######...",
                                        "....####....",
                                        ".....###....",
                                        ".....##....."}};

// Two stacked drives, as asked: a lid over the upper unit and a pair of
// indicator dashes on each. It is a pictogram, not a gauge, so the
// percentage beside it carries the value.
static const Icon ICON_STORAGE = {15, 16, {"....#######....",
                                           "..##.......##..",
                                           ".#...........#.",
                                           "###############",
                                           "#.............#",
                                           "#.##...####...#",
                                           "#.##...####...#",
                                           "#.............#",
                                           "###############",
                                           ".#...........#.",
                                           "###############",
                                           "#.............#",
                                           "#.##...####...#",
                                           "#.##...####...#",
                                           "#.............#",
                                           "###############"}};

// Generated by tools/genicon_charge.py.
static const Icon ICON_CHARGE = {9, 16, {".....#...",
                                         "....##...",
                                         "....##...",
                                         "...###...",
                                         "..####...",
                                         "..####...",
                                         ".#####...",
                                         ".########",
                                         "########.",
                                         "....####.",
                                         "....###..",
                                         "....##...",
                                         "...###...",
                                         "...##....",
                                         "...#.....",
                                         "...#....."}};

// A play triangle for "trip running". Solid, symmetric, and narrow: it
// shares the footer with five other marks and the widest of them is the
// shock burst at 24 px.
static const Icon ICON_TRIP = {9, 16, {"#........",
                                       "##.......",
                                       "###......",
                                       "####.....",
                                       "#####....",
                                       "######...",
                                       "#######..",
                                       "########.",
                                       "########.",
                                       "#######..",
                                       "######...",
                                       "#####....",
                                       "####.....",
                                       "###......",
                                       "##.......",
                                       "#........"}};

// `off` strikes the icon through instead of hiding it: a persisted frame
// must not leave "not connected" and "not refreshed" looking the same.
static int drawIcon(const Icon &ic, int x, int bottom, uint16_t colour,
                    bool off = false) {
  int top = bottom - ic.h;
  // Stop at a missing row or a short one rather than trusting w and h.
  // A header that claims more rows than the initialiser lists leaves null
  // pointers in the struct's tail, and reading one panics the chip: that
  // has now cost two debugging sessions, so the loop checks instead.
  for (int r = 0; r < ic.h && ic.rows[r] != nullptr; r++) {
    const char *row = ic.rows[r];
    for (int c = 0; c < ic.w && row[c] != '\0'; c++) {
      if (row[c] == '#') display.drawPixel(x + c, top + r, colour);
    }
  }
  if (off) display.drawLine(x - 1, top + ic.h, x + ic.w, top - 1, colour);
  return ic.w;
}

// A cell standing on end, the same height and the same bottom line as the
// storage pictogram beside it, split into four blocks. Four blocks are
// counted at a glance where the length of a bar has to be judged against
// nothing, and on a panel with no greys a block is either there or it is
// not -- no half-lit state to misread.
static const int BATT_W = 11;
// Two rows taller than the storage pictogram it stands beside, and
// bottom-aligned with it. At 14 the four blocks only fit by running into
// the shell top and bottom, which made the end blocks look thicker than
// the middle two and turned a full cell into a striped brick; 16 buys the
// one row of white at each end that keeps all four the same block.
static const int BATT_H = 16;

static void batteryGauge(int x, int bottom, int pct, uint16_t colour) {
  int top = bottom - BATT_H;
  display.fillRect(x + 3, top, 5, 1, colour);                 // terminal
  display.drawRect(x, top + 1, BATT_W, BATT_H - 1, colour);   // shell

  // Nearest quarter, but never draw an empty cell while the thing is
  // still running: 1% and 0% must not look the same.
  int lit = (pct * 4 + 50) / 100;
  if (lit < 1 && pct > 0) lit = 1;
  if (lit > 4) lit = 4;

  // Bottom block first, three rows apart: two rows of block, one of gap.
  for (int i = 0; i < lit; i++) {
    display.fillRect(x + 2, top + 12 - i * 3, BATT_W - 4, 2, colour);
  }
}

// State of everything the footer can show, in one bundle: the templates
// pass it straight through, so adding an indicator does not mean widening
// four signatures.
struct Foot {
  bool trip, wifi, cloud, gnss, shock, charging;
  int batt, mem;
};

static const int BATT_LOW = 20;   // % at or below -> red
static const int MEM_HIGH = 90;   // % used at or above -> red
static const int GAP = 7;         // between footer icons

static void drawFooter(const Foot &f) {
  display.drawFastHLine(0, RULE, display.width(), GxEPD_BLACK);

  // Wi-Fi, cloud and GNSS are always drawn -- struck through rather than
  // hidden when they have nothing -- so the two that come and go, trip and
  // shock, sit after them and never shift what is already on the glass.
  int x = M;
  x += drawIcon(ICON_WIFI, x, ICON_BOT, GxEPD_BLACK, !f.wifi) + GAP;
  x += drawIcon(ICON_CLOUD, x, ICON_BOT, GxEPD_BLACK, !f.cloud) + GAP;
  x += drawIcon(ICON_GNSS, x, ICON_BOT, GxEPD_BLACK, !f.gnss) + GAP;
  if (f.trip) x += drawIcon(ICON_TRIP, x, ICON_BOT, GxEPD_BLACK) + GAP;
  if (f.shock) x += drawIcon(ICON_SHOCK, x, ICON_BOT, INK_ACCENT) + GAP;
  int leftEnd = x - GAP;

  char s[8];
  x = display.width() - M;

  uint16_t memInk = (f.mem >= MEM_HIGH) ? INK_ACCENT : GxEPD_BLACK;
  snprintf(s, sizeof(s), "%d%%", f.mem);
  x -= trackedWidth(s, &mColdCaps11);
  drawTracked(x, B_FOOT, s, &mColdCaps11, memInk);
  x -= 5 + ICON_STORAGE.w;
  drawIcon(ICON_STORAGE, x, ICON_BOT, memInk);
  x -= 11;

  uint16_t battInk = (f.batt <= BATT_LOW) ? INK_ACCENT : GxEPD_BLACK;
  snprintf(s, sizeof(s), "%d%%", f.batt);
  x -= trackedWidth(s, &mColdCaps11);
  drawTracked(x, B_FOOT, s, &mColdCaps11, battInk);
  x -= 5 + BATT_W;
  batteryGauge(x, ICON_BOT, f.batt, battInk);

  // The bolt goes outside the cell, not inside it: the four blocks are the
  // reading, and dropping a bolt on top of them would cover the one thing
  // the icon exists to say.
  if (f.charging) {
    x -= 5 + ICON_CHARGE.w;
    drawIcon(ICON_CHARGE, x, ICON_BOT, GxEPD_BLACK);
  }

  if (leftEnd > x) Serial.printf("  WARN footer overlaps by %d px\n", leftEnd - x);
}

static void drawHeader(const char *clock) {
  display.fillRect(0, 0, display.width(), BAR, GxEPD_BLACK);
  drawTracked(M, B_BAR, "MCOLD-0117", &mColdCaps11, GxEPD_WHITE);
  uint16_t w = trackedWidth(clock, &mColdCaps11);
  drawTracked(display.width() - M - w, B_BAR, clock, &mColdCaps11, GxEPD_WHITE);
}

static void alarmFrame() {
  for (int i = 0; i < 3; i++) {
    display.drawRect(i, i, display.width() - 2 * i, VIS_H - 2 * i, INK_ACCENT);
  }
}

// ---- rich strings ---------------------------------------------------

// The subset fonts carry ASCII only, but the design separates data items
// with a middle dot and ends a temperature with a degree ring, neither of
// which is ASCII. The strings mark them instead: '|' for the dot, '~' for
// the ring, and both are drawn rather than set. One routine both measures
// and draws, so a right-aligned value cannot be measured by different
// rules than it is painted with.
static const int DOT_W = 10;
static const int RING_W = 6;

static int richRender(int x, int y, const char *s, const GFXfont *f,
                      uint16_t colour, bool track, bool draw) {
  const int x0 = x;
  char buf[80];
  int n = 0;
  for (const char *p = s;; p++) {
    if (*p == '|' || *p == '~' || *p == 0) {
      buf[n] = 0;
      if (n) {
        if (draw) {
          if (track) drawTracked(x, y, buf, f, colour);
          else       drawAt(x, y, buf, f, colour);
        }
        x += track ? trackedWidth(buf, f) : advance(buf, f);
        n = 0;
      }
      if (*p == 0) break;
      if (*p == '|') {
        if (draw) display.fillRect(x + 4, y - 4, 2, 2, colour);
        x += DOT_W;
      } else {
        if (draw) {
          display.fillRect(x + 1, y - 10, 3, 3, colour);
          display.drawPixel(x + 2, y - 9, GxEPD_WHITE);
        }
        x += RING_W;
      }
    } else if (n < (int)sizeof(buf) - 1) {
      buf[n++] = *p;
    }
  }
  return x - x0;
}

static uint16_t richWidth(const char *s, const GFXfont *f, bool track) {
  return (uint16_t)richRender(0, 0, s, f, 0, track, false);
}

static void drawRich(int x, int y, const char *s, const GFXfont *f,
                     uint16_t colour, bool track) {
  richRender(x, y, s, f, colour, track, true);
}

// Text that runs past its column is the one layout fault a photograph of
// the panel hides, so every variable-width item reports itself.
static void fits(const char *what, int width, int room) {
  if (width > room) Serial.printf("  WARN %s overflows by %d px\n", what, width - room);
}

// ---- templates ------------------------------------------------------

struct Row { const char *label, *value; };

// A -- Monitor: temperature centred and largest, min/max last. Red means
// the number is not to be trusted; a red frame means it is an alarm. No
// words explain either -- the reason is in the LED, the log and the app.
static void monitorPage(const Foot &f, const char *temp, const char *lo,
                        const char *hi, const char *note, bool red, bool alarm) {
  drawHeader("14:32");
  drawHero(temp, (red || alarm) ? INK_ACCENT : GxEPD_BLACK);
  if (lo) {
    drawMinMax(lo, hi);
  } else if (note) {
    uint16_t w = trackedWidth(note, &mColdCaps11);
    drawTracked((display.width() - w) / 2, B_MM, note, &mColdCaps11, GxEPD_BLACK);
  }
  drawFooter(f);
  if (alarm) alarmFrame();
}

// A6 -- Charge: no temperature at all, the whole charging picture instead.
static void chargePage(const Foot &f, int soc, const char *state,
                       const Row *rows) {
  drawHeader("14:32");
  char s[8];
  snprintf(s, sizeof(s), "%d%%", soc);
  drawAt(M, 66, s, &mColdBig44, GxEPD_BLACK);
  drawTracked(M, 90, state, &mColdStat13, GxEPD_BLACK);

  const int col = 104, edge = display.width() - M;
  for (int i = 0; i < 3; i++) {
    const int y = 42 + i * 18;
    drawTracked(col, y, rows[i].label, &mColdCaps11, GxEPD_BLACK);
    uint16_t vw = advance(rows[i].value, &mColdRead15);
    fits(rows[i].value, vw, edge - col - trackedWidth(rows[i].label, &mColdCaps11) - 7);
    drawAt(edge - vw, y, rows[i].value, &mColdRead15, GxEPD_BLACK);
  }
  drawFooter(f);
}

// B -- Takeover: one headline, one sentence, one data line.
static void takeoverPage(const Foot &f, const char *clock, const char *title,
                         const char *sub, const char *data, bool alarm,
                         bool redTitle) {
  drawHeader(clock);
  const int room = display.width() - 2 * M;
  fits(title, advance(title, &mColdTitle26), room);
  drawAt(M, 54, title, &mColdTitle26, redTitle ? INK_ACCENT : GxEPD_BLACK);
  fits(sub, advance(sub, &mColdBody13), room);
  drawAt(M, 76, sub, &mColdBody13, GxEPD_BLACK);
  if (data) {
    fits(data, richWidth(data, &mColdCaps11, true), room);
    drawRich(M, 93, data, &mColdCaps11, GxEPD_BLACK, true);
  }
  drawFooter(f);
  if (alarm) alarmFrame();
}

// C -- Detail: label left, value right, three rows, no hero.
// The title takes the same two columns as the rows under it: what the
// page is about on the left, its state on the right. Set as one string
// the two ran together -- "TRIP 0142 CLOSED" reads as a four-word blur
// at caps 11 -- and the eye had nothing to tell the number from the word.
static void detailPage(const Foot &f, const char *title, const char *state,
                       const Row *rows) {
  drawHeader("14:32");
  drawTracked(M, 36, title, &mColdCaps11, GxEPD_BLACK);
  if (state) {
    uint16_t w = trackedWidth(state, &mColdCaps11);
    drawTracked(display.width() - M - w, 36, state, &mColdCaps11, GxEPD_BLACK);
  }
  for (int i = 0; i < 3; i++) {
    const int y = 56 + i * 18;
    drawAt(M, y, rows[i].label, &mColdBody13, GxEPD_BLACK);
    uint16_t vw = richWidth(rows[i].value, &mColdBody13, false);
    fits(rows[i].value, vw, display.width() - 2 * M
                            - advance(rows[i].label, &mColdBody13) - 8);
    drawRich(display.width() - M - vw, y, rows[i].value, &mColdBody13,
             GxEPD_BLACK, false);
  }
  drawFooter(f);
}

// ---- the fourteen states --------------------------------------------

struct PageInfo { const char *id, *name; };

static const PageInfo PAGES[] = {
  {"A1", "IDLE / READY"},     {"A2", "TRIP ACTIVE"},
  {"A3", "OUT OF BAND"},      {"A4", "ALARM"},
  {"A5", "NO READING"},       {"A6", "CHARGING"},
  {"B1", "BOOT / SELF-TEST"}, {"B2", "USB CONNECTED"},
  {"B3", "BLE SESSION"},      {"B4", "STORAGE FULL"},
  {"B5", "CRITICAL BATTERY"}, {"B6", "FAULT / RECOVERY"},
  {"C1", "TRIP SUMMARY"},     {"C2", "DEVICE DETAIL"},
};
static const int N_PAGES = sizeof(PAGES) / sizeof(PAGES[0]);

// Foot fields in declaration order:
//   trip, wifi, cloud, gnss, shock, charging, batt, mem
static void paintPage(int i) {
  display.fillScreen(GxEPD_WHITE);
  switch (i) {
    case 0: {   // A1  idle: no trip yet, so there is no min/max to show
      Foot f = {false, true, true, true, false, false, 78, 6};
      monitorPage(f, "4.2", nullptr, nullptr, "NO ACTIVE TRIP", false, false);
      break;
    }
    case 1: {   // A2  trip running normally -- all black, silence is normal
      Foot f = {true, true, true, true, false, false, 78, 6};
      monitorPage(f, "4.2", "2.8", "6.1", nullptr, false, false);
      break;
    }
    case 2: {   // A3  out of band: red number, no frame
      Foot f = {true, true, true, true, false, false, 78, 6};
      monitorPage(f, "8.4", "2.8", "8.4", nullptr, true, false);
      break;
    }
    case 3: {   // A4  alarm: red number and red frame
      Foot f = {true, true, false, true, true, false, 61, 7};
      monitorPage(f, "11.6", "2.8", "11.6", nullptr, false, true);
      break;
    }
    case 4: {   // A5  no usable sample -- "--", never 0.0
      Foot f = {true, false, false, false, false, false, 16, 6};
      monitorPage(f, "--", "2.8", "6.1", nullptr, true, false);
      break;
    }
    case 5: {   // A6  charging
      Foot f = {false, true, true, true, false, true, 62, 6};
      static const Row rows[] = {{"SOURCE", "DOCK PD 20V"},
                                 {"CURRENT", "1.18 A"},
                                 {"FULL IN", "42 MIN"}};
      chargePage(f, 62, "FAST CHARGE", rows);
      break;
    }
    case 6: {   // B1  boot: clock not set yet, radios not up
      Foot f = {false, false, false, false, false, false, 78, 6};
      takeoverPage(f, "--:--", "SELF-TEST", "Checking sensors and storage",
                   "FIRMWARE 0.1.0|SERIAL 0117", false, false);
      break;
    }
    case 7: {   // B2  USB mass storage, read-only
      Foot f = {true, true, true, true, false, false, 78, 6};
      takeoverPage(f, "14:32", "USB DRIVE", "Read-only. Copy the CSV, then eject.",
                   "SNAPSHOT 14:31|2,184 RECORDS", false, false);
      break;
    }
    case 8: {   // B3  BLE pairing window
      Foot f = {false, true, true, true, false, false, 78, 6};
      takeoverPage(f, "14:32", "MCOLD-0117", "Confirm this ID in the app to pair.",
                   "PAIRING WINDOW 60 S", false, false);
      break;
    }
    case 9: {   // B4  storage full -- data was actually lost, so: frame
      Foot f = {true, true, false, true, false, false, 54, 100};
      takeoverPage(f, "14:32", "STORAGE FULL", "Oldest finished trip was dropped.",
                   "TRIP 0139 LOST|UPLOAD NOW", true, true);
      break;
    }
    case 10: {  // B5  critical battery -- logging has stopped
      Foot f = {false, false, false, false, false, false, 4, 6};
      takeoverPage(f, "14:32", "BATTERY 4%", "Logging stopped. Charge the device.",
                   "LAST RECORD 14:32", true, true);
      break;
    }
    case 11: {  // B6  fault, but the trip goes on -- red title, no frame
      Foot f = {true, true, true, true, false, false, 78, 6};
      takeoverPage(f, "14:32", "SENSOR FAULT", "Temperature bus not responding.",
                   "TRIP CONTINUES|SEE APP", false, true);
      break;
    }
    case 12: {  // C1  trip summary, drawn automatically after STOP_TRIP
      Foot f = {false, true, true, true, false, false, 74, 9};
      static const Row rows[] = {{"Duration", "3d 06h 12m"},
                                 {"Temperature", "2.8 / 6.1 ~C"},
                                 {"Alarms", "1 high|2 shock"}};
      detailPage(f, "TRIP 0142", "CLOSED", rows);
      break;
    }
    default: {  // C2  device detail, from an NFC tap or the app
      Foot f = {true, true, true, true, false, false, 78, 6};
      static const Row rows[] = {{"Trip", "0142|21 Sep 08:20"},
                                 {"Upload", "184 records pending"},
                                 {"Firmware", "0.1.0|RTC OK"}};
      detailPage(f, "DEVICE", "DETAIL", rows);
      break;
    }
  }
}

// ---- driver ---------------------------------------------------------

static int g_page = 1;   // A2 on power-up: the state the box spends its life in

static void showPage(int i) {
  g_page = i;
  uint32_t t0 = millis();
  display.setFullWindow();
  display.firstPage();
  do {
    paintPage(i);
  } while (display.nextPage());
  Serial.printf("%s  %-18s %lu ms\n", PAGES[i].id, PAGES[i].name,
                (unsigned long)(millis() - t0));
  Serial.flush();
}

static void listPages() {
  Serial.println();
  for (int i = 0; i < N_PAGES; i++) {
    Serial.printf("  %s  %-18s%s\n", PAGES[i].id, PAGES[i].name,
                  i == g_page ? "  <- on screen" : "");
  }
  Serial.println("  ENTER or N next   P prev   R redraw   ? this list");
  Serial.println();
  Serial.flush();
}

static void handleCommand(char *s) {
  for (char *p = s; *p; p++) *p = toupper((unsigned char)*p);
  // A bare Enter steps forward. Typing into a serial terminal is awkward
  // enough that walking fourteen pages should not cost two keys a time.
  if (!*s) { showPage((g_page + 1) % N_PAGES); return; }
  if (!strcmp(s, "?") || !strcmp(s, "L")) { listPages(); return; }
  if (!strcmp(s, "N")) { showPage((g_page + 1) % N_PAGES); return; }
  if (!strcmp(s, "P")) { showPage((g_page + N_PAGES - 1) % N_PAGES); return; }
  if (!strcmp(s, "R")) { showPage(g_page); return; }
  for (int i = 0; i < N_PAGES; i++) {
    if (!strcmp(s, PAGES[i].id)) { showPage(i); return; }
  }
  Serial.printf("unknown: %s   -- type ? for the list\n", s);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);

  Serial.println("\nmCOLD e-paper bench test 2.13 inch");
  Serial.printf("reset reason: %d  (1=power on, 3=sw, 4=panic, 5=int wdt, 6=task wdt, 9=brownout)\n",
                (int)esp_reset_reason());

  pinMode(PIN_BUSY, INPUT);
  Serial.printf("BUSY (GPIO%d) reads %s at boot\n", PIN_BUSY,
                digitalRead(PIN_BUSY) ? "HIGH" : "LOW");

  // Claim CS and DC as plain GPIO first: GxEPD2 calls digitalWrite() before
  // pinMode(), which the peripheral manager logs as an error. SS is left
  // unclaimed (-1) so the SPI driver does not fight for CS.
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);
  digitalWrite(PIN_DC, HIGH);
  SPI.end();
  SPI.begin(PIN_SCK, -1, PIN_MOSI, -1);

  display.init(115200, true, 2, false);
  display.setRotation(3);          // landscape, rotated 180 degrees
  display.setTextWrap(false);
  Serial.printf("panel %dx%d (visible %dx%d), fastPartial=%d, inks=%d\n",
                display.width(), display.height(), display.width(), VIS_H,
                display.epd2.hasFastPartialUpdate, PANEL_INKS);

  Serial.flush();
  delay(500);

  listPages();
  showPage(g_page);
}

// One command per line over USB serial. Reading the port matters for a
// second reason: with nothing draining the CDC receive FIFO the host
// side blocks, which is what made esptool time out on every upload
// earlier in this bench.
void loop() {
  static char line[16];
  static int n = 0;
  static char prev = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();

    // A terminal sending CRLF sends two characters. Treating both as line
    // ends made the LF look like an empty line of its own, and an empty
    // line means "next page" -- which is why typing C1 landed on C2.
    if (c == '\n' && prev == '\r') { prev = c; continue; }
    prev = c;

    if (c == '\r' || c == '\n') {
      Serial.println();
      line[n] = 0;
      n = 0;
      handleCommand(line);
    } else if (c == 8 || c == 127) {
      // Backspace and DEL. The terminal's own echo can move the cursor
      // back but cannot rub anything out, so the erase is written here:
      // back up, paint a space over the character, back up again.
      if (n > 0) {
        n--;
        Serial.print("\b \b");
      }
    } else if (c >= ' ' && n < (int)sizeof(line) - 1) {
      line[n++] = c;
      Serial.write(c);
    }
  }
  delay(20);
}
