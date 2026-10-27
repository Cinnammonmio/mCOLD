"""TCOLD wordmark as SVG.

The T is the THANES T, rebuilt as geometry from a photo of the THANES logo: a
T whose left arm and stem top are cut by one 45-degree line (x = y + 89 in the
units below), the cut rounded where it meets the top edge and the stem. The
right half of the bar is not cut, so the three pieces meet at one point.
Units: bar height 100, cap height 385, measured off the photo (bar 53 px).

COLD is Unbounded Bold (OFL), converted to outlines so the SVG needs no font.

usage: python mockup/brand/logo.py        -> mockup/brand/*.svg
"""
import math
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = Path(__file__).parent
WEIGHT = 700                           # stroke weight closest to the T's 0.26-0.31 of cap height
PLEX = HERE.parent.parent / "mcold-spec/fonts"
font_file = lambda w: HERE.parent / f"node_modules/@fontsource/unbounded/files/unbounded-latin-{w}-normal.woff2"

CAP = 385.0
def t_path():
    k = math.tan(math.radians(22.5))
    # left arm: top edge -> rounded into the 45-degree cut -> down to the bar's bottom at x=189
    r1 = 53; t1 = r1 * k; c1 = (89, 0)
    a1 = (c1[0] - t1, 0); b1 = (c1[0] + t1 / math.sqrt(2), t1 / math.sqrt(2))
    arm = f"M0,0 L{a1[0]:.2f},0 A{r1},{r1} 0 0 1 {b1[0]:.2f},{b1[1]:.2f} L189,100 L0,100 Z"
    bar = "M189,0 L500,0 L500,100 L189,100 Z"
    # stem: point at (189,100), the cut runs down to the right edge and rounds into it
    r2 = 75; t2 = r2 * k; c2 = (308, 219)
    a2 = (c2[0] - t2 / math.sqrt(2), c2[1] - t2 / math.sqrt(2)); b2 = (308, c2[1] + t2)
    stem = f"M189,100 L{a2[0]:.2f},{a2[1]:.2f} A{r2},{r2} 0 0 1 {b2[0]:.2f},{b2[1]:.2f} L308,{CAP:.0f} L189,{CAP:.0f} Z"
    return " ".join([arm, bar, stem]), 500.0

def cold_paths(scale, x0, gap, weight):
    """Outlines of C O L D, y down, cap height = CAP; returns (paths, right edge)."""
    f = TTFont(font_file(weight)); gs = f.getGlyphSet(); cmap = f.getBestCmap()
    out, x = [], x0
    for ch in "COLD":
        g = cmap[ord(ch)]; adv, lsb = f["hmtx"][g]
        bounds = gs[g]
        pen = SVGPathPen(gs)
        # place so the glyph's ink starts at x: font x' = (fx - lsb) * scale + x ; y' = CAP - fy * scale
        tp = TransformPen(pen, (scale, 0, 0, -scale, x - lsb * scale, CAP))
        gs[g].draw(tp)
        out.append(pen.getCommands())
        from fontTools.pens.boundsPen import BoundsPen
        bp = BoundsPen(gs); gs[g].draw(bp); xmin, _, xmax, _ = bp.bounds
        x += (xmax - lsb) * scale + gap
    return out, x - gap

def build(t_fill, cold_fill, name, defs="", weight=WEIGHT, out=HERE):
    tp, tw = t_path()
    f = TTFont(font_file(weight))
    scale = CAP / f["OS/2"].sCapHeight
    gap = 0.105 * CAP                      # THANES: 21 px between letters at 204 px cap height
    paths, right = cold_paths(scale, tw + gap, gap, weight)
    w = math.ceil(right); pad = 12          # room for the overshoot of C and O
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 {-pad} {w} {CAP + 2 * pad:.0f}" width="{w}" height="{CAP + 2 * pad:.0f}">'
           f'{defs}<path fill="{t_fill}" d="{tp}"/>'
           + "".join(f'<path fill="{cold_fill}" d="{p}"/>' for p in paths) + "</svg>")
    (Path(out) / f"{name}.svg").write_text(svg)
    return w, CAP + 2 * pad

FULL_NAME = "Thanes Cold-chain Online Logging Device"

def text_paths(fontfile, text, height, x, baseline, tracking=0.0):
    """Outlines of text set at the given cap height; returns [(char, path)], right edge."""
    f = TTFont(fontfile); gs = f.getGlyphSet(); cmap = f.getBestCmap()
    sc = height / f["OS/2"].sCapHeight; out = []
    for ch in text:
        g = cmap[ord(ch)]; adv = f["hmtx"][g][0]
        pen = SVGPathPen(gs); gs[g].draw(TransformPen(pen, (sc, 0, 0, -sc, x, baseline)))
        out.append((ch, pen.getCommands())); x += adv * sc + tracking * height
    return out, x - tracking * height

def lockup(t_fill, cold_fill, initial_fill, rest_fill, name, defs="", out=HERE):
    """Wordmark with the full name under it; the name's initials spell TCOLD."""
    tp, tw = t_path()
    f = TTFont(font_file(WEIGHT)); scale = CAP / f["OS/2"].sCapHeight; gap = 0.105 * CAP
    paths, right = cold_paths(scale, tw + gap, gap, WEIGHT)
    w = math.ceil(right)
    # size the name so it runs the full width of the wordmark
    probe, pw = text_paths(PLEX / "IBMPlexSansThai-Regular.ttf", FULL_NAME, 100, 0, 0)
    th = 100 * w / pw; base = CAP + 75 + th
    words = FULL_NAME.split(" "); x = 0; parts = []
    for i, word in enumerate(words):
        p1, x1 = text_paths(PLEX / "IBMPlexSansThai-SemiBold.ttf", word[0], th, x, base)
        p2, x2 = text_paths(PLEX / "IBMPlexSansThai-Regular.ttf", word[1:] + (" " if i < len(words) - 1 else ""), th, x1, base)
        parts += [(initial_fill, p) for _, p in p1] + [(rest_fill, p) for _, p in p2]; x = x2
    k = w / x                                        # bold initials make the line a hair wider; fit it exactly
    h = math.ceil(base + th * 0.3); pad = 12
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 {-pad} {w} {h + 2 * pad}" width="{w}" height="{h + 2 * pad}">{defs}'
           f'<path fill="{t_fill}" d="{tp}"/>' + "".join(f'<path fill="{cold_fill}" d="{p}"/>' for p in paths)
           + f'<g transform="scale({k:.5f} 1)">' + "".join(f'<path fill="{c}" d="{p}"/>' for c, p in parts if p) + "</g></svg>")
    (Path(out) / f"{name}.svg").write_text(svg)
    return w, h

if __name__ == "__main__":
    grad = ('<defs><linearGradient id="tg" x1="0" y1="0" x2="1" y2="1">'
            '<stop offset="0" stop-color="#1aa5b5"/><stop offset="1" stop-color="#0e8391"/></linearGradient></defs>')
    print(build("url(#tg)", "#0b3550", "TCOLD_logo", grad))          # on light backgrounds
    print(build("#1aa5b5", "#ffffff", "TCOLD_logo_on_dark"))         # on dark backgrounds
    print(build("#ffffff", "#ffffff", "TCOLD_logo_white"))
    print(build("#0b3550", "#0b3550", "TCOLD_logo_navy"))
    print(lockup("url(#tg)", "#0b3550", "#0e8391", "#5f707c", "TCOLD_lockup", grad))
    print(lockup("#1aa5b5", "#ffffff", "#5ee0ec", "#cfe3ea", "TCOLD_lockup_on_dark"))
