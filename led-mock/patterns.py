#!/usr/bin/env python3
"""mCOLD LED pattern diagrams.

Draws the pattern vocabulary as timelines and the four pixels as they sit
on the enclosure. These are reference drawings for people, not panel
output, so they are rendered normally (antialiased, full colour) unlike
the e-paper mockups in ../display-mock.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONTS = Path(__file__).parent.parent / "mcold-spec" / "fonts"
OUT = Path(__file__).parent / "out"

INK = (32, 34, 38)
MUTED = (122, 128, 136)
PAPER = (250, 250, 249)
GRID = (222, 224, 228)

# LED colours as named in the spec, at the capped brightness they run at.
RED = (198, 44, 38)
GREEN = (30, 158, 74)
AMBER = (224, 152, 16)
BLUE = (42, 108, 186)
WHITE = (244, 244, 240)


def font(weight, size):
    return ImageFont.truetype(str(FONTS / f"IBMPlexSansThai-{weight}.ttf"), size)


# (label, note, colour, sequence) where the sequence is (ms, on) pairs.
# "on" may be "ramp" for the breathing pattern.
PATTERNS = [
    ("TICK", "heartbeat, one per wake cycle", GREEN, [(40, True)]),
    ("BLINK", "single acknowledgement", BLUE, [(120, True)]),
    ("DOUBLE", "alarm, cargo temperature out of band", RED,
     [(80, True), (120, False), (80, True)]),
    ("TRIPLE", "device fault, not the cargo", AMBER,
     [(60, True), (100, False), (60, True), (100, False), (60, True)]),
    ("FAST", "attention window only, 5 Hz for 1 s", RED,
     [(60, True), (140, False)] * 5),
    ("BREATHE", "charging; external power only", AMBER, [(2000, "ramp")]),
    ("SWEEP", "boot self-test and locate; LED1 to LED3 in turn", WHITE,
     [(80, True), (0, False), (80, True), (0, False), (80, True)]),
]

SPAN = 2400  # ms drawn on every timeline


def vocabulary():
    row_h, top, left, width = 46, 74, 232, 560
    h = top + row_h * len(PATTERNS) + 34
    img = Image.new("RGB", (left + width + 130, h), PAPER)
    d = ImageDraw.Draw(img)

    d.text((28, 26), "LED pattern vocabulary", font=font("SemiBold", 19), fill=INK)
    d.text((28, 50), "ทุก pattern จบในตัวเอง ไม่มีสถานะติดค้าง · rail เปิดเฉพาะช่วงที่วาดไว้",
           font=font("Light", 12), fill=MUTED)

    # Time grid every 500 ms
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
            x1 = left + width * (t + ms) / SPAN
            if on == "ramp":
                # Breathing: brightness ramps up then back down.
                steps = 48
                for s in range(steps):
                    a = s / (steps - 1)
                    level = 1 - abs(2 * a - 1)
                    bx0 = x0 + (x1 - x0) * s / steps
                    bx1 = x0 + (x1 - x0) * (s + 1) / steps + 1
                    bar = tuple(round(p + (PAPER[j] - p) * (1 - level))
                                for j, p in enumerate(color))
                    d.rectangle([(bx0, y + 8), (bx1, y + 25)], fill=bar)
            elif on:
                box = [(x0, y + 8), (max(x1, x0 + 2), y + 25)]
                d.rectangle(box, fill=color,
                            outline=MUTED if color is WHITE else None)
            t += ms
        if name == "SWEEP":
            for k in range(3):
                x = left + width * (k * 80 + 40) / SPAN
                d.text((x, y + 12), str(k + 1), font=font("SemiBold", 9),
                       fill=INK, anchor="mm")
        total = sum(ms for ms, _ in seq)
        d.text((left + width + 12, y + 16), f"{total} ms", font=font("Light", 11),
               fill=MUTED, anchor="lm")

    img.save(OUT / "pattern_vocabulary.png")
    return img


def led_map():
    img = Image.new("RGB", (760, 300), PAPER)
    d = ImageDraw.Draw(img)
    d.text((28, 26), "LED placement and ownership", font=font("SemiBold", 19), fill=INK)
    d.text((28, 50), "chain index ตามลำดับสายข้อมูล: LED4 เป็นพิกเซลแรก",
           font=font("Light", 12), fill=MUTED)

    # Front face: the row of three status pixels.
    d.rounded_rectangle([(40, 92), (430, 262)], 14, outline=(200, 202, 206), width=2)
    d.text((52, 104), "FRONT", font=font("SemiBold", 11), fill=MUTED)
    front = [("LED1", "index 1", RED, "CARGO", "อุณหภูมิเกินช่วง"),
             ("LED2", "index 2", GREEN, "ALIVE", "trip ทำงานปกติ"),
             ("LED3", "index 3", AMBER, "DEVICE", "เครื่องมีปัญหา")]
    for i, (name, idx, color, role, note) in enumerate(front):
        cx = 108 + i * 108
        d.ellipse([(cx - 17, 140), (cx + 17, 174)], fill=color,
                  outline=(255, 255, 255), width=2)
        d.text((cx, 190), name, font=font("SemiBold", 12), fill=INK, anchor="ma")
        d.text((cx, 206), idx, font=font("Light", 10), fill=MUTED, anchor="ma")
        d.text((cx, 224), role, font=font("SemiBold", 10), fill=color, anchor="ma")
        d.text((cx, 240), note, font=font("Light", 10), fill=MUTED, anchor="ma")

    # Side face: the charge pixel.
    d.rounded_rectangle([(470, 92), (718, 262)], 14, outline=(200, 202, 206), width=2)
    d.text((482, 104), "SIDE", font=font("SemiBold", 11), fill=MUTED)
    d.ellipse([(577, 140), (611, 174)], fill=AMBER, outline=(255, 255, 255), width=2)
    d.text((594, 190), "LED4", font=font("SemiBold", 12), fill=INK, anchor="ma")
    d.text((594, 206), "index 0 · XL-4020RGBC", font=font("Light", 10), fill=MUTED, anchor="ma")
    d.text((594, 224), "CHARGE", font=font("SemiBold", 10), fill=AMBER, anchor="ma")
    d.text((594, 240), "สว่างเฉพาะตอนมีไฟนอก", font=font("Light", 10), fill=MUTED, anchor="ma")

    img.save(OUT / "led_map.png")
    return img


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    vocabulary()
    led_map()
    print(f"2 diagrams -> {OUT}")
