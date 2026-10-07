"""Builds the manuals' PDFs from their Markdown sources.

    python docs/manuals/build.py            every manual
    python docs/manuals/build.py quick-guide.md
    python docs/manuals/build.py docs/server/server-mqtt-brief.md   (any path)

Each source starts with a version block (--- name / lang / version /
status / date / firmware ---). The version goes into the PDF's file name
(pdf/mCOLD_<name>_v<version>.pdf), its first page and every page footer.
Bump `version` in the source when its content changes, then rebuild.

Needs Python 3 and Microsoft Edge (headless print), nothing else: the
Markdown here is a small subset (headings, paragraphs, lists, tables,
**bold**, `code`, ``` blocks, ---) converted below. Fonts come from mcold-spec/fonts.
"""
import html
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
FONTS = (HERE.parent.parent / "mcold-spec" / "fonts").as_uri()
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
SOURCES = ["quick-guide.md", "user-manual-th.md", "user-manual-en.md", "service-manual.md"]

LABELS = {
    "th": {"version": "เวอร์ชัน", "draft": "ฉบับร่าง", "release": "ฉบับใช้งาน",
           "firmware": "firmware", "hardware": "hardware", "page": "หน้า"},
    "en": {"version": "Version", "draft": "draft", "release": "release",
           "firmware": "firmware", "hardware": "hardware", "page": "page"},
}


def front_matter(text):
    m = re.match(r"---\n(.*?)\n---\n", text, re.S)
    if not m:
        sys.exit("no version block at the top")
    meta = dict(line.split(":", 1) for line in m.group(1).splitlines() if ":" in line)
    return {k.strip(): v.strip() for k, v in meta.items()}, text[m.end():]


def inline(s):
    s = html.escape(s, quote=False).replace("\\|", "|")
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", s)
    return s


def cells(row):
    row = row.strip().strip("|")
    return [c.strip() for c in re.split(r"(?<!\\)\|", row)]


def to_html(md):
    out, para, lines, i = [], [], md.splitlines(), 0

    def flush():
        if para:
            out.append("<p>" + inline(" ".join(para)) + "</p>")
            para.clear()

    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if not s:
            flush()
        elif s == "---":
            flush()
            out.append("<hr>")
        elif s.startswith("```"):
            flush()
            code = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i].strip())
                i += 1
            out.append("<pre>" + html.escape(chr(10).join(code)) + "</pre>")
        elif m := re.match(r"(#{1,3}) (.*)", s):
            flush()
            n = len(m.group(1))
            out.append(f"<h{n}>{inline(m.group(2))}</h{n}>")
        elif s.startswith("|"):
            flush()
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(lines[i])
                i += 1
            head, body = cells(rows[0]), [cells(r) for r in rows[2:]]
            t = ["<table><thead><tr>" + "".join(f"<th>{inline(c)}</th>" for c in head) + "</tr></thead><tbody>"]
            for r in body:
                t.append("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>")
            out.append("".join(t) + "</tbody></table>")
            continue
        elif re.match(r"(- |\d+\. )", s):
            flush()
            tag = "ol" if s[0].isdigit() else "ul"
            items = []
            while i < len(lines) and re.match(r"\s*(- |\d+\. )", lines[i]):
                item = re.sub(r"\s*(- |\d+\. )", "", lines[i], count=1).strip()
                i += 1
                while (i < len(lines) and lines[i].startswith("  ") and lines[i].strip()
                       and not lines[i].strip().startswith("```")):
                    item += " " + lines[i].strip()   # a wrapped list item
                    i += 1
                items.append(f"<li>{inline(item)}</li>")
            out.append(f"<{tag}>" + "".join(items) + f"</{tag}>")
            continue
        else:
            para.append(s)
        i += 1
    flush()
    return "\n".join(out)


CSS = """
@font-face { font-family: Plex; src: url("%(f)s/IBMPlexSansThai-Regular.ttf"); font-weight: 400; }
@font-face { font-family: Plex; src: url("%(f)s/IBMPlexSansThai-SemiBold.ttf"); font-weight: 600; }
@font-face { font-family: Plex; src: url("%(f)s/IBMPlexSansThai-Bold.ttf"); font-weight: 700; }
:root { --ink:#1d2433; --muted:#5b6475; --line:#d9dee7; --soft:#f4f6f9; --brand:#0f5c8c; }
@page { size: A4; margin: 15mm 15mm 17mm 15mm;
  @bottom-left { content: "%(foot)s"; font: 8pt Plex, sans-serif; color: #5b6475; }
  @bottom-right { content: "%(page)s " counter(page) " / " counter(pages); font: 8pt Plex, sans-serif; color: #5b6475; } }
html, body { background: #fff; }
body { font-family: Plex, sans-serif; color: var(--ink); font-size: 10pt; line-height: 1.55; margin: 0; }
.meta { color: var(--muted); font-size: 9pt; border-bottom: 2.5px solid var(--brand);
  padding-bottom: 8px; margin: 2px 0 14px; }
.meta b { color: var(--ink); font-weight: 600; }
h1 { font-size: 20pt; font-weight: 700; color: var(--brand); margin: 0; line-height: 1.25; }
h2 { font-size: 13pt; font-weight: 700; margin: 18px 0 6px; break-after: avoid; }
h3 { font-size: 11pt; font-weight: 600; margin: 12px 0 4px; break-after: avoid; }
p { margin: 4px 0 8px; }
table { width: 100%%; border-collapse: collapse; margin: 4px 0 10px; }
th, td { text-align: left; vertical-align: top; padding: 4px 7px; border-bottom: 1px solid var(--line); }
th { font-weight: 600; font-size: 9pt; color: var(--muted); background: var(--soft); }
tr { break-inside: avoid; }
td:first-child { font-weight: 600; }
ul, ol { margin: 2px 0 8px; padding-left: 20px; }
li { margin: 2px 0; }
code { font-family: Consolas, monospace; font-size: 9pt; background: var(--soft); padding: 0 3px; border-radius: 3px; }
pre { font-family: Consolas, monospace; font-size: 8.6pt; background: var(--soft);
  border: 1px solid var(--line); border-radius: 5px; padding: 6px 9px; margin: 4px 0 8px;
  white-space: pre-wrap; break-inside: avoid; }
hr { border: 0; border-top: 1px solid var(--line); margin: 18px 0 6px; }
hr + h2 { color: var(--muted); font-size: 11pt; }
hr ~ ul, hr ~ ul li { color: var(--muted); font-size: 9pt; }
"""


def build(src):
    # A name here, or any path: other documents (docs/server/..., docs/app/...) build the
    # same way, into a pdf/ folder beside their source.
    path = HERE / src if (HERE / src).exists() else pathlib.Path(src).resolve()
    out = path.parent / "pdf"
    meta, body = front_matter(path.read_text(encoding="utf-8"))
    L = LABELS.get(meta.get("lang", "en"), LABELS["en"])
    ver, status = meta["version"], meta.get("status", "draft")
    title = re.search(r"^# (.*)$", body, re.M).group(1)
    tag = f"{L['version']} {ver} ({L['draft'] if status == 'draft' else L['release']})"
    parts = [f"<b>{tag}</b>", meta.get("date", "")]
    if meta.get("firmware"):
        parts.append(f"{L['firmware']} {meta['firmware']}")
    if meta.get("hardware"):
        parts.append(f"{L['hardware']} {meta['hardware']}")
    content = to_html(body)
    # The version line goes right under the title.
    content = content.replace("</h1>", "</h1>\n<div class='meta'>" + " · ".join(p for p in parts if p) + "</div>", 1)
    foot = f"mCOLD Foam V.1 · {meta['name'].replace('-', ' ').replace('_', ' ')} · {L['version']} {ver} · {meta.get('date', '')}"
    css = CSS % {"f": FONTS, "foot": foot.replace('"', ""), "page": L["page"]}
    if meta.get("compact") == "yes":   # one page: the Quick Guide
        css += ("body{font-size:8.6pt;line-height:1.38} h1{font-size:16pt} .meta{margin-bottom:8px}"
                " h2{font-size:10.5pt;margin:9px 0 3px} p{margin:2px 0 3px} th,td{padding:2px 6px}"
                " hr{margin:9px 0 3px} @page{margin:11mm 13mm 13mm 13mm}")
    page = (f"<!doctype html><html lang='{meta.get('lang', 'en')}'><head><meta charset='utf-8'>"
            f"<title>{html.escape(title)}</title><style>{css}</style></head><body>{content}</body></html>")

    out.mkdir(exist_ok=True)
    built = out / f"{path.stem}.html"
    built.write_text(page, encoding="utf-8")
    pdf = out / f"mCOLD_{meta['name']}_v{ver}.pdf"
    cmd = [EDGE, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
           "--allow-file-access-from-files", "--virtual-time-budget=8000",
           f"--print-to-pdf={pdf}", built.as_uri()]
    subprocess.run(cmd, check=True, timeout=180, capture_output=True)
    built.unlink()
    print("wrote", pdf.relative_to(HERE.parent.parent), pdf.stat().st_size, "bytes")


if __name__ == "__main__":
    for s in sys.argv[1:] or SOURCES:
        build(s)
