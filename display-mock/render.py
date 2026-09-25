#!/usr/bin/env python3
"""mCOLD e-paper screen mockups.

Renders every display state of the 2.13" Waveshare (G) panel at its real
250x122 geometry using three of its inks (white, black, red -- yellow is
unused by choice). Text is rasterised 1-bit with hinting, so the preview
carries no greys and no soft edges the panel cannot reproduce.

The layout tables and the icon bitmaps here are the firmware view model;
only the backend changes when the panel driver comes up.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

W, H = 250, 122

WHITE = (255, 255, 255)
BLACK = (26, 26, 26)
RED = (198, 44, 38)

FONTS = Path(__file__).parent.parent / "mcold-spec" / "fonts"
OUT = Path(__file__).parent / "out"


def font(weight, size):
    """Basic layout keeps advances on whole pixels; Raqm's fractional ones
    smear tracked caps."""
    return ImageFont.truetype(str(FONTS / f"IBMPlexSansThai-{weight}.ttf"), size,
                              layout_engine=ImageFont.Layout.BASIC)


def mono(img):
    """A draw context that rasterises text 1-bit with hinting, no antialiasing."""
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    return d


# Type scale: weight rises with size. Hinted 1-bit rendering holds a Light
# stem at one pixel, so small text stays thin and large text carries the weight.
CHROME = font("Light", 12)       # header bar, footer percentages
LABEL = font("Light", 11)        # tracked caps: status line, min/max labels
BODY = font("Light", 13)         # sentences on takeover screens
READING = font("Light", 15)      # min/max values
READINGS = [font("Light", s) for s in (15, 13, 12)]  # right-aligned values
STATUS = font("Medium", 13)      # charge state word
TITLE = font("Bold", 26)         # takeover headline
BIG = font("Bold", 44)           # state of charge
HEROES = [font("Bold", s) for s in (54, 48, 42)]
HERO_UNIT = font("Light", 18)
TRACK = 0.8
BATT_LOW = 20       # % at or below -> battery indicator red
MEM_HIGH = 90       # % used at or above -> storage indicator red

# Layout bands
BAR = 21            # header bar height
RULE = 100          # footer divider
M = 8               # side margin
EDGE_R = W - M
BASE_BAR = 14       # header baseline
BASE_HERO = 74      # temperature, fixed on every monitor screen
BASE_BOTTOM = 96    # min/max, or a caps note when there is no trip
ICON_BOTTOM = 115   # icons and footer text sit on this line

# 1-bit icons, drawn on the pixel grid rather than scaled from vector art.
ICONS = {
    "wifi": ('.#######.',
             '#.......#',
             '..#####..',
             '.#.....#.',
             '...###...',
             '....#....'),
    "cloud": ('...####....',
              '..#....##..',
              '.#.......#.',
              '#.........#',
              '#.........#',
              '.#########.'),
    "bolt": ('...##',
             '..##.',
             '.##..',
             '#####',
             '..##.',
             '.##..',
             '##...'),
    "trip": ('##.....',
             '####...',
             '######.',
             '#######',
             '######.',
             '####...',
             '##.....'),
    "shock": ('..#.#.#..',
              '...###...',
              '#..###..#',
              '.#######.',
              '#..###..#',
              '...###...',
              '..#.#.#..'),
}


class Screen:
    """A 250x122 frame that only ever contains white, black and red."""

    def __init__(self, name):
        self.name = name
        self.img = Image.new("RGB", (W, H), WHITE)
        self.d = mono(self.img)

    def _stamp(self, mask, color):
        self.img.paste(color, (0, 0), mask)

    def width(self, txt, fnt, track=0.0):
        if not track:
            return round(self.d.textlength(txt, font=fnt))
        return sum(round(self.d.textlength(c, font=fnt)) + track for c in txt) - track

    def text(self, xy, txt, fnt, color=BLACK, anchor="ls", track=0.0):
        m = Image.new("L", (W, H), 0)
        d = mono(m)
        x, y = round(xy[0]), round(xy[1])
        if not track:
            d.text((x, y), txt, font=fnt, fill=255, anchor=anchor)
        else:
            widths = [round(d.textlength(c, font=fnt)) for c in txt]
            total = sum(widths) + track * (len(txt) - 1)
            if anchor[0] == "r":
                x -= round(total)
            elif anchor[0] == "m":
                x -= round(total / 2)
            for c, w in zip(txt, widths):
                d.text((round(x), y), c, font=fnt, fill=255, anchor="l" + anchor[1])
                x += w + track
        self._stamp(m, color)

    def box(self, xy0, xy1, color=BLACK, fill=True, width=1):
        m = Image.new("L", (W, H), 0)
        d = mono(m)
        if fill:
            d.rectangle([xy0, xy1], fill=255)
        else:
            d.rectangle([xy0, xy1], outline=255, width=width)
        self._stamp(m, color)

    def icon(self, name, x, bottom, color=BLACK, off=False):
        """Blit a 1-bit icon; `off` strikes it through instead of hiding it,
        so a persisted frame never reads as 'indicator missing'."""
        art = ICONS[name]
        h, w = len(art), len(art[0])
        top = bottom - h
        m = Image.new("L", (W, H), 0)
        d = ImageDraw.Draw(m)
        for row, line in enumerate(art):
            for col, px in enumerate(line):
                if px == "#":
                    d.point((x + col, top + row), fill=255)
        if off:
            d.line([(x - 1, top + h), (x + w, top - 1)], fill=255)
        self._stamp(m, color)
        return w

    # ---- shared bands -------------------------------------------------
    def header(self, device, clock):
        """Inverted: white on black. Ink spread thins reversed text, so this
        is the one small text that gets a heavier weight than its size."""
        self.box((0, 0), (W - 1, BAR - 1))
        self.text((M, BASE_BAR), device, CHROME, WHITE, track=TRACK)
        self.text((EDGE_R, BASE_BAR), clock, CHROME, WHITE, anchor="rs", track=TRACK)

    def footer(self, *, trip=False, shock=False, wifi=False, cloud=False,
               charging=False, battery=78, storage=6):
        """One row of state. `storage` is percent USED, so both gauges fill
        toward their own bad news and the red rule reads the same way.
        No SD indicator -- the card is optional and not user-facing."""
        self.box((0, RULE), (W - 1, RULE))

        # Left: always-on indicators first, conditional ones appended to the
        # right, so an icon appearing never shifts the ones already there.
        x = M
        for name, on in (("wifi", wifi), ("cloud", cloud)):
            x += self.icon(name, x, ICON_BOTTOM, off=not on) + 7
        if trip:
            x += self.icon("trip", x, ICON_BOTTOM) + 7
        if shock:
            x += self.icon("shock", x, ICON_BOTTOM, RED) + 7

        # Right: gauge + percentage, built right to left.
        x = EDGE_R
        for kind, pct in (("storage", storage), ("battery", battery)):
            red = pct >= MEM_HIGH if kind == "storage" else pct <= BATT_LOW
            color = RED if red else BLACK
            value = f"{pct}%"
            x -= self.width(value, CHROME)
            self.text((x, ICON_BOTTOM), value, CHROME, color)
            x -= 4
            x -= self.gauge(kind, x, ICON_BOTTOM, pct, color)
            x -= 9
        if charging:
            self.icon("bolt", x - 6, ICON_BOTTOM)

    def gauge(self, kind, right, bottom, pct, color):
        """Battery lies down and shows charge left; storage stands up and
        fills from the bottom. Different silhouettes so neither is read as
        the other, and the fill level carries the value at a glance."""
        m = Image.new("L", (W, H), 0)
        d = mono(m)
        pct = max(0, min(pct, 100))
        if kind == "battery":
            w, h = 15, 7
            x, top = right - w, bottom - h
            d.rectangle([(x, top), (x + 12, bottom - 1)], outline=255)
            d.rectangle([(x + 13, top + 2), (x + 14, bottom - 3)], fill=255)
            fill = round(pct / 100 * 9)
            if fill:
                d.rectangle([(x + 2, top + 2), (x + 1 + fill, bottom - 3)], fill=255)
        else:
            w, h = 8, 11
            x, top = right - w, bottom - h
            d.rectangle([(x + 1, top), (x + w - 2, top)], fill=255)
            d.rectangle([(x, top + 1), (x, bottom - 1)], fill=255)
            d.rectangle([(x + w - 1, top + 1), (x + w - 1, bottom - 1)], fill=255)
            d.rectangle([(x, bottom - 1), (x + w - 1, bottom - 1)], fill=255)
            fill = round(pct / 100 * (h - 3))
            if fill:
                d.rectangle([(x + 2, bottom - 1 - fill), (x + w - 3, bottom - 2)], fill=255)
        self._stamp(m, color)
        return w

    def alarm_frame(self):
        """Alarm is the whole frame, not a badge: visible across a warehouse."""
        self.box((0, 0), (W - 1, H - 1), RED, fill=False, width=3)

    def centered(self, runs, baseline, gap=0):
        """Lay out (text, font, colour, track) runs as one centred group."""
        widths = [self.width(t, f, tr) for t, f, _, tr in runs]
        total = sum(widths) + gap * (len(runs) - 1)
        x = (W - total) / 2
        for (t, f, c, tr), w in zip(runs, widths):
            self.text((x, baseline), t, f, c, track=tr)
            x += w + gap

    def save(self):
        self.img.save(OUT / f"{self.name}.png")
        return self.img


# ---------------------------------------------------------------------
# Template A -- Monitor: temperature centred and largest, min/max last.
# ---------------------------------------------------------------------
def monitor(name, *, device="MCOLD-0117", clock="14:32", temp="4.2",
            red=False, alarm=False, minmax=("2.8", "6.1"), note="", **flags):
    """No words explain the temperature any more: red means this number is
    not to be trusted (out of band, or no valid sample) and a red frame
    means it is an alarm. The reason lives in the LED, the log and the app."""
    s = Screen(name)
    s.header(device, clock)
    hero_color = RED if (red or alarm) else BLACK
    room = W - 2 * M - s.width("°C", HERO_UNIT) - 5
    hero = next((f for f in HEROES if s.width(temp, f) <= room), HEROES[-1])
    s.centered([(temp, hero, hero_color, 0), ("°C", HERO_UNIT, hero_color, 0)],
               BASE_HERO, gap=5)
    if minmax:
        lo, hi = minmax
        s.centered([("MIN", LABEL, BLACK, TRACK), (lo, READING, BLACK, 0),
                    ("MAX", LABEL, BLACK, TRACK), (hi, READING, BLACK, 0)],
                   BASE_BOTTOM, gap=7)
    elif note:
        s.centered([(note, LABEL, BLACK, TRACK)], BASE_BOTTOM)
    s.footer(**flags)
    if alarm:
        s.alarm_frame()
    return s.save()


# ---------------------------------------------------------------------
# Charge screen -- no temperature; the full charging picture instead.
# ---------------------------------------------------------------------
def charge(name, *, device="MCOLD-0117", clock="14:32", soc=62,
           state="FAST CHARGE", rows=(), **flags):
    s = Screen(name)
    s.header(device, clock)
    s.text((M, 66), f"{soc}%", BIG)
    s.text((M, 90), state, STATUS, track=0.4)
    col = 104
    for i, (label, value) in enumerate(rows):
        y = 42 + i * 18
        s.text((col, y), label, LABEL, track=TRACK)
        room = EDGE_R - col - s.width(label, LABEL, TRACK) - 7
        fnt = next((f for f in READINGS if s.width(value, f) <= room), None)
        if fnt is None:
            fnt = READINGS[-1]
            print(f"  warn {name}: {value!r} overflows by "
                  f"{round(s.width(value, fnt) - room)} px")
        s.text((EDGE_R, y), value, fnt, anchor="rs")
    s.footer(**flags)
    return s.save()


# ---------------------------------------------------------------------
# Template B -- Takeover: one headline, one sentence, one data line.
# ---------------------------------------------------------------------
def takeover(name, *, device="MCOLD-0117", clock="14:32", title="", sub="",
             data="", alarm=False, title_color=BLACK, **flags):
    s = Screen(name)
    s.header(device, clock)
    s.text((M, 54), title, TITLE, title_color)
    s.text((M, 76), sub, BODY)
    if data:
        s.text((M, 93), data, LABEL, track=TRACK)
    s.footer(**flags)
    if alarm:
        s.alarm_frame()
    return s.save()


# ---------------------------------------------------------------------
# Template C -- Detail: label/value rows, no hero.
# ---------------------------------------------------------------------
def detail(name, *, device="MCOLD-0117", clock="14:32", title="", rows=(), **flags):
    s = Screen(name)
    s.header(device, clock)
    s.text((M, 36), title, LABEL, track=TRACK)
    for i, (label, value) in enumerate(rows):
        y = 56 + i * 18
        s.text((M, y), label, BODY)
        s.text((EDGE_R, y), value, BODY, anchor="rs")
    s.footer(**flags)
    return s.save()


SCREENS = []
LIVE = dict(trip=True, wifi=True, cloud=True, battery=78, storage=6)


def build():
    OUT.mkdir(exist_ok=True)
    add = SCREENS.append

    add(("A1  IDLE / READY", monitor(
        "A1_idle", minmax=None, note="NO ACTIVE TRIP",
        wifi=True, cloud=True, battery=78, storage=6)))

    add(("A2  TRIP ACTIVE", monitor("A2_trip", **LIVE)))

    add(("A3  OUT OF BAND", monitor(
        "A3_warning", temp="8.4", red=True, minmax=("2.8", "8.4"), **LIVE)))

    add(("A4  ALARM", monitor(
        "A4_alarm", temp="11.6", alarm=True, minmax=("2.8", "11.6"),
        trip=True, shock=True, wifi=True, cloud=False, battery=61, storage=7)))

    add(("A5  NO READING", monitor(
        "A5_noreading", temp="--", red=True, minmax=("2.8", "6.1"),
        trip=True, wifi=False, cloud=False, battery=16, storage=6)))

    add(("A6  CHARGING", charge(
        "A6_charging", soc=62, state="FAST CHARGE", rows=(
            ("SOURCE", "DOCK PD 20V"),
            ("CURRENT", "1.18 A"),
            ("FULL IN", "42 MIN")),
        charging=True, wifi=True, cloud=True, battery=62, storage=6)))

    add(("B1  BOOT / SELF-TEST", takeover(
        "B1_boot", clock="--:--", title="SELF-TEST", sub="Checking sensors and storage",
        data="FIRMWARE 0.1.0 · SERIAL 0117", battery=78, storage=6)))

    add(("B2  USB CONNECTED", takeover(
        "B2_usb", title="USB DRIVE", sub="Read-only. Copy the CSV, then eject.",
        data="SNAPSHOT 14:31 · 2,184 RECORDS", **LIVE)))

    add(("B3  BLE SESSION", takeover(
        "B3_ble", title="MCOLD-0117", sub="Confirm this ID in the app to pair.",
        data="PAIRING WINDOW 60 S", wifi=True, cloud=True, battery=78, storage=6)))

    add(("B4  STORAGE FULL", takeover(
        "B4_storage", title="STORAGE FULL", sub="Oldest finished trip was dropped.",
        data="TRIP 0139 LOST · UPLOAD NOW", alarm=True, title_color=RED,
        trip=True, wifi=True, cloud=False, battery=54, storage=100)))

    add(("B5  CRITICAL BATTERY", takeover(
        "B5_battery", title="BATTERY 4%", sub="Logging stopped. Charge the device.",
        data="LAST RECORD 14:32", alarm=True, title_color=RED,
        wifi=False, cloud=False, battery=4, storage=6)))

    add(("B6  FAULT / RECOVERY", takeover(
        "B6_fault", title="SENSOR FAULT", sub="Temperature bus not responding.",
        data="TRIP CONTINUES · SEE APP", title_color=RED, **LIVE)))

    add(("C1  TRIP SUMMARY", detail(
        "C1_summary", title="TRIP 0142 CLOSED", rows=(
            ("Duration", "3d 06h 12m"),
            ("Temperature", "2.8 / 6.1 °C"),
            ("Alarms", "1 high · 2 shock")),
        wifi=True, cloud=True, battery=74, storage=9)))

    add(("C2  DETAIL (NFC TAP)", detail(
        "C2_detail", title="DEVICE DETAIL", rows=(
            ("Trip", "0142 · 21 Sep 08:20"),
            ("Upload", "184 records pending"),
            ("Firmware", "0.1.0 · RTC OK")),
        **LIVE)))

    contact_sheet()
    print(f"{len(SCREENS)} screens -> {OUT}")


def contact_sheet(scale=2, cols=3, gap=18, label_h=20):
    cw, ch = W * scale, H * scale
    rows = (len(SCREENS) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cw + gap * (cols + 1),
                              rows * (ch + label_h + gap) + gap), (245, 245, 245))
    d = ImageDraw.Draw(sheet)
    lbl = font("SemiBold", 13)
    for i, (name, img) in enumerate(SCREENS):
        x = gap + (i % cols) * (cw + gap)
        y = gap + (i // cols) * (ch + label_h + gap)
        d.text((x, y), name, font=lbl, fill=(40, 40, 40))
        big = img.resize((cw, ch), Image.NEAREST)
        sheet.paste(big, (x, y + label_h))
        d.rectangle([x - 1, y + label_h - 1, x + cw, y + label_h + ch], outline=(200, 200, 200))
        big.save(OUT / f"{name.split()[0]}_2x.png")
    sheet.save(OUT / "contact_sheet.png")


if __name__ == "__main__":
    build()
