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

import icons

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
# Every tracked caps run -- header, footer percentages, min/max labels,
# data lines -- is one face. Light 11 was tried first and lost: at this
# size a Light stem is a single pixel, and against the black header bar
# ink spread eats it, so the caps carry a heavier weight than their size
# would suggest while the sentences and readings stay Light.
CAPS = font("SemiBold", 11)      # header, footer %, labels, data lines
BODY = font("Light", 13)         # sentences on takeover screens
READING = font("Light", 15)      # min/max and charge-row values
READINGS = [font("Light", s) for s in (15, 13, 12)]  # right-aligned values
STATUS = font("Medium", 13)      # charge state word
TITLE = font("Bold", 26)         # takeover headline
BIG = font("Bold", 44)           # state of charge
HEROES = [font("Bold", s) for s in (80, 72, 64)]
HERO_UNIT = font("Bold", 20)     # the C of the unit; the ring is drawn
HERO_CAP = 57                    # cap height of the 80px hero
TRACK = 1                        # whole pixels: fractional advances smear
BATT_LOW = 20       # % at or below -> battery indicator red
MEM_HIGH = 90       # % used at or above -> storage indicator red

# Layout bands
BAR = 17            # header bar height
RULE = 100          # footer divider
M = 8               # side margin
EDGE_R = W - M
GAP = 7             # between footer icons
BASE_BAR = 12       # header baseline
BASE_HERO = 80      # temperature, fixed on every monitor screen
BASE_BOTTOM = 96    # min/max, or a caps note when there is no trip
BASE_FOOT = 118     # footer percentages
ICON_BOTTOM = 120   # footer icons stop one row above this

# One height for every footer mark, from icons.py. Before that each shape
# was drawn at whatever size suited it and the row read as a set of
# mismatched stickers; they share a height now and sit on one line, and
# the widths still follow the shape because a cloud is flat and a bolt is
# not. The battery is the one exception: it is a gauge, so it is drawn
# from its value rather than blitted.
ICONS = icons.bitmaps()
BATT_W, BATT_H = 11, icons.ICON_H


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
        self.text((M, BASE_BAR), device, CAPS, WHITE, track=TRACK)
        self.text((EDGE_R, BASE_BAR), clock, CAPS, WHITE, anchor="rs", track=TRACK)

    def footer(self, *, trip=False, shock=False, wifi=False, cloud=False,
               gnss=False, charging=False, battery=78, storage=6):
        """One row of state. `storage` is percent USED, so both readings
        move toward their own bad news and the red rule reads the same way
        for each. No SD indicator -- the card is optional and not
        user-facing -- and no door, which is logged but not shown."""
        self.box((0, RULE), (W - 1, RULE))

        # Wi-Fi, cloud and GNSS are always drawn -- struck through rather
        # than hidden when they have nothing, because a frame persists on
        # the glass after power is gone and "not connected" must not look
        # like "not refreshed". The two that come and go sit after them,
        # so neither ever shifts what is already there.
        x = M
        for name, on in (("wifi", wifi), ("cloud", cloud), ("gnss", gnss)):
            x += self.icon(name, x, ICON_BOTTOM, off=not on) + GAP
        if trip:
            x += self.icon("trip", x, ICON_BOTTOM) + GAP
        if shock:
            x += self.icon("shock", x, ICON_BOTTOM, RED) + GAP
        left_end = x - GAP

        # Right: value and mark, built right to left.
        x = EDGE_R
        value, color = f"{storage}%", RED if storage >= MEM_HIGH else BLACK
        x -= self.width(value, CAPS, TRACK)
        self.text((x, BASE_FOOT), value, CAPS, color, track=TRACK)
        x -= 5 + len(ICONS["storage"][0])
        self.icon("storage", x, ICON_BOTTOM, color)
        x -= 11

        value, color = f"{battery}%", RED if battery <= BATT_LOW else BLACK
        x -= self.width(value, CAPS, TRACK)
        self.text((x, BASE_FOOT), value, CAPS, color, track=TRACK)
        x -= 5 + BATT_W
        self.cell(x, ICON_BOTTOM, battery, color)

        # The bolt goes outside the cell, not inside it: the four blocks
        # are the reading, and a bolt on top of them would cover the one
        # thing the mark exists to say.
        if charging:
            x -= 5 + len(ICONS["charge"][0])
            self.icon("charge", x, ICON_BOTTOM)

        if left_end > x:
            print(f"  warn {self.name}: footer groups overlap by "
                  f"{round(left_end - x)} px")

    def cell(self, x, bottom, pct, color):
        """The battery: a cell standing on end, split into four blocks.
        Blocks are counted at a glance where the length of a bar has to be
        judged against nothing, and on a panel with no greys a block is
        either there or it is not -- there is no half-lit state to
        misread. It is two rows taller than the storage pictogram beside
        it because at the same height the four blocks only fit by running
        into the shell, which made the end two look thicker than the
        middle two."""
        m = Image.new("L", (W, H), 0)
        d = mono(m)
        pct = max(0, min(pct, 100))
        top = bottom - BATT_H
        d.rectangle([(x + 3, top), (x + 7, top)], fill=255)
        d.rectangle([(x, top + 1), (x + BATT_W - 1, top + BATT_H - 1)],
                    outline=255)
        # Nearest quarter, but never an empty cell while the thing is
        # still running: 1% and 0% must not draw the same picture.
        lit = (pct * 4 + 50) // 100
        if lit < 1 and pct > 0:
            lit = 1
        for i in range(min(lit, 4)):
            y = top + 12 - i * 3
            d.rectangle([(x + 2, y), (x + 2 + BATT_W - 5, y + 1)], fill=255)
        self._stamp(m, color)
        return BATT_W

    def alarm_frame(self):
        """Alarm is the whole frame, not a badge: visible across a warehouse."""
        self.box((0, 0), (W - 1, H - 1), RED, fill=False, width=3)

    def unit(self, x, top, color):
        """The degree ring, hung off the top of the numeral. A disc with a
        smaller disc punched out of it: two concentric circle outlines
        look like the same ring and leave eight white specks where the
        two rasterised circles fail to meet, one at each octant."""
        m = Image.new("L", (W, H), 0)
        d = mono(m)
        d.ellipse([(x, top + 3), (x + 10, top + 13)], fill=255)
        d.ellipse([(x + 2, top + 5), (x + 8, top + 11)], fill=0)
        self._stamp(m, color)
        self.text((x + 13, top + 24), "C", HERO_UNIT, color)

    def minmax(self, lo, hi):
        """A label and the value it names belong together; the two pairs
        do not. Even gaps made the four items read as one run, so the gap
        inside a pair is tight, the gap between pairs is wide, and a dot
        sits in the middle of the wide one. A vertical rule was tried
        first and read as a digit with figures on both sides of it."""
        pair, split = 5, 26
        w_min = self.width("MIN", CAPS, TRACK)
        w_max = self.width("MAX", CAPS, TRACK)
        w_lo, w_hi = self.width(lo, READING), self.width(hi, READING)
        x = (W - (w_min + pair + w_lo + split + w_max + pair + w_hi)) / 2
        self.text((x, BASE_BOTTOM), "MIN", CAPS, track=TRACK)
        x += w_min + pair
        self.text((x, BASE_BOTTOM), lo, READING)
        x += w_lo
        mid = round(x) + split // 2
        self.box((mid, BASE_BOTTOM - 6), (mid + 1, BASE_BOTTOM - 5))
        x += split
        self.text((x, BASE_BOTTOM), "MAX", CAPS, track=TRACK)
        x += w_max + pair
        self.text((x, BASE_BOTTOM), hi, READING)

    def rich(self, x, y, txt, fnt, color=BLACK, track=0.0, draw=True):
        """Text carrying two marks the subset fonts cannot: '|' is the
        middle dot between data items, '~' the degree ring after a
        temperature. The firmware draws both rather than setting them,
        because its font tables are ASCII, so they are drawn here too --
        a mockup that uses the real glyphs would be showing a screen the
        panel cannot produce. Measuring and drawing share this one path,
        so a right-aligned value cannot be measured by different rules
        than it is painted with."""
        dot_w, ring_w = 10, 6
        x0, run = x, ""
        for ch in list(txt) + [None]:
            if ch is None or ch in "|~":
                if run:
                    if draw:
                        self.text((x, y), run, fnt, color, track=track)
                    x += self.width(run, fnt, track)
                    run = ""
                if ch is None:
                    break
                if ch == "|":
                    if draw:
                        self.box((round(x) + 4, y - 4), (round(x) + 5, y - 3), color)
                    x += dot_w
                else:
                    if draw:
                        self.box((round(x) + 1, y - 10), (round(x) + 3, y - 8), color)
                        self.box((round(x) + 2, y - 9), (round(x) + 2, y - 9), WHITE)
                    x += ring_w
            else:
                run += ch
        return x - x0

    def rich_width(self, txt, fnt, track=0.0):
        return self.rich(0, 0, txt, fnt, track=track, draw=False)

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
    unit_w = 13 + s.width("C", HERO_UNIT)
    room = W - 2 * M - unit_w - 5
    hero = next((f for f in HEROES if s.width(temp, f) <= room), HEROES[-1])
    w_num = s.width(temp, hero)
    x = (W - (w_num + 5 + unit_w)) / 2
    s.text((x, BASE_HERO), temp, hero, hero_color)
    s.unit(round(x + w_num + 5), BASE_HERO - HERO_CAP, hero_color)
    if minmax:
        s.minmax(*minmax)
    elif note:
        s.centered([(note, CAPS, BLACK, TRACK)], BASE_BOTTOM)
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
        s.text((col, y), label, CAPS, track=TRACK)
        room = EDGE_R - col - s.width(label, CAPS, TRACK) - 7
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
        room = W - 2 * M
        if s.rich_width(data, CAPS, TRACK) > room:
            print(f"  warn {name}: data line overflows")
        s.rich(M, 93, data, CAPS, track=TRACK)
    s.footer(**flags)
    if alarm:
        s.alarm_frame()
    return s.save()


# ---------------------------------------------------------------------
# Template C -- Detail: label/value rows, no hero.
# ---------------------------------------------------------------------
def detail(name, *, device="MCOLD-0117", clock="14:32", title="", state="",
           rows=(), **flags):
    """The title takes the same two columns as the rows under it: what the
    page is about on the left, its state on the right. Set as one string
    the two ran together -- "TRIP 0142 CLOSED" is a four-word blur at caps
    11, with nothing to tell the number from the word."""
    s = Screen(name)
    s.header(device, clock)
    s.text((M, 36), title, CAPS, track=TRACK)
    if state:
        s.text((EDGE_R, 36), state, CAPS, anchor="rs", track=TRACK)
    for i, (label, value) in enumerate(rows):
        y = 56 + i * 18
        s.text((M, y), label, BODY)
        w = s.rich_width(value, BODY)
        s.rich(EDGE_R - w, y, value, BODY)
    s.footer(**flags)
    return s.save()


SCREENS = []
LIVE = dict(trip=True, wifi=True, cloud=True, gnss=True, battery=78, storage=6)


def build():
    OUT.mkdir(exist_ok=True)
    add = SCREENS.append

    add(("A1  IDLE / READY", monitor(
        "A1_idle", minmax=None, note="NO ACTIVE TRIP",
        wifi=True, cloud=True, gnss=True, battery=78, storage=6)))

    add(("A2  TRIP ACTIVE", monitor("A2_trip", **LIVE)))

    add(("A3  OUT OF BAND", monitor(
        "A3_warning", temp="8.4", red=True, minmax=("2.8", "8.4"), **LIVE)))

    add(("A4  ALARM", monitor(
        "A4_alarm", temp="11.6", alarm=True, minmax=("2.8", "11.6"),
        trip=True, shock=True, wifi=True, cloud=False, gnss=True,
        battery=61, storage=7)))

    add(("A5  NO READING", monitor(
        "A5_noreading", temp="--", red=True, minmax=("2.8", "6.1"),
        trip=True, wifi=False, cloud=False, battery=16, storage=6)))

    add(("A6  CHARGING", charge(
        "A6_charging", soc=62, state="FAST CHARGE", rows=(
            ("SOURCE", "DOCK PD 20V"),
            ("CURRENT", "1.18 A"),
            ("FULL IN", "42 MIN")),
        charging=True, wifi=True, cloud=True, gnss=True,
        battery=62, storage=6)))

    add(("B1  BOOT / SELF-TEST", takeover(
        "B1_boot", clock="--:--", title="SELF-TEST", sub="Checking sensors and storage",
        data="FIRMWARE 0.1.0|SERIAL 0117", battery=78, storage=6)))

    add(("B2  USB CONNECTED", takeover(
        "B2_usb", title="USB DRIVE", sub="Read-only. Copy the CSV, then eject.",
        data="SNAPSHOT 14:31|2,184 RECORDS", **LIVE)))

    add(("B3  BLE SESSION", takeover(
        "B3_ble", title="MCOLD-0117", sub="Confirm this ID in the app to pair.",
        data="PAIRING WINDOW 60 S", wifi=True, cloud=True, gnss=True,
        battery=78, storage=6)))

    add(("B4  STORAGE FULL", takeover(
        "B4_storage", title="STORAGE FULL", sub="Oldest finished trip was dropped.",
        data="TRIP 0139 LOST|UPLOAD NOW", alarm=True, title_color=RED,
        trip=True, wifi=True, cloud=False, gnss=True, battery=54, storage=100)))

    add(("B5  CRITICAL BATTERY", takeover(
        "B5_battery", title="BATTERY 4%", sub="Logging stopped. Charge the device.",
        data="LAST RECORD 14:32", alarm=True, title_color=RED,
        wifi=False, cloud=False, battery=4, storage=6)))

    add(("B6  FAULT / RECOVERY", takeover(
        "B6_fault", title="SENSOR FAULT", sub="Temperature bus not responding.",
        data="TRIP CONTINUES · SEE APP", title_color=RED, **LIVE)))

    add(("C1  TRIP SUMMARY", detail(
        "C1_summary", title="TRIP 0142", state="CLOSED", rows=(
            ("Duration", "3d 06h 12m"),
            ("Temperature", "2.8 / 6.1 ~C"),
            ("Alarms", "1 high|2 shock")),
        wifi=True, cloud=True, gnss=True, battery=74, storage=9)))

    add(("C2  DETAIL (NFC TAP)", detail(
        "C2_detail", title="DEVICE", state="DETAIL", rows=(
            ("Trip", "0142|21 Sep 08:20"),
            ("Upload", "184 records pending"),
            ("Firmware", "0.1.0|RTC OK")),
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
