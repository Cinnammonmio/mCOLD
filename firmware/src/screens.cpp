#include "screens.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fonts_mcold.h"
#include "fonts_head.h"

namespace {

// ---- the grid, straight from render.py --------------------------------

// The case's bezel covers a few pixels on every side (2026-10-04), so
// the layout is drawn into the area it leaves visible: the insets are
// config epd_inset_t/b/l/r, measured with `screen cal`. Everything below
// is relative to that area; with no insets it is render.py's grid.
int INS_T = 0, INS_B = 0, INS_L = 0, INS_R = 0;
int W = CANVAS_W;
int H = CANVAS_H;
const int BAR = 17;          // header bar height
int RULE = 100;              // footer divider
const int M = 8;             // side margin
int EDGE_R = W - M;
const int GAP = 7;           // between footer icons
const int BASE_BAR = 12;     // header baseline
int BASE_HERO = 80;          // temperature, fixed on every monitor screen
int BASE_BOTTOM = 96;        // min/max, or a caps note when there is no trip
int BASE_FOOT = 118;         // footer percentages
int ICON_BOTTOM = 120;       // footer icons stop one row above this

void layout(void) {
  W = CANVAS_W - INS_L - INS_R;
  H = CANVAS_H - INS_T - INS_B;
  EDGE_R = W - M;
  RULE = H - 22;
  BASE_FOOT = H - 4;
  ICON_BOTTOM = H - 2;
  BASE_BOTTOM = RULE - 4;
  BASE_HERO = RULE - 20;
}

// Every template starts here: blank panel, origin at the safe area.
void begin(Canvas &c) {
  c.ox = 0;
  c.oy = 0;
  c.clear();
  c.ox = INS_L;
  c.oy = INS_T;
}
const int HERO_CAP = 57;     // cap height of the 80 px hero
const float TRACK = 1;       // whole pixels: fractional advances smear
const int BATT_LOW = 20;     // % at or below -> battery in accent
const int MEM_HIGH = 90;     // % used at or above -> storage in accent
const int BATT_W = 11;
const int BATT_H = 16;

const GFXfont *CAPS = &mColdCaps11;     // footer %, labels
// The header: bold, and with lower case, because the SN has it
// (mCDV1-...) and white-on-black at 11 px semibold read thin (2026-10-04).
const GFXfont *HEAD = &mColdHead12;
const GFXfont *BODY = &mColdBody13;     // sentences on takeover screens
const GFXfont *READING = &mColdRead15;  // min/max and charge-row values
const GFXfont *STATUS = &mColdStat13;   // charge state word
const GFXfont *TITLE = &mColdTitle26;   // takeover headline
const GFXfont *BIG = &mColdBig44;       // state of charge
const GFXfont *HERO = &mColdHero80;
const GFXfont *HERO_UNIT = &mColdUnit20;

// ---- text, measured as render.py measures it ---------------------------

int adv(char ch, const GFXfont *f) {
  const uint8_t c = (uint8_t)ch;
  if (c < f->first || c > f->last) return 0;
  return f->glyph[c - f->first].xAdvance;
}

float width(const char *s, const GFXfont *f, float track = 0) {
  float w = 0;
  int n = 0;
  for (const char *p = s; *p; p++, n++) w += adv(*p, f);
  return n > 1 ? w + track * (n - 1) : w;
}

// Left baseline at x; with `right`, x is where the text ends.
void text(Canvas &c, float x, int y, const char *s, const GFXfont *f, Ink ink,
          float track = 0, bool right = false) {
  if (right) x -= width(s, f, track);
  if (!track) {
    c.text((int)lroundf(x), y, s, f, ink);
    return;
  }
  char one[2] = {0, 0};
  for (const char *p = s; *p; p++) {
    one[0] = *p;
    c.text((int)lroundf(x), y, one, f, ink);
    x += adv(*p, f) + track;
  }
}

// ---- icons, as drawn in display-mock/icons.py ---------------------------

struct Icon {
  uint8_t w, h;
  const char *rows[16];
};

const Icon ICON_WIFI = {20, 16, {"........####........",
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

const Icon ICON_CLOUD = {20, 16, {"....................",
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

const Icon ICON_SHOCK = {20, 16, {"..............#.....",
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

const Icon ICON_GNSS = {12, 16, {"....####....",
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

const Icon ICON_STORAGE = {15, 16, {"....#######....",
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

const Icon ICON_CHARGE = {9, 16, {".....#...",
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

const Icon ICON_TRIP = {9, 16, {"#........",
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

// `off` strikes the icon through instead of hiding it: a frame persists
// on the glass after power is gone, and "not connected" must not look
// like "not refreshed".
int icon(Canvas &c, const Icon &ic, int x, int bottom, Ink ink, bool off = false) {
  const int top = bottom - ic.h;
  for (int r = 0; r < ic.h && ic.rows[r]; r++) {
    const char *row = ic.rows[r];
    for (int k = 0; k < ic.w && row[k]; k++) {
      if (row[k] == '#') c.pixel(x + k, top + r, ink);
    }
  }
  if (off) c.line(x - 1, top + ic.h, x + ic.w, top - 1, ink);
  return ic.w;
}

// ---- shared bands --------------------------------------------------------

void header(Canvas &c, const char *device, const char *clock) {
  // Inverted. Ink spread thins reversed text, which is why the header is
  // the one small text set in SemiBold.
  c.fill_rect(0, 0, W, BAR, Ink::Black);
  text(c, M, BASE_BAR, device, HEAD, Ink::White);
  text(c, EDGE_R, BASE_BAR, clock, HEAD, Ink::White, 0, true);
}

// The battery: a cell on end, four blocks. Blocks are counted at a
// glance; on a panel with no greys a block is there or it is not.
void cell(Canvas &c, int x, int bottom, int pct, Ink ink) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  const int top = bottom - BATT_H;
  c.fill_rect(x + 3, top, 5, 1, ink);
  c.rect(x, top + 1, BATT_W, BATT_H - 1, ink);
  int lit = (pct * 4 + 50) / 100;
  if (lit < 1 && pct > 0) lit = 1;     // 1 % and 0 % must not look the same
  for (int i = 0; i < lit && i < 4; i++) {
    c.fill_rect(x + 2, top + 12 - i * 3, BATT_W - 4, 2, ink);
  }
}

void footer(Canvas &c, const Foot &f) {
  c.hline(0, RULE, W, Ink::Black);

  int x = M;
  x += icon(c, ICON_WIFI, x, ICON_BOTTOM, Ink::Black, !f.wifi) + GAP;
  x += icon(c, ICON_CLOUD, x, ICON_BOTTOM, Ink::Black, !f.cloud) + GAP;
  x += icon(c, ICON_GNSS, x, ICON_BOTTOM, Ink::Black, !f.gnss) + GAP;
  if (f.trip) x += icon(c, ICON_TRIP, x, ICON_BOTTOM, Ink::Black) + GAP;
  if (f.shock) x += icon(c, ICON_SHOCK, x, ICON_BOTTOM, Ink::Accent) + GAP;
  const int left_end = x - GAP;

  // Right side, built right to left.
  char s[8];
  float r = EDGE_R;
  Ink ink = f.mem >= MEM_HIGH ? Ink::Accent : Ink::Black;
  snprintf(s, sizeof(s), "%d%%", f.mem);
  r -= width(s, CAPS, TRACK);
  text(c, r, BASE_FOOT, s, CAPS, ink, TRACK);
  r -= 5 + ICON_STORAGE.w;
  icon(c, ICON_STORAGE, (int)lroundf(r), ICON_BOTTOM, ink);
  r -= 11;

  // No fuel gauge reading is "--", never 0 %: an empty cell would send the
  // box for charging because an I2C read failed.
  ink = (f.batt >= 0 && f.batt <= BATT_LOW) ? Ink::Accent : Ink::Black;
  if (f.batt >= 0) snprintf(s, sizeof(s), "%d%%", f.batt);
  else snprintf(s, sizeof(s), "--");
  r -= width(s, CAPS, TRACK);
  text(c, r, BASE_FOOT, s, CAPS, ink, TRACK);
  r -= 5 + BATT_W;
  cell(c, (int)lroundf(r), ICON_BOTTOM, f.batt < 0 ? 0 : f.batt, ink);

  // The bolt goes outside the cell, so it never covers the reading.
  if (f.charging) {
    r -= 5 + ICON_CHARGE.w;
    icon(c, ICON_CHARGE, (int)lroundf(r), ICON_BOTTOM, Ink::Black);
  }
  if (left_end > r) printf("[screen] footer groups overlap by %d px\n",
                           (int)(left_end - r));
}

void alarm_frame(Canvas &c) {
  // The whole frame, not a badge: visible across a warehouse.
  for (int i = 0; i < 3; i++) c.rect(i, i, W - 2 * i, H - 2 * i, Ink::Accent);
}

void unit(Canvas &c, int x, int top, Ink ink) {
  // A disc with a smaller disc punched out of it: two outlines leave
  // white specks where the circles fail to meet.
  c.fill_circle(x + 5, top + 8, 5, ink);
  c.fill_circle(x + 5, top + 8, 3, Ink::White);
  text(c, x + 13, top + 24, "C", HERO_UNIT, ink);
}

void minmax(Canvas &c, const char *lo, const char *hi) {
  // Tight inside a pair, wide between pairs, a dot in the wide gap.
  const float pair = 5, split = 26;
  const float w_min = width("MIN", CAPS, TRACK), w_max = width("MAX", CAPS, TRACK);
  const float w_lo = width(lo, READING), w_hi = width(hi, READING);
  float x = (W - (w_min + pair + w_lo + split + w_max + pair + w_hi)) / 2;
  text(c, x, BASE_BOTTOM, "MIN", CAPS, Ink::Black, TRACK);
  x += w_min + pair;
  text(c, x, BASE_BOTTOM, lo, READING, Ink::Black);
  x += w_lo;
  const int mid = (int)lroundf(x) + (int)split / 2;
  c.fill_rect(mid, BASE_BOTTOM - 6, 2, 2, Ink::Black);
  x += split;
  text(c, x, BASE_BOTTOM, "MAX", CAPS, Ink::Black, TRACK);
  x += w_max + pair;
  text(c, x, BASE_BOTTOM, hi, READING, Ink::Black);
}

// '|' middle dot, '~' degree ring; measured and drawn by one routine.
float rich(Canvas *c, float x, int y, const char *s, const GFXfont *f, Ink ink,
           float track) {
  const float x0 = x;
  char run[80];
  int n = 0;
  for (const char *p = s;; p++) {
    if (*p == '|' || *p == '~' || !*p) {
      run[n] = 0;
      if (n) {
        if (c) text(*c, x, y, run, f, ink, track);
        x += width(run, f, track);
        n = 0;
      }
      if (!*p) break;
      const int rx = (int)lroundf(x);
      if (*p == '|') {
        if (c) c->fill_rect(rx + 4, y - 4, 2, 2, ink);
        x += 10;
      } else {
        if (c) {
          c->fill_rect(rx + 1, y - 10, 3, 3, ink);
          c->pixel(rx + 2, y - 9, Ink::White);
        }
        x += 6;
      }
    } else if (n < (int)sizeof(run) - 1) {
      run[n++] = *p;
    }
  }
  return x - x0;
}

}  // namespace

// ---- templates -------------------------------------------------------------

void scr_monitor(Canvas &c, const char *device, const char *clock,
                 const Foot &f, const char *temp, const char *lo,
                 const char *hi, const char *note, bool red, bool alarm) {
  begin(c);
  header(c, device, clock);
  const Ink hero = (red || alarm) ? Ink::Accent : Ink::Black;
  const float unit_w = 13 + width("C", HERO_UNIT);
  const float w_num = width(temp, HERO);
  // The number is centred on its own; the unit hangs off its right side
  // (2026-10-04), so the reading sits in the middle of the panel whatever
  // its width -- unless the unit would then run off the edge.
  float x = (W - w_num) / 2;
  if (x + w_num + 5 + unit_w > EDGE_R) x = EDGE_R - (w_num + 5 + unit_w);
  text(c, x, BASE_HERO, temp, HERO, hero);
  unit(c, (int)lroundf(x + w_num + 5), BASE_HERO - HERO_CAP, hero);
  if (lo && hi) {
    minmax(c, lo, hi);
  } else if (note) {
    text(c, (W - width(note, CAPS, TRACK)) / 2, BASE_BOTTOM, note, CAPS,
         Ink::Black, TRACK);
  }
  footer(c, f);
  if (alarm) alarm_frame(c);
}

void scr_charge(Canvas &c, const char *device, const char *clock,
                const Foot &f, int soc, const char *state, const Row *rows,
                int nrows) {
  begin(c);
  header(c, device, clock);
  char s[8];
  snprintf(s, sizeof(s), "%d%%", soc);
  text(c, M, 66, s, BIG, Ink::Black);
  text(c, M, 90, state, STATUS, Ink::Black, 0.4f);
  const int col = 104;
  for (int i = 0; i < nrows && i < 3; i++) {
    const int y = 42 + i * 18;
    text(c, col, y, rows[i].label, CAPS, Ink::Black, TRACK);
    text(c, EDGE_R, y, rows[i].value, READING, Ink::Black, 0, true);
  }
  footer(c, f);
}

void scr_takeover(Canvas &c, const char *device, const char *clock,
                  const Foot &f, const char *title, const char *sub,
                  const char *data, bool alarm, bool red_title) {
  begin(c);
  header(c, device, clock);
  text(c, M, 54, title, TITLE, red_title ? Ink::Accent : Ink::Black);
  text(c, M, 76, sub, BODY, Ink::Black);
  if (data) rich(&c, M, 93, data, CAPS, Ink::Black, TRACK);
  footer(c, f);
  if (alarm) alarm_frame(c);
}

void scr_detail(Canvas &c, const char *device, const char *clock,
                const Foot &f, const char *title, const char *state,
                const Row *rows, int nrows) {
  begin(c);
  header(c, device, clock);
  text(c, M, 36, title, CAPS, Ink::Black, TRACK);
  if (state) text(c, EDGE_R, 36, state, CAPS, Ink::Black, TRACK, true);
  for (int i = 0; i < nrows && i < 3; i++) {
    const int y = 56 + i * 18;
    text(c, M, y, rows[i].label, BODY, Ink::Black);
    const float w = rich(nullptr, 0, 0, rows[i].value, BODY, Ink::Black, 0);
    rich(&c, EDGE_R - w, y, rows[i].value, BODY, Ink::Black, 0);
  }
  footer(c, f);
}

// ---- the design's fourteen states, with the mockup's sample data -----------

namespace {
const char *const DEMO_NAMES[SCR_DEMO_PAGES] = {
    "A1 IDLE",        "A2 TRIP ACTIVE",   "A3 OUT OF BAND",    "A4 ALARM",
    "A5 NO READING",  "A6 CHARGING",      "B1 BOOT",           "B2 USB",
    "B3 BLE",         "B4 STORAGE FULL",  "B5 CRITICAL BATT",  "B6 FAULT",
    "C1 TRIP SUMMARY", "C2 DEVICE DETAIL",
};
}  // namespace

const char *scr_demo_name(int p) {
  return (p >= 0 && p < SCR_DEMO_PAGES) ? DEMO_NAMES[p] : "?";
}

void scr_set_insets(int top, int bottom, int left, int right) {
  INS_T = top;
  INS_B = bottom;
  INS_L = left;
  INS_R = right;
  layout();
}

void scr_calibrate(Canvas &c) {
  c.ox = c.oy = 0;
  c.clear();
  // Six nested frames, 3 px apart, on the physical panel: count, on each
  // side, the frames that can be seen; the hidden ones say how much the
  // bezel covers there.
  for (int k = 0; k < 6; k++) {
    const int d = k * 3;
    c.rect(d, d, CANVAS_W - 2 * d, CANVAS_H - 2 * d, Ink::Black);
  }
  text(c, 26, 46, "COUNT THE FRAMES", CAPS, Ink::Black, TRACK);
  text(c, 26, 62, "YOU CAN SEE ON EACH SIDE", CAPS, Ink::Black, TRACK);
  text(c, 26, 82, "6 = NOTHING HIDDEN", CAPS, Ink::Black, TRACK);
  text(c, 26, 98, "EACH MISSING = 3 PX", CAPS, Ink::Black, TRACK);
}

void scr_demo(Canvas &c, int p) {
  const char *dev = "MCOLD-0117";
  // trip, wifi, cloud, gnss, shock, charging, batt, mem
  switch (p) {
    case 0: scr_monitor(c, dev, "14:32", {false, true, true, true, false, false, 78, 6},
                        "4.2", nullptr, nullptr, "NO ACTIVE TRIP", false, false); break;
    case 1: scr_monitor(c, dev, "14:32", {true, true, true, true, false, false, 78, 6},
                        "4.2", "2.8", "6.1", nullptr, false, false); break;
    case 2: scr_monitor(c, dev, "14:32", {true, true, true, true, false, false, 78, 6},
                        "8.4", "2.8", "8.4", nullptr, true, false); break;
    case 3: scr_monitor(c, dev, "14:32", {true, true, false, true, true, false, 61, 7},
                        "11.6", "2.8", "11.6", nullptr, false, true); break;
    case 4: scr_monitor(c, dev, "14:32", {true, false, false, false, false, false, 16, 6},
                        "--", "2.8", "6.1", nullptr, true, false); break;
    case 5: {
      static const Row r[] = {{"SOURCE", "DOCK PD 20V"}, {"CURRENT", "1.18 A"},
                              {"FULL IN", "42 MIN"}};
      scr_charge(c, dev, "14:32", {false, true, true, true, false, true, 62, 6},
                 62, "FAST CHARGE", r, 3);
      break;
    }
    case 6: scr_takeover(c, dev, "--:--", {false, false, false, false, false, false, 78, 6},
                         "SELF-TEST", "Checking sensors and storage",
                         "FIRMWARE 0.1.0|SERIAL 0117", false, false); break;
    case 7: scr_takeover(c, dev, "14:32", {true, true, true, true, false, false, 78, 6},
                         "USB DRIVE", "Read-only. Copy the CSV, then eject.",
                         "SNAPSHOT 14:31|2,184 RECORDS", false, false); break;
    case 8: scr_takeover(c, dev, "14:32", {false, true, true, true, false, false, 78, 6},
                         "MCOLD-0117", "Confirm this ID in the app to pair.",
                         "PAIRING WINDOW 60 S", false, false); break;
    case 9: scr_takeover(c, dev, "14:32", {true, true, false, true, false, false, 54, 100},
                         "STORAGE FULL", "Oldest finished trip was dropped.",
                         "TRIP 0139 LOST|UPLOAD NOW", true, true); break;
    case 10: scr_takeover(c, dev, "14:32", {false, false, false, false, false, false, 4, 6},
                          "BATTERY 4%", "Logging stopped. Charge the device.",
                          "LAST RECORD 14:32", true, true); break;
    case 11: scr_takeover(c, dev, "14:32", {true, true, true, true, false, false, 78, 6},
                          "SENSOR FAULT", "Temperature bus not responding.",
                          "TRIP CONTINUES|SEE APP", false, true); break;
    case 12: {
      static const Row r[] = {{"Duration", "3d 06h 12m"}, {"Temperature", "2.8 / 6.1 ~C"},
                              {"Alarms", "1 high|2 shock"}};
      scr_detail(c, dev, "14:32", {false, true, true, true, false, false, 74, 9},
                 "TRIP 0142", "CLOSED", r, 3);
      break;
    }
    default: {
      static const Row r[] = {{"Trip", "0142|21 Sep 08:20"}, {"Upload", "184 records pending"},
                              {"Firmware", "0.1.0|RTC OK"}};
      scr_detail(c, dev, "14:32", {true, true, true, true, false, false, 78, 6},
                 "DEVICE", "DETAIL", r, 3);
      break;
    }
  }
}
