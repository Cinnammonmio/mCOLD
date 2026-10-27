"""Transparent PNG cut-outs of every object, for placing on slides with their own theme.

usage: python mockup/objects.py        (after prepare.py and npm install)
       -> mockup/out/objects/*.png, trimmed to the object with a small margin
"""
import json, subprocess, sys
from pathlib import Path
from PIL import Image

HERE = Path(__file__).parent
OUT = HERE / "out" / "objects"
TMP = HERE / "build" / "objects"

# name -> objects.html query (3D) ; DPR 2 on a 1920x1080 page = 3840x2160 before trimming
SCENES = {
    "device_with_probe": "o=device",
    "device_front": "o=device_front",
    "device_back": "o=device_back",
    "explode_stack": "o=explode",
    "thermocouple": "o=thermocouple",
    "phone_app": "o=phone",
    "phone_app_tilted": "o=phone&tilt=1",
    "part_1_front_case": "o=front_case",
    "part_2_light_pipes": "o=light_pipes",
    "part_3_epaper": "o=epaper",
    "part_4_nfc_board": "o=nfc_board",
    "part_5_plate": "o=plate",
    "part_6_main_board": "o=main_board",
    "part_6b_main_board_components": "o=main_board_back",
    "part_7_battery": "o=battery",
    "part_8_back_case": "o=back_case",
    "part_8b_back_case_outside": "o=back_case_outside",
}

# spec objects from spec.html, light and dark
SPEC = {"spec_sheet": "o=sheet", "spec_dimensions": "o=dims"}
SPEC.update({f"spec_card_{g}": f"o=card&g={g}" for g in ["temp", "body", "sense", "link", "data", "power"]})
SPEC.update({f"spec_kpi_{i + 1}_{n}": f"o=kpi&k={i}" for i, n in enumerate(["resolution", "interval", "storage", "runtime", "size", "display"])})

def trim(src, dst, pad=24):
    im = Image.open(src).convert("RGBA")
    a = im.getchannel("A").point(lambda v: 255 if v > 3 else 0)
    l, t, r, b = a.getbbox()
    im.crop((max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))).save(dst, optimize=True)

def logos():
    """Brand SVGs to PNG at 2400 px wide (transparent)."""
    import re
    for svg in sorted((HERE / "brand").glob("*.svg")):
        w, h = map(float, re.search(r'width="([\d.]+)" height="([\d.]+)"', svg.read_text()).groups())
        page = TMP / f"{svg.stem}.html"
        page.write_text(f'<html><body style="margin:0;background:transparent"><img src="../../brand/{svg.name}" '
                        f'style="width:1200px;display:block"><script>onload=()=>window.done=1</script></body></html>')
        yield [f"build/objects/{page.name}", str(OUT / f"{svg.stem}.png"), 1200, int(1200 * h / w), 2, 1]

if __name__ == "__main__":
    TMP.mkdir(parents=True, exist_ok=True); OUT.mkdir(parents=True, exist_ok=True)
    jobs = [[f"objects.html?{q}", str(TMP / f"{n}.png"), 1920, 1080, 2, 1] for n, q in SCENES.items()]
    jobs += [[f"spec.html?{q}&t={t}", str(TMP / f"{n}_{t}.png"), 1920, 1080, 2, 1] for n, q in SPEC.items() for t in ("light", "dark")]
    jobs += list(logos())
    subprocess.run(["node", str(HERE / "render.js"), json.dumps(jobs)], check=True)
    for n in SCENES:
        trim(TMP / f"{n}.png", OUT / f"{n}.png")
        print(n, Image.open(OUT / f"{n}.png").size)
    (OUT / "spec").mkdir(exist_ok=True)
    for n in SPEC:
        for t in ("light", "dark"):
            trim(TMP / f"{n}_{t}.png", OUT / "spec" / f"{n}_{t}.png", pad=48)   # room for the card shadow
    # the e-paper screen itself, at 4x (1000 x 488)
    Image.open(HERE / "build" / "screen_tex.png").resize((1000, 488), Image.LANCZOS).save(OUT / "epaper_screen_trip.png")
