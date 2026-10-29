"""mflash: the factory station for mCOLD (and, once its profile is filled in, eTEMP).

    mflash ports                                  what is on the USB bus
    mflash fw build mcold production firmware/v0.6.0
    mflash fw approve mcold production 0.6.0 --by somchai
    mflash plan mcold all                         what a run would do, without a box
    mflash run mcold all                          test -> production -> setup -> verify
    mflash run mcold verify --all-ports           every box on the bus at once
    mflash run mcold reflash_setup                a box already made: new app and settings, same SN
    mflash history mCDV1-L0169-1069-001
    mflash export records.csv
    mflash summary mcold mCDV1-L0169-1069         every SN of a lot, its box and its result
    mflash gui                                    the six-slot window
    mflash gui --demo                             the window with simulated boxes
"""
from __future__ import annotations

import argparse
import getpass
import json
import os
import sys
import threading

from . import firmware as fwlib
from . import profile as proflib
from . import site as sitelib
from .db import Db
from .esp import Esp
from .ports import UsbIds, scan
from .runner import Box, Runner, StepFailed
from .ui import TerminalUi


def _profile(name: str) -> proflib.Profile:
    try:
        p = proflib.load(name)
    except proflib.ProfileError as e:
        sys.exit(str(e))
    if p.incomplete:
        print(f"!! profile {p.product} is incomplete:", file=sys.stderr)
        for line in p.incomplete:
            print(f"   - {line}", file=sys.stderr)
    return p


def _stages(p: proflib.Profile, stage: str) -> list[str]:
    mode = p.mode(stage)            # all, resetup, reflash, reflash_setup ...
    if mode is not None:
        return mode
    out = stage.split(",")
    for s in out:
        p.stage(s)
    return out


def cmd_ports(a) -> None:
    profiles = []
    for name in proflib.list_profiles():
        try:
            profiles.append(proflib.load(name))
        except proflib.ProfileError:
            pass
    for port in scan():
        tag = ""
        for p in profiles:
            ids = UsbIds(p.usb)
            if ids.is_app(port):
                tag = f"{p.product} firmware"
            elif ids.is_usj(port):
                tag = f"{p.product}? USB-Serial-JTAG (ROM, blank chip or test firmware)"
            elif ids.is_rom(port):
                tag = f"{p.product}? ROM download mode (USB-OTG)"
            if tag:
                break
        print(f"{port}  {tag}")


def cmd_profiles(a) -> None:
    for name in proflib.list_profiles():
        try:
            p = proflib.load(name)
            state = "INCOMPLETE" if p.incomplete else "ok"
            print(f"{name:<10} {state:<10} {p.name}  stages: {' -> '.join(p.sequence)}")
        except proflib.ProfileError as e:
            print(f"{name:<10} ERROR      {e}")


def _describe(st: dict) -> str:
    t = st["type"]
    bits = []
    if st.get("send"):
        bits.append(f"> {st['send']}")
    if t == "flash":
        bits.append(f"firmware {st['firmware']}" + (", erase whole chip" if st.get("erase") else "")
                    + (f", only {' + '.join(st['only'])}, nothing erased" if st.get("only") else ""))
    if st.get("expect"):
        bits.append("expect " + " & ".join(f"/{x}/" for x in st["expect"]))
    if st.get("forbid"):
        bits.append("forbid " + " | ".join(f"/{x}/" for x in st["forbid"]))
    if t == "health":
        bits.append("devices ok" + (", allowed: " + json.dumps(st.get("states")) if st.get("states") else ""))
    if st.get("ask"):
        bits.append(f"ask: {st['ask']}")
    if st.get("enabled") is False:
        bits.append("(disabled)")
    if st.get("optional"):
        bits.append("(optional)")
    return "; ".join(bits)


def cmd_plan(a) -> None:
    p = _profile(a.product)
    for s in _stages(p, a.stage):
        print(f"\n== {s}")
        for i, st in enumerate(p.stage(s), 1):
            print(f"  {i:2}. [{st['type']}] {st['name']}")
            d = _describe(st)
            if d:
                print(f"      {d}")
    print()
    for kind in p.firmware:
        try:
            m = fwlib.resolve(p.product, kind, "approved")
            print(f"{kind}: {m['id']} (approved by {m['approved']['by']})")
        except fwlib.FirmwareError as e:
            print(f"{kind}: {e}")


def cmd_fw(a) -> None:
    if a.fw_cmd == "list":
        rows = fwlib.list_firmware(a.product, a.kind)
        if not rows:
            print("library is empty")
        for m in rows:
            appr = f"approved by {m['approved']['by']}" if m.get("approved") else "NOT approved"
            src = m["source"].get("git_ref") or m["source"].get("build_dir", "")
            print(f"{m['kind']:<11} {m['id']:<28} {m['project_name']:<14} "
                  f"{'signed' if m['signed'] else 'unsigned':<9} {appr:<26} {src}")
        return
    p = _profile(a.product)
    if a.kind not in p.firmware:
        sys.exit(f"{p.product} has no firmware '{a.kind}' (has {', '.join(p.firmware)})")
    try:
        if a.fw_cmd == "build":
            m = fwlib.build_from_git(p, a.kind, a.ref)
        elif a.fw_cmd == "import":
            m = fwlib.import_build(p, a.kind, a.build_dir)
        elif a.fw_cmd == "approve":
            m = fwlib.approve(p.product, a.kind, a.which, a.by or getpass.getuser())
        else:
            sys.exit(f"unknown fw command {a.fw_cmd}")
    except fwlib.FirmwareError as e:
        sys.exit(str(e))
    print(f"{m['kind']} {m['id']}: {m['project_name']} {m['version']}, IDF {m['idf']}, "
          f"built {m['built']}, {'signed' if m['signed'] else 'unsigned'}")
    for img in m["images"]:
        print(f"  0x{img['offset']:06X}  {img['file']:<24} {img['size']:>9} B  {img['sha256'][:16]}")
    if m.get("approved"):
        print(f"  approved by {m['approved']['by']} at {m['approved']['at']}")


def _pick_ports(p: proflib.Profile, a) -> list:
    ids = UsbIds(p.usb)
    ours = [x for x in scan() if ids.ours(x)]
    if a.port:
        hit = [x for x in scan() if x.device == a.port]
        if not hit:
            sys.exit(f"no port {a.port}")
        return hit
    if a.all_ports:
        if not ours:
            sys.exit("no boxes on the bus")
        return ours
    if len(ours) != 1:
        found = "\n  ".join(str(x) for x in ours) or "none"
        sys.exit(f"{len(ours)} boxes on the bus; name one with --port or use --all-ports:\n  {found}")
    return ours


def cmd_run(a) -> None:
    p = _profile(a.product)
    if p.incomplete and not a.allow_incomplete:
        sys.exit("refusing to run an incomplete profile (--allow-incomplete for bench work)")
    stages = _stages(p, a.stage)
    choice = {}
    for c in a.fw or []:
        k, _, v = c.partition("=")
        choice[k] = v
    site = sitelib.load(a.site)
    db = Db(a.db)
    ui = TerminalUi(assume_yes=a.yes)
    ports = _pick_ports(p, a)
    if a.sn and len(ports) > 1:
        sys.exit("--sn names one box; it cannot go with several ports")
    results = {}

    def one(i, port):
        box = Box(label=f"{i + 1}:{port.device}", port=port)
        r = Runner(p, db, ui, site, firmware_choice=choice, allow_unapproved=a.allow_unapproved,
                   operator=a.operator or getpass.getuser(), sn=a.sn or "")
        try:
            results[box.label] = (r.run(box, stages), box)
        except Exception as e:      # one box's crash must not take the others with it
            ui.info(box.label, f"stopped: {e!r}")
            results[box.label] = (False, box)

    threads = [threading.Thread(target=one, args=(i, port)) for i, port in enumerate(ports)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print()
    bad = 0
    for label, (ok, box) in sorted(results.items()):
        print(f"{'PASS' if ok else 'FAIL'}  {label:<18} {box.mac or '?':<18} {box.sn}")
        bad += not ok
    sys.exit(1 if bad else 0)


def cmd_history(a) -> None:
    db = Db(a.db)
    for r in db.history(a.who or "", a.limit):
        print(f"#{r['id']:<5} {r['started']}  {r['product']:<6} {r['stage']:<11} "
              f"{r['result'] or 'running':<7} {r['mac'] or '':<18} {r['sn'] or '':<21} "
              f"{r['firmware'] or ''}{'  UNAPPROVED' if r['unapproved'] else ''}")


def cmd_show(a) -> None:
    db = Db(a.db)
    for s in db.steps(a.run_id):
        print(f"[{s['result']}] {s['name']}" + (f" -- {s['detail']}" if s['detail'] else ""))
        if a.output and s["output"]:
            print("    " + s["output"].strip().replace("\n", "\n    "))


def cmd_export(a) -> None:
    n = Db(a.db).export_csv(a.path, a.product or "")
    print(f"{n} boxes -> {a.path}")


def cmd_backup(a) -> None:
    """Everything on a box worth keeping, read out before it is repaired or reflashed."""
    p = _profile(a.product)
    fk = p.firmware.get("production")
    if not fk or not fk.partitions_csv:
        sys.exit("the production firmware has no partitions_csv to read the layout from")
    rows = fwlib.read_partitions_csv(os.path.join(proflib.REPO, fk.partitions_csv))
    want = [r for r in rows if r[0] in a.parts.split(",")]
    ports = _pick_ports(p, a)
    box = Box(label=ports[0].device, port=ports[0])
    r = Runner(p, Db(a.db), TerminalUi(), sitelib.load(a.site))
    try:
        r.identify(box)
        port, otg = r.to_loader(box)
    except StepFailed as e:
        sys.exit(e.detail)
    out = os.path.join(a.out, (box.sn or box.mac.replace(":", "")) or "box")
    os.makedirs(out, exist_ok=True)
    esp = Esp(p.chip, p.flash)
    for name, off, size in want:
        path = os.path.join(out, f"{name}.bin")
        res = esp.read_flash(port.device, off, size, path,
                             before="no_reset" if otg else "default_reset")
        print(f"  {name:<9} 0x{off:06X} 0x{size:06X} -> {path}  {'ok' if res.ok else 'FAILED'}")
        if not res.ok:
            print(res.output[-600:])
            break
    esp.reset(port.device, otg)


def cmd_summary(a) -> None:
    from .station import Station
    p = _profile(a.product)
    st = Station(p.product, Db(a.db), sitelib.load(a.site), hw=_NoBus())
    if a.lot and not p.lot_ok(a.lot):
        sys.exit(f"'{a.lot}' is not a lot of {p.product} (e.g. {p.sn.get('lot_example', '')})")
    if a.csv:
        with open(a.csv, "w", encoding="utf-8-sig", newline="") as f:
            f.write(st.summary_csv(a.lot or ""))
        print(f"-> {a.csv}")
        return
    sm = st.summary(a.lot or "")
    print(f"{p.product} lot {sm['lot'] or '(all)'}: {sum(1 for r in sm['rows'] if r['sn'])} SNs, "
          f"{sm['counts']['PASS']} passed, {sm['counts']['FAIL']} failed, "
          f"{sm['counts']['INCOMPLETE']} incomplete")
    if sm["missing"]:
        print("unit numbers skipped: " + ", ".join(str(u) for u in sm["missing"]))
    for r in sm["rows"]:
        why = f"{r['fail']['stage']}: {r['fail']['step']}" if r["fail"] else ""
        print(f"  {r['sn'] or '(no SN)':<22} {r['result']:<10} {r['mac']:<18} {r['firmware'] or '-':<14} "
              f"{(r['finished'] or '')[:16]:<16} {r['operator'] or '':<10} {why}")


class _NoBus:
    """A station that only reads its record touches no port."""

    def scan(self):
        return []


def cmd_gui(a) -> None:
    from .gui import serve
    serve(a.product, host=a.host, port=a.http_port, demo=a.demo, browser=not a.no_browser,
          db_path=a.db, site_path=a.site, speed=a.speed)


def main(argv=None) -> None:
    ap = argparse.ArgumentParser(prog="mflash", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", help="record database (default flasher/records/mflash.db)")
    ap.add_argument("--site", help="site settings (default flasher/site.ini)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports", help="USB ports and what is on them").set_defaults(fn=cmd_ports)
    sub.add_parser("profiles", help="product profiles").set_defaults(fn=cmd_profiles)

    s = sub.add_parser("plan", help="print what a run would do")
    s.add_argument("product")
    s.add_argument("stage", nargs="?", default="all")
    s.set_defaults(fn=cmd_plan)

    fw = sub.add_parser("fw", help="the firmware library")
    fws = fw.add_subparsers(dest="fw_cmd", required=True)
    x = fws.add_parser("build", help="build a git ref of this repo and import it")
    x.add_argument("product"); x.add_argument("kind"); x.add_argument("ref")
    x = fws.add_parser("import", help="import a PlatformIO build folder")
    x.add_argument("product"); x.add_argument("kind"); x.add_argument("build_dir")
    x = fws.add_parser("list", help="what is in the library")
    x.add_argument("product"); x.add_argument("kind", nargs="?")
    x = fws.add_parser("approve", help="allow an image on the production line")
    x.add_argument("product"); x.add_argument("kind"); x.add_argument("which", help="id or version")
    x.add_argument("--by")
    fw.set_defaults(fn=cmd_fw)

    s = sub.add_parser("run", help="run stages on boxes")
    s.add_argument("product")
    s.add_argument("stage", help="a mode (all, resetup, reflash, reflash_setup) or stages with commas")
    s.add_argument("--port")
    s.add_argument("--all-ports", action="store_true", help="every box of this product on the bus")
    s.add_argument("--fw", action="append", metavar="KIND=WHICH",
                   help="firmware other than the newest approved, e.g. production=0.7.0")
    s.add_argument("--sn", help="SN for a single box instead of asking")
    s.add_argument("--operator")
    s.add_argument("--allow-unapproved", action="store_true", help="engineer: unapproved images")
    s.add_argument("--allow-incomplete", action="store_true", help="engineer: incomplete profile")
    s.add_argument("--yes", action="store_true", help="answer yes to every check (bench only)")
    s.set_defaults(fn=cmd_run)

    s = sub.add_parser("history", help="past runs")
    s.add_argument("who", nargs="?", help="MAC or SN")
    s.add_argument("--limit", type=int, default=50)
    s.set_defaults(fn=cmd_history)

    s = sub.add_parser("show", help="the steps of one run")
    s.add_argument("run_id", type=int)
    s.add_argument("--output", action="store_true", help="with the console text")
    s.set_defaults(fn=cmd_show)

    s = sub.add_parser("export", help="one CSV row per box")
    s.add_argument("path")
    s.add_argument("--product")
    s.set_defaults(fn=cmd_export)

    s = sub.add_parser("backup", help="read NVS, trip log and core dump off a box")
    s.add_argument("product")
    s.add_argument("--port")
    s.add_argument("--all-ports", action="store_true", help=argparse.SUPPRESS)
    s.add_argument("--parts", default="nvs,log_meta,trip_log,coredump")
    s.add_argument("--out", default=os.path.join(proflib.FLASHER, "backups"))
    s.set_defaults(fn=cmd_backup)

    s = sub.add_parser("summary", help="every SN of a lot: box, firmware, result")
    s.add_argument("product")
    s.add_argument("lot", nargs="?", help="e.g. mCDV1-L0169-1069; none = every lot")
    s.add_argument("--csv", help="write it as CSV instead")
    s.set_defaults(fn=cmd_summary)

    s = sub.add_parser("gui", help="the six-slot station window")
    s.add_argument("product", nargs="?", default="mcold")
    s.add_argument("--demo", action="store_true", help="simulated boxes, nothing is flashed")
    s.add_argument("--speed", type=float, default=0.4, help="demo: delays x this (1 = real time)")
    s.add_argument("--host", default="127.0.0.1", help="0.0.0.0 to reach it from a tablet")
    s.add_argument("--http-port", type=int, default=8765)
    s.add_argument("--no-browser", action="store_true", help="do not open a window")
    s.set_defaults(fn=cmd_gui)

    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
