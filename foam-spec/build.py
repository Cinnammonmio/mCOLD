import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
OUT_PDF = HERE.parent / "Foam_Functions_and_Usage_Flow_2026-09-20.pdf"
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
PROFILE = pathlib.Path(os.environ.get("TEMP", HERE)) / "foam-spec-edge"


def icon_svg(name):
    raw = (HERE / "icons" / f"{name}.svg").read_text(encoding="utf-8")
    inner = re.search(r"<svg[^>]*>(.*)</svg>", raw, re.S).group(1)
    inner = re.sub(r'<path stroke="none" d="M0 0h24v24H0z" fill="none"\s*/>', "", inner).strip()
    return ('<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" '
            'stroke-linecap="round" stroke-linejoin="round">' + inner + "</svg>")


def main():
    html = (HERE / "template.html").read_text(encoding="utf-8")
    missing = set()

    def sub(m):
        name = m.group(1)
        if not (HERE / "icons" / f"{name}.svg").exists():
            missing.add(name)
            return ""
        return icon_svg(name)

    html = re.sub(r"\{\{i:([a-z0-9-]+)\}\}", sub, html)
    if missing:
        sys.exit(f"missing icons: {sorted(missing)}")

    built = HERE / "foam_spec.html"
    built.write_text(html, encoding="utf-8")

    cmd = [EDGE, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
           "--print-to-pdf-no-header", f"--user-data-dir={PROFILE}",
           "--virtual-time-budget=8000", f"--print-to-pdf={OUT_PDF}", built.as_uri()]
    subprocess.run(cmd, check=True, timeout=180, capture_output=True)
    print("wrote", OUT_PDF, OUT_PDF.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
