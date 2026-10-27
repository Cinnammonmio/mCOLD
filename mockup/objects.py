"""Transparent PNG cut-outs of every object, for placing on slides with their own theme.

usage: python mockup/objects.py [word]   (after prepare.py and npm install)
       word: only render objects whose name contains it, e.g. "dock"
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
    # the device in each state, LED lit as the firmware does it (docs/led-design.md)
    "state_A1_ready": "o=device_front&scr=A1&led=-,-,-",
    "state_A2_trip": "o=device_front&scr=A2&led=-,1,-",
    "state_A3_out_of_range": "o=device_front&scr=A3&led=-,1,-",
    "state_A4_alarm": "o=device_front&scr=A4&led=0,-,-",
    "state_C1_summary": "o=device_front&scr=C1&led=-,-,-",
    # the 6-slot dock, holding six charging TCOLDs
    "dock_with_6": "o=dock",
    "dock_front": "o=dock_front",
    "dock_back": "o=dock_back",
    "dock_top": "o=dock_top",
    "dock_empty": "o=dock_empty",
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
SPEC = {"spec_sheet": "o=sheet", "spec_dimensions": "o=dims", "spec_dock_dimensions": "o=dims&what=dock"}
SPEC.update({f"spec_card_{g}": f"o=card&g={g}" for g in ["temp", "body", "sense", "link", "data", "power", "dock"]})
SPEC.update({f"spec_kpi_{i + 1}_{n}": f"o=kpi&k={i}" for i, n in enumerate(["resolution", "interval", "storage", "runtime", "size", "display", "dock_slots", "dock_cable", "dock_adapter"])})

def trim(src, dst, pad=24):
    im = Image.open(src).convert("RGBA")
    a = im.getchannel("A").point(lambda v: 255 if v > 3 else 0)
    l, t, r, b = a.getbbox()
    im.crop((max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))).save(dst, optimize=True)

def shadow(im):
    """PIL version of the pages' drop-shadow(0 40px 40px .25) + (0 10px 12px .18), at 2x."""
    from PIL import ImageFilter
    pad = 260; W, H = im.width + 2 * pad, im.height + 2 * pad
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0)); a = im.getchannel("A")
    for dy, blur, op in ((80, 80, .25), (20, 24, .18)):
        m = Image.new("L", (W, H), 0); m.paste(a, (pad, pad + dy)); m = m.filter(ImageFilter.GaussianBlur(blur / 2))
        layer = Image.new("RGBA", (W, H), (11, 53, 80, 0)); layer.putalpha(m.point(lambda v: int(v * op)))
        out = Image.alpha_composite(out, layer)
    out.alpha_composite(im, (pad, pad))
    l, t, r, b = out.getchannel("A").point(lambda v: 255 if v > 3 else 0).getbbox()
    return out.crop((max(0, l - 24), max(0, t - 24), min(W, r + 24), min(H, b + 24)))

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
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    SCENES = {n: q for n, q in SCENES.items() if only in n}
    SPEC = {n: q for n, q in SPEC.items() if only in n}
    jobs = [[f"objects.html?{q}", str(TMP / f"{n}.png"), 1920, 1080, 2, 1] for n, q in SCENES.items()]
    # the sheet carries the dimension drawing and runs taller than a 1080 page
    jobs += [[f"spec.html?{q}&t={t}", str(TMP / f"{n}_{t}.png"), 1920, 1340 if n == "spec_sheet" else 1080, 2, 1]
             for n, q in SPEC.items() for t in ("light", "dark")]
    jobs += [j for j in logos() if only in Path(j[1]).stem.lower() or not only]
    subprocess.run(["node", str(HERE / "render.js"), json.dumps(jobs)], check=True)
    for n in SCENES:
        trim(TMP / f"{n}.png", OUT / f"{n}.png")
        print(n, Image.open(OUT / f"{n}.png").size)
    (OUT / "spec").mkdir(exist_ok=True)
    for n in SPEC:
        for t in ("light", "dark"):
            trim(TMP / f"{n}_{t}.png", OUT / "spec" / f"{n}_{t}.png", pad=48)   # room for the card shadow
    # the dimension drawing also at the top level, where it is easy to find
    for t in ("light", "dark"):
        for a, b in (("device_dimensions", "spec_dimensions"), ("dock_dimensions", "spec_dock_dimensions")):
            if (OUT / "spec" / f"{b}_{t}.png").exists():
                (OUT / f"{a}_{t}.png").write_bytes((OUT / "spec" / f"{b}_{t}.png").read_bytes())
    # the e-paper screen itself, at 4x (1000 x 488), with the same shadow as the rest
    scr = Image.open(HERE / "build" / "screen_tex.png").convert("RGBA").resize((1000, 488), Image.LANCZOS)
    shadow(scr).save(OUT / "epaper_screen_trip.png", optimize=True)
