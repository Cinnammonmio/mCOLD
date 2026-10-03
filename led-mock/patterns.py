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


# (event, light, pattern, colour, on battery, on USB)
STATES = [
    ("เปิดเครื่อง", "ทั้ง 3", "SWEEP ขาว แล้ว BLINK เขียว", WHITE, "ครั้งเดียวตอนบูต", "ครั้งเดียวตอนบูต"),
    ("เปิดเครื่อง มีปัญหา", "ขวา", "TRIPLE เหลือง (แทน BLINK)", AMBER, "ครั้งเดียวตอนบูต", "ครั้งเดียวตอนบูต"),
    ("แตะ NFC", "ทั้ง 3", "BLINK ขาว", WHITE, "ทันที", "ทันที"),
    ("เริ่ม trip", "กลาง", "TRIPLE เขียว", GREEN, "ทันที", "ทันที"),
    ("trip ทำงาน", "กลาง", "TICK เขียว", GREEN, "ทุก 15 นาที", "ทุก 1 วินาที"),
    ("หยุด trip", "กลาง", "BLINK เขียว", GREEN, "ทันที", "ทันที"),
    ("alarm อุณหภูมิ", "ซ้าย", "DOUBLE แดง + บี๊บ 3", RED, "ทันที แล้วทุกรอบตื่น (5 นาที)", "ทุก 1 วินาที"),
    ("รับทราบ alarm", "ซ้าย", "BLINK น้ำเงิน", BLUE, "ทันที", "ทันที"),
    ("เครื่องมีปัญหา", "ขวา", "TRIPLE เหลือง", AMBER, "ทันที แล้วทุก 15 นาที", "ทุก 1 วินาที"),
    ("มีคนขยับกล่อง", "ตามสถานะ", "สถานะทั้งหมด 1 เฟรม", MUTED, "หน้าต่าง 2 วินาที, ≤ 4 ครั้ง/ชม.", "หน้าต่าง 30 วินาที"),
    ("เสียบชาร์จ", "ข้าง", "BREATHE / STEADY / BLINK", AMBER, "—", "ตลอดที่เสียบ"),
]


def states():
    cols = [28, 210, 300, 530, 760]   # event, light, pattern, battery, usb
    row_h, top = 30, 104
    img = Image.new("RGB", (1000, top + row_h * len(STATES) + 40), PAPER)
    d = ImageDraw.Draw(img)
    d.text((28, 26), "When each light shows", font=font("SemiBold", 19), fill=INK)
    d.text((28, 50), "บนแบต เครื่องหลับระหว่างรอบ ไฟจึงติดเฉพาะตอนตื่น · เสียบ USB เครื่องไม่หลับ จังหวะถี่ขึ้น",
           font=font("Light", 12), fill=MUTED)
    for x, h in zip(cols, ("เหตุการณ์", "ไฟ", "รูปแบบ", "บนแบต", "เสียบ USB")):
        d.text((x, 80), h, font=font("SemiBold", 11), fill=MUTED)
    d.line([(28, 98), (972, 98)], fill=GRID, width=2)
    for i, (ev, light, pat, color, batt, usb) in enumerate(STATES):
        y = top + i * row_h
        d.text((cols[0], y + 4), ev, font=font("Regular", 12), fill=INK)
        d.ellipse([(cols[1], y + 6), (cols[1] + 12, y + 18)], fill=color,
                  outline=MUTED if color is WHITE else None)
        d.text((cols[1] + 20, y + 4), light, font=font("Light", 12), fill=INK)
        d.text((cols[2], y + 4), pat, font=font("Regular", 12), fill=INK)
        d.text((cols[3], y + 4), batt, font=font("Light", 12), fill=INK)
        d.text((cols[4], y + 4), usb, font=font("Light", 12), fill=INK)
        d.line([(28, y + row_h - 3), (972, y + row_h - 3)], fill=GRID)
    img.save(OUT / "led_states.png")
    return img


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    vocabulary()
    led_map()
    states()
    print(f"3 diagrams -> {OUT}")
