"""TCOLD DOCK Station logos, built on the TCOLD wordmark from logo.py.

- horizontal: TCOLD | DOCK over STATION, for the dock's front, slide headers and labels
- stacked:    TCOLD over DOCK STATION, for square spaces
- symbol:     the TCOLD T over the dock seen from the front, six TCOLD standing in it

DOCK is Unbounded Bold like COLD; STATION is Unbounded Regular, tracked out to the width
of DOCK (horizontal) or set with DOCK to the width of the wordmark (stacked).
All text is converted to outlines, so the SVGs need no fonts.

usage: python mockup/brand/dock_logo.py      -> mockup/brand/TCOLD_DOCK_STATION_*.svg
"""
import math, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from fontTools.ttLib import TTFont
import logo
from logo import CAP, HERE, t_path, cold_paths, text_paths, font_file

GRAD = ('<linearGradient id="tg" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="#1aa5b5"/><stop offset="1" stop-color="#0e8391"/></linearGradient>')
LIGHT = dict(t="url(#tg)", cold="#0b3550", dock="#0e8391", station="#5f707c", rule="#c9d8de", defs=GRAD)
DARK = dict(t="#1aa5b5", cold="#ffffff", dock="#5ee0ec", station="#cfe3ea", rule="#4f7590", defs="")
UB = lambda w: font_file(w)


def wordmark():
    """TCOLD outlines at cap height CAP, ink from x 0; returns (svg paths with fills as {t}/{cold}), width."""
    tp, tw = t_path()
    scale = CAP / TTFont(UB(logo.WEIGHT))["OS/2"].sCapHeight; gap = 0.105 * CAP
    paths, right = cold_paths(scale, tw + gap, gap, logo.WEIGHT)
    return [("t", tp)] + [("cold", p) for p in paths], right


def width(fontfile, text, h, tracking=0.0):
    return text_paths(fontfile, text, h, 0, 0, tracking)[1]


def tracking_to(fontfile, text, h, target):
    """Tracking (fraction of h per letter gap) that makes text exactly target wide."""
    w0 = width(fontfile, text, h)
    return (target - w0) / (h * (len(text) - 1))


def svg(parts, w, h, c, name, pad=14, bg=None):
    body = "".join(f'<path fill="{c[k]}" d="{d}"/>' for k, d in parts if d)
    back = f'<rect x="{-pad}" y="{-pad}" width="{w + 2 * pad}" height="{h + 2 * pad}" fill="{bg}"/>' if bg else ""
    (HERE / f"{name}.svg").write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{-pad} {-pad} {w + 2 * pad:.0f} {h + 2 * pad:.0f}" '
        f'width="{w + 2 * pad:.0f}" height="{h + 2 * pad:.0f}"><defs>{c["defs"]}</defs>{back}{body}</svg>')
    return round(w + 2 * pad), round(h + 2 * pad)


def horizontal(c, name):
    parts, ww = wordmark()
    rx = ww + 0.34 * CAP; rw = 0.028 * CAP                       # thin rule between the two names
    parts.append(("rule", f"M{rx:.1f},0 h{rw:.1f} v{CAP:.0f} h{-rw:.1f} Z"))
    x0 = rx + rw + 0.34 * CAP
    hd, hs = 0.52 * CAP, 0.27 * CAP                             # DOCK cap height, STATION cap height
    dock, dw = text_paths(UB(700), "DOCK", hd, x0, hd, tracking=0.06)
    tr = tracking_to(UB(400), "STATION", hs, dw - x0)
    station, _ = text_paths(UB(400), "STATION", hs, x0, CAP, tracking=tr)
    parts += [("dock", p) for _, p in dock] + [("station", p) for _, p in station]
    return svg(parts, dw, CAP, c, name)


def stacked(c, name):
    parts, ww = wordmark()
    h = 0.30 * CAP; base = CAP + 0.38 * CAP + h
    # DOCK bold · STATION regular on one line, spread to the wordmark's width; a dot keeps the words apart
    sp = 1.6 * h; dot = 0.16 * h
    natural = width(UB(700), "DOCK", h) + sp + width(UB(400), "STATION", h)
    tr = (ww - natural) / (h * 9)                                # 9 letter gaps inside the two words
    dock, dx = text_paths(UB(700), "DOCK", h, 0, base, tracking=tr)
    station, _ = text_paths(UB(400), "STATION", h, dx + sp, base, tracking=tr)
    cx, cy = dx + sp / 2, base - h / 2
    parts += [("dock", p) for _, p in dock] + [("rule", f"M{cx - dot:.1f},{cy:.1f} a{dot:.1f},{dot:.1f} 0 1 0 {2 * dot:.1f},0 a{dot:.1f},{dot:.1f} 0 1 0 {-2 * dot:.1f},0 Z")]
    parts += [("station", p) for _, p in station]
    return svg(parts, ww, base, c, name)


def symbol(name, bg, t, devices, body, light):
    """Square mark: the T over the dock seen from the front, six TCOLD standing in it,
    each with its side light on (charging)."""
    S = 1000; tp, tw = t_path(); k = 0.84
    tx, ty = (S - tw * k) / 2, 120
    t = f'<path fill="{t}" transform="translate({tx:.1f} {ty}) scale({k})" d="{tp}"/>'
    n, dw, dh, g = 6, 74, 250, 18; span = n * dw + (n - 1) * g; dx0 = (S - span) / 2; dy = 490
    devs = "".join(f'<rect x="{dx0 + i * (dw + g):.1f}" y="{dy}" width="{dw}" height="{dh}" rx="18" fill="{devices}"/>'
                   f'<circle cx="{dx0 + i * (dw + g) + dw / 2:.1f}" cy="{dy + 34}" r="11" fill="{light}"/>' for i in range(n))
    bw, bh = span + 110, 180; bx, by = (S - bw) / 2, dy + 150
    dock = (f'<path fill="{body}" d="M{bx + 14:.1f},{by} H{bx + bw - 14:.1f} L{bx + bw:.1f},{by + bh - 44} '
            f'Q{bx + bw:.1f},{by + bh} {bx + bw - 44:.1f},{by + bh} H{bx + 44:.1f} Q{bx:.1f},{by + bh} {bx:.1f},{by + bh - 44} Z"/>')
    back = f'<rect width="{S}" height="{S}" rx="230" fill="{bg}"/>' if bg else ""
    (HERE / f"{name}.svg").write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {S} {S}" width="{S}" height="{S}">'
        f'<defs>{GRAD}</defs>{back}{devs}{dock}{t}</svg>')
    return S, S


if __name__ == "__main__":
    print(horizontal(LIGHT, "TCOLD_DOCK_STATION_logo"))
    print(horizontal(DARK, "TCOLD_DOCK_STATION_logo_on_dark"))
    print(stacked(LIGHT, "TCOLD_DOCK_STATION_stacked"))
    print(stacked(DARK, "TCOLD_DOCK_STATION_stacked_on_dark"))
    print(symbol("TCOLD_DOCK_STATION_symbol", "#0b3550", "#1aa5b5", "#ffffff", "#1aa5b5", "#3fd47a"))
    print(symbol("TCOLD_DOCK_STATION_symbol_on_light", None, "url(#tg)", "#c9d8de", "#0b3550", "#3fd47a"))
