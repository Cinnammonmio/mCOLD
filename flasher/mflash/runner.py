"""Running a profile's stages on one box.

A stage is a list of steps; a step passes, fails or is skipped, and the
first failure that is not `optional` ends the stage. Every step's verdict
and the console text it was judged on go into the record.

The box restarts several times in one run (into the loader, into a new
firmware, after `sn set`), and each time it comes back on a different
port. It is followed by its MAC and SN (ports.py), never by guessing.
"""
from __future__ import annotations

import datetime as dt
import os
import re
import time
from dataclasses import dataclass, field

from . import firmware as fwlib
from . import parsers
from .console import Console, ConsoleError
from .db import Db
from .esp import Esp
from .ports import Port, UsbIds, norm_mac, scan
from .profile import Profile
from .site import Site, mask
from .ui import Ui

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"


class StepFailed(Exception):
    def __init__(self, detail: str, output: str = ""):
        super().__init__(detail)
        self.detail, self.output = detail, output


@dataclass
class StepResult:
    result: str
    detail: str = ""
    output: str = ""


@dataclass
class Box:
    label: str
    mac: str = ""
    sn: str = ""
    port: Port | None = None
    flashed: dict = field(default_factory=dict)      # firmware kind -> manifest
    running: str = ""                                # firmware kind on it now, if known


class Hardware:
    """The real bus. Tests replace it with a simulated box."""

    def scan(self) -> list[Port]:
        return scan()

    def console(self, port: Port, baud: int, eol: str) -> Console:
        return Console(port.device, baud, eol)

    def sleep(self, s: float) -> None:
        time.sleep(s)

    def esp(self, chip: str, flash: dict) -> Esp:
        return Esp(chip, flash, log=lambda m: None)


def fill(s: str, ctx: dict, escape: bool = False) -> str:
    """{sn} and {mac} in a command or a pattern; other braces (JSON) are left alone."""
    def rep(m):
        k = m.group(1)
        if k not in ctx:
            return m.group(0)
        v = str(ctx[k])
        return re.escape(v) if escape else v
    return re.sub(r"\{(\w+)\}", rep, s)


class Runner:
    def __init__(self, profile: Profile, db: Db, ui: Ui, site: Site, hw: Hardware | None = None,
                 firmware_choice: dict | None = None, allow_unapproved: bool = False,
                 operator: str = "", station: str = "", sn: str = "", should_stop=None,
                 keep_existing_sn: bool = False):
        self.p, self.db, self.ui, self.site = profile, db, ui, site
        self.hw = hw or Hardware()
        self.ids = UsbIds(profile.usb)
        self.choice = firmware_choice or {}
        self.allow_unapproved = allow_unapproved
        self.operator = operator
        self.station = station or site.station
        self.preset_sn = sn
        self.should_stop = should_stop or (lambda: False)
        # a box that already carries a valid SN keeps it (a rework), whatever
        # number its slot was going to give it
        self.keep_existing_sn = keep_existing_sn
        self.baud = profile.console.get("baud", 115200)
        self.eol = profile.console.get("eol", "\r\n")
        self.esp = self.hw.esp(profile.chip, profile.flash)
        self._secrets = site.secrets()

    # ---- finding the box ----------------------------------------------

    def identify(self, box: Box) -> None:
        """MAC and SN of the box on box.port, from what the port itself says."""
        p = box.port
        if self.ids.is_usj(p) and p.serial:
            box.mac = norm_mac(p.serial)
        elif self.ids.is_app(p):
            out = self._cmd(box, 'rpc {"id":1,"cmd":"GET_STATUS"}', quiet=1.0)
            box.mac = parsers.rpc_mac(out) or box.mac
            box.running = box.running or "production"
            if p.serial and p.serial != self.ids.unset_name(box.mac):
                box.sn = p.serial
        if box.mac:
            self.db.seen(box.mac, self.p.product)
            d = self.db.device(box.mac)
            if d and d["sn"] and not box.sn:
                box.sn = d["sn"]

    def locate(self, box: Box, kind: str, timeout: float) -> Port:
        end = time.monotonic() + timeout
        while True:
            ports = self.hw.scan()
            if self.ids.bridge and box.port:
                same = [p for p in ports if p.device == box.port.device]
                if same:
                    box.port = same[0]
                    return same[0]
            for p in ports:
                if self.ids.matches(p, box.mac, box.sn, kind):
                    box.port = p
                    return p
            if not box.mac and kind in ("rom", "any"):
                # a box whose MAC is not known yet (firmware too old to say):
                # only safe when it is the one loader on the bus
                roms = [p for p in ports if self.ids.is_rom(p)]
                if len(roms) == 1:
                    box.port = roms[0]
                    if self.ids.is_usj(roms[0]) and roms[0].serial:
                        box.mac = norm_mac(roms[0].serial)
                    return roms[0]
            if time.monotonic() >= end:
                raise StepFailed(f"the box ({box.mac or '?'} {box.sn}) did not come back "
                                 f"on a {kind} port within {timeout:.0f} s")
            self.hw.sleep(0.5)

    def to_loader(self, box: Box) -> tuple[Port, bool]:
        """Into ROM download mode; returns (port, is the USB-OTG ROM port)."""
        p = box.port
        if self.ids.is_rom(p):
            return p, not self.ids.is_usj(p)
        if not self.ids.is_app(p):
            raise StepFailed(f"{p.device} is not a port of this product")
        cmd = self.p.console.get("download_command", "flash")
        self._cmd(box, cmd, quiet=0.5, timeout=2, may_vanish=True)
        p = self.locate(box, "rom", 20)
        return p, not self.ids.is_usj(p)

    def console_port(self, box: Box, kind: str, timeout: float = 40) -> Port:
        """The port the firmware `kind` keeps its console on, once it is up."""
        where = self.p.firmware[kind].console
        self.hw.sleep(self.p.console.get("boot_wait_s", 3))
        want = "rom" if where == "usj" else "app"
        try:
            return self.locate(box, want, timeout * 0.75)
        except StepFailed:
            if want != "app":
                raise
            # mCOLD 0.6.0, or usb_drive 0: the console stays on the USB-Serial-JTAG
            p = self.locate(box, "rom", timeout * 0.25)
            self.hw.sleep(self.p.console.get("boot_wait_s", 3))
            return p

    # ---- console ------------------------------------------------------

    def _cmd(self, box: Box, line: str, quiet: float = 1.5, timeout: float = 10,
             until: str | None = None, may_vanish: bool = False) -> str:
        c = self.hw.console(box.port, self.baud, self.eol)
        try:
            c.open()
            return c.command(line, quiet=quiet, timeout=timeout, until=until, may_vanish=may_vanish)
        except ConsoleError as e:
            raise StepFailed(str(e)) from e
        finally:
            c.close()

    def _masked(self, s: str) -> str:
        return mask(s, self._secrets)

    # ---- running ------------------------------------------------------

    def run(self, box: Box, stages: list[str]) -> bool:
        for s in stages:
            if not self.run_stage(box, s):
                return False
        return True

    def run_stage(self, box: Box, stage: str) -> bool:
        steps = self.p.stage(stage)
        if box.port and not box.mac:
            try:
                self.identify(box)
            except StepFailed as e:
                self.ui.info(box.label, f"could not ask the box who it is: {e.detail}")
        run_id = self.db.start_run(self.p.product, stage, box.mac, box.sn, self.station,
                                   self.operator, unapproved=self.allow_unapproved)
        self.ui.info(box.label, f"== {stage} ({len(steps)} steps)  {box.mac or ''} {box.sn}")
        self.ui.stage_started(box.label, stage, len(steps))
        ok = True
        for i, st in enumerate(steps):
            name = st["name"]
            if self.should_stop():
                self.db.add_step(run_id, i, name, st["type"], FAIL, "stopped by the operator", "")
                self.ui.info(box.label, f"  [stop] {name}")
                ok = False
                break
            self.ui.step_started(box.label, i, name)
            if st.get("enabled", True) is False:
                res = StepResult(SKIP, "disabled in the profile")
            else:
                res = self._attempt(box, st, run_id)
            self.db.add_step(run_id, i, name, st["type"], res.result, self._masked(res.detail),
                             self._masked(res.output))
            mark = {"PASS": "ok  ", "FAIL": "FAIL", "SKIP": "skip"}[res.result]
            self.ui.info(box.label, f"  [{mark}] {name}" + (f" -- {res.detail}" if res.detail else ""))
            self.ui.step_done(box.label, i, name, res.result, self._masked(res.detail),
                              st.get("help", "") if res.result == FAIL else "")
            if res.result == FAIL:
                if st.get("help"):
                    self.ui.info(box.label, f"         {st['help']}")
                if not st.get("optional"):
                    ok = False
                    break
        result = PASS if ok else FAIL
        self.db.finish_run(run_id, result, box.mac, box.sn)
        self.ui.info(box.label, f"== {stage}: {result}")
        self.ui.stage_done(box.label, stage, result)
        return ok

    def _attempt(self, box: Box, st: dict, run_id: int) -> StepResult:
        tries = int(st.get("retries", 0)) + 1
        last = StepResult(FAIL, "not run")
        for n in range(tries):
            if n and self.should_stop():
                break
            try:
                handler = getattr(self, "step_" + st["type"])
                last = handler(box, st, run_id) or StepResult(PASS)
            except StepFailed as e:
                last = StepResult(FAIL, e.detail, e.output)
            if last.result != FAIL:
                return last
            if n + 1 < tries:
                self.hw.sleep(float(st.get("retry_wait", 5)))
        return last

    def _ctx(self, box: Box) -> dict:
        return {"sn": box.sn, "mac": box.mac}

    def _check(self, out: str, st: dict, box: Box) -> None:
        ctx = self._ctx(box)
        for rx in st.get("expect", []) or []:
            if not re.search(fill(rx, ctx, escape=True), out, re.M):
                raise StepFailed(st.get("expect_fail", f"expected /{rx}/"), out)
        for rx in st.get("forbid", []) or []:
            m = re.search(fill(rx, ctx, escape=True), out, re.M)
            if m:
                raise StepFailed(f"found '{m.group(0).strip()}'", out)

    # ---- step types -----------------------------------------------------

    def step_note(self, box: Box, st: dict, run_id: int) -> StepResult:
        self.ui.info(box.label, st.get("text", st["name"]))
        if st.get("wait"):
            self.ui.yes_no(box.label, "พร้อมแล้ว / ready?")
        return StepResult(PASS)

    def step_confirm(self, box: Box, st: dict, run_id: int) -> StepResult:
        """Run the check, ask the person; "again" runs it once more and asks again
        (the LEDs went by while they were looking elsewhere)."""
        out = ""
        for n in range(int(st.get("max_repeat", 10)) + 1):
            if st.get("send"):
                out = self._cmd(box, fill(st["send"], self._ctx(box)), quiet=st.get("quiet", 1.5),
                                timeout=st.get("timeout", 15))
                if st.get("expect") or st.get("forbid"):
                    self._check(out, st, box)
            a = self.ui.yes_no(box.label, st["ask"], repeat=bool(st.get("send")))
            if a is None:
                continue
            if not a:
                raise StepFailed("operator: no", out)
            return StepResult(PASS, f"after {n} repeats" if n else "", out)
        raise StepFailed("repeated too many times", out)

    def step_console(self, box: Box, st: dict, run_id: int) -> StepResult:
        out = self._cmd(box, fill(st["send"], self._ctx(box)), quiet=st.get("quiet", 1.5),
                        timeout=st.get("timeout", 10), until=st.get("until"),
                        may_vanish=st.get("may_vanish", False))
        self._check(out, st, box)
        return StepResult(PASS, "", out)

    def step_chip(self, box: Box, st: dict, run_id: int) -> StepResult:
        port, otg = self.to_loader(box)
        r, info = self.esp.chip_info(port.device, before="no_reset" if otg else "default_reset")
        if not r.ok:
            raise StepFailed("esptool could not talk to the chip", r.output)
        self.esp.reset(port.device, otg=otg)
        if info.get("mac"):
            box.mac = norm_mac(info["mac"])
            self.db.seen(box.mac, self.p.product, info.get("chip", ""))
        bad = []
        for k, want in (self.p.chip_expect or {}).items():
            got = info.get(k, "")
            if str(want).lower() not in got.lower():
                bad.append(f"{k} {got or '?'} (expected {want})")
        for k in ("chip", "revision", "flash_size", "psram", "features"):
            if info.get(k):
                self.db.set_value(box.mac, "chip_" + k, info[k], run_id)
        detail = ", ".join(f"{k} {v}" for k, v in info.items() if k != "features")
        if bad:
            raise StepFailed("; ".join(bad), r.output)
        if box.running:
            self.console_port(box, box.running)
        else:
            self.locate(box, "any", 40)
        return StepResult(PASS, detail, r.output)

    def _firmware(self, kind: str) -> dict:
        which = self.choice.get(kind, "approved")
        m = fwlib.resolve(self.p.product, kind, which)
        if not m.get("approved") and not self.allow_unapproved:
            raise StepFailed(f"{kind} {m['id']} is not approved (mflash fw approve, "
                             "or --allow-unapproved)")
        return m

    def step_flash(self, box: Box, st: dict, run_id: int) -> StepResult:
        kind = st["firmware"]
        try:
            m = self._firmware(kind)
            images = fwlib.verify_files(m)
        except fwlib.FirmwareError as e:
            raise StepFailed(str(e)) from e
        if st.get("only"):
            # a box already made: the app (and an otadata that boots it), nothing else
            images = [(o, p) for o, p in images if os.path.basename(p) in st["only"]]
            if not images:
                raise StepFailed(f"none of {st['only']} in {kind} {m['id']}")
        self.db.update_run(run_id, firmware=f"{kind} {m['id']}")
        self.ui.info(box.label, f"  flashing {kind} {m['id']}"
                     + (" (whole chip erased first)" if st.get("erase") else ""))
        port, otg = self.to_loader(box)
        r = self.esp.write(port.device, images, erase_all=bool(st.get("erase")),
                           before="no_reset" if otg else "default_reset",
                           after="no_reset" if otg else "hard_reset")
        if r.mac:
            box.mac = norm_mac(r.mac)
            self.db.seen(box.mac, self.p.product)
        if not r.ok:
            raise StepFailed("write failed; the box is still in download mode -- run again",
                             r.output[-2000:])
        if otg:
            self.esp.reset(port.device, otg=True)
        box.flashed[kind] = m
        box.running = kind
        self.db.set_value(box.mac, "firmware_" + kind, m["id"], run_id)
        self.console_port(box, kind, st.get("boot_timeout", 40))
        return StepResult(PASS, f"{m['version']} on {box.port.device}", r.output[-2000:])

    def step_reboot(self, box: Box, st: dict, run_id: int) -> StepResult:
        self._cmd(box, st.get("send", "reboot"), quiet=0.5, timeout=2, may_vanish=True)
        self.hw.sleep(1.0)
        kind = box.running or "production"
        self.console_port(box, kind, st.get("timeout", 40))
        return StepResult(PASS, f"back on {box.port.device}")

    def step_version(self, box: Box, st: dict, run_id: int) -> StepResult:
        kind = st.get("firmware", "production")
        m = box.flashed.get(kind)
        if not m:
            try:
                m = self._firmware(kind)
            except fwlib.FirmwareError as e:
                raise StepFailed(str(e)) from e
        out = self._cmd(box, st.get("send", "version"))
        v = parsers.version(out)
        if not v:
            raise StepFailed("no version in the reply", out)
        ver, sha = v
        if ver != m["version"]:
            raise StepFailed(f"box runs {ver}, expected {m['version']}", out)
        if sha and not m["elf_sha256"].startswith(sha):
            raise StepFailed(f"box's ELF {sha} is not {m['elf_sha256'][:16]}: same version, "
                             "different build", out)
        return StepResult(PASS, f"{ver} elf {sha}", out)

    def step_health(self, box: Box, st: dict, run_id: int) -> StepResult:
        out = self._cmd(box, st.get("send", "health"), quiet=1.0)
        states = parsers.health_states(out)
        if not states:
            raise StepFailed("no device table in the reply", out)
        allowed = st.get("states", {}) or {}
        default = st.get("default", ["ok"])
        bad = [f"{d} {s}" for d, s in states.items() if s not in allowed.get(d, default)]
        for d in st.get("require_listed", []) or []:
            if d not in states:
                bad.append(f"{d} not listed")
        if st.get("temperature"):
            t = parsers.health_temperature(out)
            lo, hi = st.get("temp_range", [-60, 125])
            if t is None:
                bad.append("temperature -- (probe?)")
            elif not lo <= t <= hi:
                bad.append(f"temperature {t} outside {lo}..{hi}")
        if st.get("clock_usable"):
            c = parsers.health_clock(out)
            if not c or not c[2]:
                bad.append("clock not usable" if c else "clock no answer")
        if bad:
            raise StepFailed(", ".join(bad), out)
        ok = sum(1 for s in states.values() if s == "ok")
        return StepResult(PASS, f"{ok} ok of {len(states)}", out)

    def step_nmea(self, box: Box, st: dict, run_id: int) -> StepResult:
        secs = int(st.get("seconds", 15))
        out = self._cmd(box, fill(st["send"], {"seconds": secs}), quiet=3.0, timeout=secs + 10,
                        until=st.get("until"))
        good, bad, kinds = parsers.nmea_sentences(out)
        need = int(st.get("min_good", 3))
        if good < need:
            raise StepFailed(f"{good} sentences with a good checksum, {bad} bad (need {need}); "
                             "bytes alone do not count", out[-3000:])
        return StepResult(PASS, f"{good} good, {bad} bad ({', '.join(sorted(set(kinds)))})",
                          out[-3000:])

    def step_input_sn(self, box: Box, st: dict, run_id: int) -> StepResult:
        if self.keep_existing_sn and box.sn and self.p.sn_ok(box.sn):
            owner = self.db.sn_owner(self.p.product, box.sn)
            if not owner or not box.mac or owner == box.mac:
                return StepResult(PASS, f"{box.sn} (SN เดิมของเครื่อง)")
        if self.preset_sn:
            why = self._sn_problem(self.preset_sn.strip(), box)
            if why:
                raise StepFailed(why)
            box.sn = self.preset_sn.strip()
            return StepResult(PASS, box.sn)
        # typed or scanned: a wrong or used one is asked again, not a failed box
        ask = st.get("ask", "สแกน SN จากฉลาก")
        default = box.sn or ""
        for _ in range(int(st.get("max_tries", 5))):
            sn = self.ui.text(box.label, ask, default).strip()
            why = self._sn_problem(sn, box)
            if not why:
                box.sn = sn
                return StepResult(PASS, sn)
            ask = f"{why} · สแกน SN ใหม่"
            default = ""
        raise StepFailed(why)

    def _sn_problem(self, sn: str, box: Box) -> str:
        if not self.p.sn_ok(sn):
            return f"'{sn}' ไม่ตรงรูปแบบ SN (เช่น {self.p.sn.get('example', '')})"
        owner = self.db.sn_owner(self.p.product, sn)
        if owner and owner != box.mac:
            return f"{sn} ใช้แล้วกับกล่อง {owner}"
        return ""

    def step_set_sn(self, box: Box, st: dict, run_id: int) -> StepResult:
        if not box.sn:
            raise StepFailed("no SN chosen (an input_sn step comes first)")
        out = self._cmd(box, fill(st.get("send", "sn set {sn}"), self._ctx(box)))
        self._check(out, {"expect": st.get("expect", [r"stored"])}, box)
        if box.mac:
            self.db.set_sn(box.mac, box.sn)
        if st.get("reboot", True):
            self._cmd(box, "reboot", quiet=0.5, timeout=2, may_vanish=True)
            self.hw.sleep(1.0)
            self.console_port(box, box.running or "production", st.get("timeout", 60))
        back = self._cmd(box, st.get("check", "sn"))
        if box.sn not in back or "not the factory pattern" in back:
            raise StepFailed("the box does not report the new SN", back)
        return StepResult(PASS, f"{box.sn} on {box.port.device}", out + back)

    def step_provision(self, box: Box, st: dict, run_id: int) -> StepResult:
        s = self.site
        lines: list[tuple[str, str]] = []           # (command, pattern its reply must match)
        need = set(st.get("require", []) or [])
        out_all = ""
        if st.get("wifi", True):
            if not s.wifi and "wifi" in need:
                raise StepFailed(f"no [wifi.N] networks in {s.path}")
            if st.get("replace_wifi") and s.wifi:
                # the settings changed since this box was made: forget what is not in them
                known = parsers.wifi_known(self._cmd(box, "wifi", quiet=1.0))
                keep = {w.ssid for w in s.wifi}
                for ssid in known:
                    if ssid not in keep:
                        q = f'"{ssid}"' if " " in ssid else ssid
                        out = self._cmd(box, f"wifi del {q}", quiet=1.0)
                        out_all += f"> wifi del {ssid}\n{out}\n"
            for w in s.wifi:
                ssid = f'"{w.ssid}"' if " " in w.ssid else w.ssid
                lines.append((f"wifi add {ssid} {w.password}".rstrip(), r"stored \("))
                if not w.dhcp:
                    lines.append((f"wifi ip {w.ssid} {w.ip} {w.gateway} {w.subnet} {w.dns}".rstrip(),
                                  r"fixed address"))
        if st.get("mqtt", True):
            m = s.mqtt
            if not m and "mqtt" in need:
                raise StepFailed(f"no [mqtt] host in {s.path}")
            if m:
                if any(c.isspace() for c in m["username"] + m["password"]):
                    raise StepFailed("the console cannot take a broker login with spaces")
                cmd = f"mqtt set {m['host']} {m['port']}"
                if m["username"]:
                    cmd += f" {m['username']} {m['password']}"
                lines.append((cmd, r"stored; connecting"))
        if st.get("ota", True):
            if not s.ota_base and "ota" in need:
                raise StepFailed(f"no [ota] base in {s.path}")
            if s.ota_base:
                lines.append((f"ota base {s.ota_base}", r"^\s+stored"))
        if st.get("config", True):
            for k, v in s.config.items():
                lines.append((f"config set {k} {v}", rf"{re.escape(k)} = {v}, stored"))
        for cmd, pat in lines:
            out = self._cmd(box, cmd, quiet=1.0)
            out_all += f"> {cmd}\n{out}\n"
            if not re.search(pat, out, re.M):
                raise StepFailed(f"'{self._masked(cmd)}' refused", out_all)
        return StepResult(PASS, f"{len(lines)} settings", out_all)

    def step_calibrate(self, box: Box, st: dict, run_id: int) -> StepResult:
        """One-point offset against a reference thermometer.

        The offset is measured with calibration zeroed, so a box calibrated
        before gets the same answer as a new one.
        """
        for cmd in ("config set cal_offset_c100 0", "config set cal_gain_ppm 1000000"):
            out = self._cmd(box, cmd, quiet=1.0)
            if "stored" not in out:
                raise StepFailed(f"'{cmd}' refused", out)
        ref_s = self.ui.text(box.label, st.get("ask", "อุณหภูมิจากเทอร์โมมิเตอร์อ้างอิง (°C)"))
        try:
            ref = float(ref_s)
        except ValueError:
            raise StepFailed(f"'{ref_s}' is not a temperature")
        self.hw.sleep(float(st.get("settle_s", 10)))
        vals = []
        for _ in range(int(st.get("samples", 5))):
            t = parsers.health_temperature(self._cmd(box, "health", quiet=1.0))
            if t is not None:
                vals.append(t)
            self.hw.sleep(float(st.get("interval", 6)))
        if len(vals) < 3:
            raise StepFailed(f"only {len(vals)} readings")
        spread = max(vals) - min(vals)
        if spread > float(st.get("max_spread_c", 0.75)):
            raise StepFailed(f"readings spread {spread:.2f} C: not settled")
        mean = sum(vals) / len(vals)
        offset = round((ref - mean) * 100)
        if abs(offset) > float(st.get("max_offset_c", 5)) * 100:
            raise StepFailed(f"offset {offset / 100:+.2f} C is more than a calibration should be")
        out = self._cmd(box, f"config set cal_offset_c100 {offset}", quiet=1.0)
        if "stored" not in out:
            raise StepFailed("the offset was refused", out)
        self.db.set_value(box.mac, "cal_offset_c100", offset, run_id)
        self.db.set_value(box.mac, "cal_reference_c", ref, run_id)
        self.db.set_value(box.mac, "cal_date", dt.date.today().isoformat(), run_id)
        return StepResult(PASS, f"mean {mean:.2f} C vs {ref:.2f} C: offset {offset / 100:+.2f} C",
                          out)
