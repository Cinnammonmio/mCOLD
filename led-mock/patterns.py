#!/usr/bin/env python3
"""mCOLD LED pattern diagrams.

Draws the pattern vocabulary as timelines, the four pixels as they sit on
the enclosure, and when each light shows. These are reference drawings for
people, not panel output, so they are rendered normally (antialiased, full
colour) unlike the e-paper mockups in ../display-mock.

Every timing and colour here is the firmware's, as built (updated
2026-10-03): front patterns in firmware/src/indicate.cpp (PATS, sweep),
the side light in firmware/src/chargeled.cpp, pixel order in
firmware/src/leds.h. Change those first, then this.
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONTS = Path(__file__).parent.parent / "mcold-spec" / "fonts"
OUT = Path(__file__).parent / "out"

INK = (32, 34, 38)
MUTED = (122, 128, 136)
PAPER = (250, 250, 249)
GRID = (222, 224, 228)

# LED colours as the firmware sends them, drawn at full strength so they
# can be told apart; the front pixels really run at 2 % and the side at 20 %.
RED = (198, 44, 38)
GREEN = (30, 158, 74)
AMBER = (224, 152, 16)
BLUE = (42, 108, 186)
WHITE = (244, 244, 240)


def font(weight, size):
    return ImageFont.truetype(str(FONTS / f"IBMPlexSansThai-{weight}.ttf"), size)


# (label, note, colour, sequence): (ms, on) pairs, "on" may be "breathe".
# Front patterns: indicate.cpp PATS. Side: chargeled.cpp render().
PATTERNS = [
    ("TICK", "front · trip running (alive)", GREEN, [(40, True)]),
    ("BLINK", "front · one acknowledgement", BLUE, [(120, True)]),
    ("DOUBLE", "front · cargo alarm", RED, [(80, True), (120, False), (80, True)]),
    ("TRIPLE", "front · device problem; trip started (green)", AMBER,
     [(80, True), (70, False), (80, True), (70, False), (80, True)]),
    ("SWEEP", "front · boot: left → middle → right, 80 ms each", WHITE,
     [(80, True), (0, False), (80, True), (0, False), (80, True)]),
    ("BREATHE", "side · charging (amber) / ≥ 80 % (green), 12–100 %", AMBER,
     [(2000, "breathe")]),
    ("STEADY", "side · charge full", GREEN, [(2000, True)]),
    ("SLOW BLINK", "side · power in, not charging: 120 ms every 2 s", AMBER,
     [(120, True), (1880, False)]),
    ("FAULT BLINK", "side · charge fault: 120 ms every 1 s", RED,
     [(120, True), (880, False), (120, True), (880, False)]),
]

SPAN = 2400  # ms drawn on every timeline


def vocabulary():
    row_h, top, left, width = 46, 74, 300, 520
    h = top + row_h * len(PATTERNS) + 34
    img = Image.new("RGB", (left + width + 100, h), PAPER)
    d = ImageDraw.Draw(img)

    d.text((28, 26), "LED pattern vocabulary", font=font("SemiBold", 19), fill=INK)
    d.text((28, 50), "ทุก pattern จบในตัวเอง ไม่มีสถานะติดค้าง · rail เปิดเฉพาะช่วงที่วาดไว้ · ตาม firmware 0.7",
           font=font("Light", 12), fill=MUTED)

    for ms in range(0, SPAN + 1, 500):
        x = left + width * ms / SPAN
        d.line([(x, top - 8), (x, top + row_h * len(PATTERNS) - 10)], fill=GRID)
        d.text((x, top + row_h * len(PATTERNS) - 4), f"{ms}", font=font("Light", 10),
               fill=MUTED, anchor="ma")
    d.text((left + width / 2, h - 16), "milliseconds", font=font("Light", 10),
           fill=MUTED, anchor="ma")

    for i, (name, note, color, seq) in enumerate(PATTERNS):
        y = top + i * row_h
        d.text((28, y + 2), name, font=font("SemiBold", 13), fill=INK)
        d.text((28, y + 19), note, font=font("Light", 11), fill=MUTED)
        d.line([(left, y + 26), (left + width, y + 26)], fill=GRID)

        t = 0
        for ms, on in seq:
            x0 = left + width * t / SPAN
            x1 = left + width * min(t + ms, SPAN) / SPAN
            if on == "breathe":
                # chargeled.cpp: 0.12 + 0.88 * (1 - cos) / 2 over 2 s.
                steps = 64
                for s in range(steps):
                    level = 0.12 + 0.88 * 0.5 * (1 - math.cos(2 * math.pi * s / steps))
                    bx0 = x0 + (x1 - x0) * s / steps
                    bx1 = x0 + (x1 - x0) * (s + 1) / steps + 1
                    bar = tuple(round(p + (PAPER[j] - p) * (1 - level))
                                for j, p in enumerate(color))
                    d.rectangle([(bx0, y + 8), (bx1, y + 25)], fill=bar)
            elif on:
                d.rectangle([(x0, y + 8), (max(x1, x0 + 2), y + 25)], fill=color,
                            outline=MUTED if color is WHITE else None)
            t += ms
        if name == "SWEEP":
            for k, lbl in enumerate(("L", "M", "R")):
                x = left + width * (k * 80 + 40) / SPAN
                d.text((x, y + 17), lbl, font=font("SemiBold", 9), fill=INK, anchor="mm")
        total = sum(ms for ms, _ in seq)
        d.text((left + width + 12, y + 16), f"{total} ms", font=font("Light", 11),
               fill=MUTED, anchor="lm")

    img.save(OUT / "pattern_vocabulary.png")
    return img


def led_map():
    img = Image.new("RGB", (760, 318), PAPER)
    d = ImageDraw.Draw(img)
    d.text((28, 26), "LED placement and ownership", font=font("SemiBold", 19), fill=INK)
    d.text((28, 50), "ตำแหน่งจริงบนบอร์ด (ตรวจด้วย ledtest) · chain index ตามลำดับสายข้อมูล: LED4 เป็นพิกเซลแรก",
           font=font("Light", 12), fill=MUTED)

    d.rounded_rectangle([(40, 92), (430, 290)], 14, outline=(200, 202, 206), width=2)
    d.text((52, 104), "FRONT · 2 %", font=font("SemiBold", 11), fill=MUTED)
    front = [("LED3", "index 3", RED, "CARGO", "สินค้า: alarm อุณหภูมิ"),
             ("LED2", "index 2", GREEN, "ALIVE", "trip ทำงาน"),
             ("LED1", "index 1", AMBER, "DEVICE", "เครื่องมีปัญหา")]
    for i, (name, idx, color, role, note) in enumerate(front):
        cx = 108 + i * 124
        d.ellipse([(cx - 17, 140), (cx + 17, 174)], fill=color,
                  outline=(255, 255, 255), width=2)
        d.text((cx, 190), name, font=font("SemiBold", 12), fill=INK, anchor="ma")
        d.text((cx, 206), idx, font=font("Light", 10), fill=MUTED, anchor="ma")
        d.text((cx, 224), role, font=font("SemiBold", 10), fill=color, anchor="ma")
        d.text((cx, 240), note, font=font("Light", 10), fill=MUTED, anchor="ma")
    d.text((232, 266), "ซ้าย · กลาง · ขวา  (มองจากหน้าเครื่อง)", font=font("Light", 10),
           fill=MUTED, anchor="ma")

    d.rounded_rectangle([(470, 92), (718, 290)], 14, outline=(200, 202, 206), width=2)
    d.text((482, 104), "SIDE · 20 %", font=font("SemiBold", 11), fill=MUTED)
    d.ellipse([(577, 140), (611, 174)], fill=AMBER, outline=(255, 255, 255), width=2)
    d.text((594, 190), "LED4", font=font("SemiBold", 12), fill=INK, anchor="ma")
    d.text((594, 206), "index 0 · XL-4020RGBC", font=font("Light", 10), fill=MUTED, anchor="ma")
    d.text((594, 224), "CHARGE", font=font("SemiBold", 10), fill=AMBER, anchor="ma")
    d.text((594, 240), "สว่างเฉพาะตอนเสียบสาย", font=font("Light", 10), fill=MUTED, anchor="ma")

    img.save(OUT / "led_map.png")
    return img


# One row per event. `lit` is which lights show, by position: L, M, R on
# the front, S the side light (on the top edge, towards the right).
# `bars` is the timeline as (start ms, length ms, colour, mark); `beeps`
# the buzzer, as (start ms, length ms). From indicate.cpp: boot_cue is the
# sweep, 150 ms, then a row blink; an alarm is DOUBLE then alarm_sound,
# three 150 ms beeps 300 ms apart.
DOUBLE = [(0, 80), (200, 80)]
TRIPLE = [(0, 80), (150, 80), (300, 80)]
SWEEP = [(0, 80, WHITE, "L"), (80, 80, WHITE, "M"), (160, 80, WHITE, "R")]


def bars(spans, colour):
    return [(t, n, colour, "") for t, n in spans]


STATES = [
    ("เปิดเครื่อง", "ครั้งเดียวตอนบูต", {"L": WHITE, "M": WHITE, "R": WHITE},
     SWEEP + [(390, 120, GREEN, "")], []),
    ("เปิดเครื่อง เครื่องมีปัญหา", "ไฟขวาแทนการกะพริบเขียว", {"L": WHITE, "M": WHITE, "R": AMBER},
     SWEEP + bars([(390 + t, n) for t, n in TRIPLE], AMBER), []),
    ("แตะ NFC", "ทันที", {"L": WHITE, "M": WHITE, "R": WHITE},
     [(0, 120, WHITE, "")], []),
    ("เริ่ม trip", "ทันที", {"M": GREEN}, bars(TRIPLE, GREEN), []),
    ("trip ทำงาน", "บนแบต ทุก 15 นาที · เสียบ USB ทุก 1 วินาที", {"M": GREEN},
     [(0, 40, GREEN, "")], []),
    ("หยุด trip", "ทันที", {"M": GREEN}, [(0, 120, GREEN, "")], []),
    ("alarm อุณหภูมิ", "ทันที แล้วทุกรอบตื่น (5 นาที) · USB ทุก 1 วินาที", {"L": RED},
     bars(DOUBLE, RED), [(280, 150), (580, 150), (880, 150)]),
    ("รับทราบ alarm", "ทันที · เสียงหยุด", {"L": BLUE}, [(0, 120, BLUE, "")], []),
    ("เครื่องมีปัญหา", "ทันที แล้วบนแบตทุก 15 นาที · USB ทุก 1 วินาที", {"R": AMBER},
     bars(TRIPLE, AMBER), []),
    ("มีคนขยับกล่อง", "แสดงสถานะ 1 ครั้ง · บนแบต ≤ 4 ครั้ง/ชม.", {"M": GREEN},
     [(0, 40, GREEN, "")], []),
    ("กำลังชาร์จ", "ตลอดที่เสียบสาย", {"S": AMBER}, [(0, 2000, AMBER, "breathe")], []),
    ("ชาร์จเกือบเต็ม (≥ 80%)", "ตลอดที่เสียบสาย", {"S": GREEN}, [(0, 2000, GREEN, "breathe")], []),
    ("ชาร์จเต็ม", "ตลอดที่เสียบสาย", {"S": GREEN}, [(0, 2000, GREEN, "")], []),
    ("เสียบสายแต่ไม่ชาร์จ", "ทุก 2 วินาที", {"S": AMBER}, [(0, 120, AMBER, "")], []),
    ("ชาร์จผิดปกติ", "ทุก 1 วินาที · บี๊บ 1 ครั้งตอนเริ่ม", {"S": RED},
     [(0, 120, RED, ""), (1000, 120, RED, "")], [(0, 150)]),
]

UNLIT = (214, 216, 220)
CASE = (176, 180, 186)


def device(d, x, y, lit):
    """The box as seen from the front: chamfered square, the screen, the
    three lights under it; the side light on the top edge, to the right."""
    w, c = 64, 9
    d.polygon([(x + c, y), (x + w - c, y), (x + w, y + c), (x + w, y + w - c),
               (x + w - c, y + w), (x + c, y + w), (x, y + w - c), (x, y + c)],
              fill=(255, 255, 255), outline=CASE)
    d.rectangle([(x + 13, y + 16), (x + w - 13, y + 36)], fill=(236, 237, 236), outline=CASE)
    d.rectangle([(x + 13, y + 16), (x + w - 13, y + 19)], fill=CASE)
    for k, pos in enumerate(("L", "M", "R")):
        cx, cy = x + 22 + k * 10, y + 47
        col = lit.get(pos)
        d.ellipse([(cx - 3.5, cy - 3.5), (cx + 3.5, cy + 3.5)], fill=col or UNLIT,
                  outline=MUTED if col is WHITE else None)
    sx, sy = x + w - 16, y          # the side light, on the top edge
    col = lit.get("S")
    d.rectangle([(sx - 5, sy - 3), (sx + 5, sy + 2)], fill=col or UNLIT,
                outline=CASE if not col else None)


def states():
    SPAN_S = 2000
    ev_x, dev_x, tl_x, tl_w = 28, 330, 440, 520
    row_h, top = 82, 106
    h = top + row_h * len(STATES) + 40
    img = Image.new("RGB", (tl_x + tl_w + 40, h), PAPER)
    d = ImageDraw.Draw(img)
    d.text((28, 26), "When each light shows", font=font("SemiBold", 19), fill=INK)
    d.text((28, 50), "บนแบต เครื่องหลับระหว่างรอบ ไฟจึงติดเฉพาะตอนตื่น · เสียบ USB เครื่องไม่หลับ จังหวะถี่ขึ้น · ตาม firmware 0.7",
           font=font("Light", 12), fill=MUTED)
    for x, t in ((ev_x, "เหตุการณ์"), (dev_x - 6, "ตำแหน่ง"), (tl_x, "รูปแบบ")):
        d.text((x, 80), t, font=font("SemiBold", 11), fill=MUTED)
    d.line([(28, 100), (tl_x + tl_w, 100)], fill=GRID, width=2)

    for ms in range(0, SPAN_S + 1, 500):
        x = tl_x + tl_w * ms / SPAN_S
        d.line([(x, top), (x, top + row_h * len(STATES) - 6)], fill=GRID)
        d.text((x, top + row_h * len(STATES)), f"{ms}", font=font("Light", 10),
               fill=MUTED, anchor="ma")
    d.text((tl_x + tl_w / 2, h - 18), "milliseconds", font=font("Light", 10),
           fill=MUTED, anchor="ma")

    for i, (ev, when, lit, seq, beeps) in enumerate(STATES):
        y = top + i * row_h
        d.text((ev_x, y + 20), ev, font=font("SemiBold", 13), fill=INK)
        d.text((ev_x, y + 40), when, font=font("Light", 11), fill=MUTED)
        device(d, dev_x, y + 8, lit)
        by = y + 26                                  # the bar's top
        for t0, n, col, mark in seq:
            x0 = tl_x + tl_w * t0 / SPAN_S
            x1 = tl_x + tl_w * min(t0 + n, SPAN_S) / SPAN_S
            if mark == "breathe":
                steps = 64
                for s_ in range(steps):
                    level = 0.12 + 0.88 * 0.5 * (1 - math.cos(2 * math.pi * s_ / steps))
                    bx0 = x0 + (x1 - x0) * s_ / steps
                    bx1 = x0 + (x1 - x0) * (s_ + 1) / steps + 1
                    bar = tuple(round(p + (PAPER[j] - p) * (1 - level)) for j, p in enumerate(col))
                    d.rectangle([(bx0, by), (bx1, by + 18)], fill=bar)
            else:
                d.rectangle([(x0, by), (max(x1, x0 + 2), by + 18)], fill=col,
                            outline=MUTED if col is WHITE else None)
                if mark:
                    d.text(((x0 + x1) / 2, by + 9), mark, font=font("SemiBold", 9),
                           fill=INK, anchor="mm")
        for t0, n in beeps:                          # the buzzer, under the bar
            x0 = tl_x + tl_w * t0 / SPAN_S
            x1 = tl_x + tl_w * (t0 + n) / SPAN_S
            d.rectangle([(x0, by + 24), (x1, by + 29)], fill=INK)
        if beeps:
            x_end = tl_x + tl_w * (beeps[-1][0] + beeps[-1][1]) / SPAN_S
            d.text((x_end + 6, by + 27), "บี๊บ", font=font("Light", 10), fill=MUTED, anchor="lm")
        d.line([(28, y + row_h - 4), (tl_x + tl_w, y + row_h - 4)], fill=GRID)

    img.save(OUT / "led_states.png")
    return img


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    vocabulary()
    led_map()
    states()
    print(f"3 diagrams -> {OUT}")
