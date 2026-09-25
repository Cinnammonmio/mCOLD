#!/usr/bin/env python3
"""mCOLD e-paper screen mockups.

Renders every display state of the 2.13" Waveshare (G) panel at its real
250x122 geometry using only the panel's four inks. Text is drawn into a
1-bit mask and thresholded, so the preview has no greys the panel cannot
show. The layout tables here are the same ones the firmware view model
will use; only the backend changes when the panel driver comes up.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

W, H = 250, 122

# Approximate ink colours of the 4-colour (G) panel, not screen primaries.
WHITE = (255, 255, 255)
BLACK = (26, 26, 26)
YELLOW = (238, 200, 32)
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


# Type scale: one micro caps size, one reading size, one value size, two display sizes.
MICRO = font("SemiBold", 11)    # tracked caps: labels, header, footer
SMALL = font("Medium", 13)      # secondary sentences and detail rows
STATUSES = [font("SemiBold", s) for s in (13, 12, 11)]  # shrinks to clear the column
TITLE = font("SemiBold", 26)    # takeover headline
HEROES = [font("Light", s) for s in (58, 52, 46)]  # shrinks to clear the column
HERO_UNIT = font("Light", 18)
VALUES = [font("Medium", s) for s in (15, 14, 13, 12)]  # widest that fits wins
TRACK = 0.8                     # caps tracking, applied on whole pixels

# Layout grid. Every y below is a baseline except the header/footer bands,
# so glyphs never cross the two hairlines.
M = 11              # side margin
RULE_TOP = 23
RULE_BOT = 99
BASE_HERO = 75      # temperature baseline
BASE_STATUS = 92    # status word baseline
COL_R = 147         # right column label x
COL_ROWS = (44, 66, 88)
EDGE_R = W - M


class Screen:
    """A 250x122 frame that only ever contains the four panel inks."""

    def __init__(self, name, accent=None):
        self.name = name
        self.img = Image.new("RGB", (W, H), WHITE)
        self.d = mono(self.img)
        if accent:
            self.bar(accent)

    def _stamp(self, mask, color):
        self.img.paste(color, (0, 0), mask)

    def bar(self, color):
        """Left edge accent: the only place colour carries state, and never alone."""
        m = Image.new("L", (W, H), 0)
        mono(m).rectangle([0, 0, 3, H - 1], fill=255)
        self._stamp(m, color)

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

    def rule(self, y, x0=M, x1=EDGE_R):
        m = Image.new("L", (W, H), 0)
        mono(m).rectangle([x0, y, x1 - 1, y], fill=255)
        self._stamp(m, BLACK)

    # ---- shared bands -------------------------------------------------
    def header(self, device, stamp, stale=False):
        self.text((M, 15), device, MICRO, track=TRACK)
        self.text((EDGE_R, 15), stamp if not stale else f"{stamp}  STALE", MICRO,
                  RED if stale else BLACK, anchor="rs", track=TRACK)
        self.rule(RULE_TOP)

    def footer(self, left, right=""):
        self.rule(RULE_BOT)
        self.text((M, 113), left, MICRO, track=TRACK)
        if right:
            self.text((EDGE_R, 113), right, MICRO, anchor="rs", track=TRACK)

    def stat_rows(self, stats):
        """Label left, value right on one baseline; the value takes the
        largest size that still clears the label."""
        for (label, value), y in zip(stats, COL_ROWS):
            self.text((COL_R, y), label, MICRO, track=TRACK)
            room = EDGE_R - COL_R - self.width(label, MICRO, TRACK) - 7
            fnt = next((f for f in VALUES if self.width(value, f) <= room), VALUES[-1])
            self.text((EDGE_R, y), value, fnt, anchor="rs")

    def save(self):
        self.img.save(OUT / f"{self.name}.png")
        return self.img


# ---------------------------------------------------------------------
# Template A -- Monitor: hero temperature left, three quiet stats right.
# ---------------------------------------------------------------------
def monitor(name, *, device="MCOLD-0117", stamp="UPDATED 14:32", stale=False,
            temp="4.2", status="IN RANGE", status_color=BLACK, hero_color=BLACK,
            accent=None, stats=(("MIN", "2.8"), ("MAX", "6.1"), ("DOOR", "CLOSED")),
            foot_l="TRIP 0142 / 3D 04H", foot_r="WI-FI  78%"):
    s = Screen(name, accent)
    s.header(device, stamp, stale)
    room = COL_R - M - 8 - s.width("°C", HERO_UNIT) - 4
    hero = next((f for f in HEROES if s.width(temp, f) <= room), HEROES[-1])
    s.text((M - 1, BASE_HERO), temp, hero, hero_color)
    s.text((M - 1 + s.width(temp, hero) + 4, BASE_HERO - 24), "°C", HERO_UNIT, hero_color)
    room = COL_R - M - 6
    fnt = next((f for f in STATUSES if s.width(status, f, 0.5) <= room), None)
    if fnt is None:
        fnt = STATUSES[-1]
        print(f"  warn {name}: status {status!r} overflows by "
              f"{round(s.width(status, fnt, 0.5) - room)} px")
    s.text((M, BASE_STATUS), status, fnt, status_color, track=0.5)
    s.stat_rows(stats)
    s.footer(foot_l, foot_r)
    return s.save()


# ---------------------------------------------------------------------
# Template B -- Takeover: one headline, one sentence, one data line.
# ---------------------------------------------------------------------
def takeover(name, *, device="MCOLD-0117", stamp="14:32", title="", sub="",
             data="", accent=None, title_color=BLACK):
    s = Screen(name, accent)
    s.header(device, stamp)
    s.text((M, 58), title, TITLE, title_color)
    s.text((M, 80), sub, SMALL)
    s.footer(data)
    return s.save()


# ---------------------------------------------------------------------
# Template C -- Detail: three label/value rows, no hero.
# ---------------------------------------------------------------------
def detail(name, *, device="MCOLD-0117", stamp="14:32", title="", rows=(), foot=""):
    s = Screen(name)
    s.header(device, stamp)
    s.text((M, 38), title, MICRO, track=TRACK)
    for i, (label, value) in enumerate(rows):
        y = 58 + i * 17
        s.text((M, y), label, SMALL)
        s.text((EDGE_R, y), value, SMALL, anchor="rs")
    s.footer(foot)
    return s.save()


SCREENS = []


def build():
    OUT.mkdir(exist_ok=True)
    add = SCREENS.append

    # -- A: monitor states -------------------------------------------
    add(("A1  IDLE / READY", monitor(
        "A1_idle", temp="4.2", status="READY",
        stats=(("BATT", "78%"), ("STORE", "94%"), ("SYNC", "13:50")),
        foot_l="NO ACTIVE TRIP", foot_r="WI-FI  78%")))

    add(("A2  TRIP ACTIVE", monitor("A2_trip", status="IN RANGE")))

    add(("A3  WARNING", monitor(
        "A3_warning", temp="8.4", status="ABOVE 8.0 · 4 MIN", accent=YELLOW,
        stats=(("MIN", "2.8"), ("MAX", "8.4"), ("DOOR", "OPEN")),
        foot_r="1 WARNING")))

    add(("A4  ALARM", monitor(
        "A4_alarm", temp="11.6", status="TEMP HIGH", status_color=RED, hero_color=RED,
        accent=RED, stats=(("MIN", "2.8"), ("MAX", "11.6"), ("DOOR", "21M")),
        foot_r="ALARM 1 OF 3")))

    add(("A5  PROBE FAULT", monitor(
        "A5_probe", temp="--", status="PROBE OPEN", accent=YELLOW,
        stats=(("LAST", "4.2"), ("SINCE", "14:12"), ("DOOR", "CLOSED")),
        foot_l="TRIP 0142 / LOGGING", foot_r="1 FAULT")))

    add(("A6  CHARGING / DOCK", monitor(
        "A6_charging", temp="4.4", status="CHARGING",
        stats=(("BATT", "62%"), ("CHG", "1.18A"), ("FULL", "42M")),
        foot_l="NO ACTIVE TRIP", foot_r="DOCK  PD 20V")))

    add(("A7  STALE SAMPLE", monitor(
        "A7_stale", temp="4.2", stamp="UPDATED 11:05", stale=True,
        status="NO SAMPLE 3H",
        stats=(("MIN", "2.8"), ("MAX", "6.1"), ("DOOR", "CLOSED")),
        foot_l="TRIP 0142 / GAP", foot_r="1 FAULT")))

    # -- B: takeovers -------------------------------------------------
    add(("B1  BOOT / SELF-TEST", takeover(
        "B1_boot", stamp="--:--", title="SELF-TEST", sub="Checking sensors and storage",
        data="FIRMWARE 0.1.0 · SERIAL 0117")))

    add(("B2  USB CONNECTED", takeover(
        "B2_usb", title="USB DRIVE", sub="Read-only. Copy the CSV, then eject.",
        data="SNAPSHOT 14:31 · 2,184 RECORDS")))

    add(("B3  BLE SESSION", takeover(
        "B3_ble", title="MCOLD-0117", sub="Confirm this ID in the app to pair.",
        data="PAIRING WINDOW 60 S")))

    add(("B4  STORAGE FULL", takeover(
        "B4_storage", title="STORAGE FULL", sub="Oldest finished trip was dropped.",
        data="TRIP 0139 LOST · UPLOAD NOW", accent=RED, title_color=RED)))

    add(("B5  CRITICAL BATTERY", takeover(
        "B5_battery", title="BATTERY 4%", sub="Logging stopped. Charge the device.",
        data="LAST RECORD 14:32", accent=RED, title_color=RED)))

    add(("B6  FAULT / RECOVERY", takeover(
        "B6_fault", title="SENSOR FAULT", sub="Temperature bus not responding.",
        data="TRIP CONTINUES · SEE APP", accent=YELLOW)))

    # -- C: detail and summary ---------------------------------------
    add(("C1  TRIP SUMMARY", detail(
        "C1_summary", title="TRIP 0142 CLOSED", rows=(
            ("Duration", "3d 06h 12m"),
            ("Temperature", "2.8 / 6.1 °C"),
            ("Door", "4 opens / 26 min")),
        foot="1 ALARM · UPLOAD PENDING")))

    add(("C2  DETAIL (NFC TAP)", detail(
        "C2_detail", title="DEVICE DETAIL", rows=(
            ("Trip", "0142 · 21 Sep 08:20"),
            ("Storage", "94% free · no SD"),
            ("Upload", "184 records pending")),
        foot="FW 0.1.0 · GNSS 12M · RTC OK")))

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
