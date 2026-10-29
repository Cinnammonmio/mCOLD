"""The rack: boxes off the dock, on USB power anywhere in reach of the line's
Wi-Fi, finished over MQTT many at a time.

The dock's quick route (profile stage `quick`) leaves each box with its SN,
the line's Wi-Fi at the fixed address of its unit number, the broker and
the OTA base. From there the profile's `rack:` steps run here, one thread
per box, and land in the same record as the dock's (stage `rack`), so the
SN summary sees them. Questions to the person (is box 007 blinking?) wait
in one queue, oldest first.

Step types (profile `rack:`):

    online           the box is on the broker; its IP is its unit's, its firmware the one flashed
    status           fields of its retained status (GET_STATUS)
    rpc              a command on mcold/v1/<sn>/rpc, its reply checked
    identify_confirm IDENTIFY (lights + beep), then the person says yes / no / again
    debug_pull       GET_DEBUG and everything the box left on the broker, to records/debug/<sn>/
    config_doc       the site's settings as a settings document; waits for config/state
    ota              a firmware file name; waits for ota/state ok
    wifi_handover    the customer's networks in, the line's out: always the last step
"""
from __future__ import annotations

import datetime as dt
import json
import os
import threading
import time
from collections import deque
from dataclasses import dataclass, field

from . import firmware as fwlib
from . import profile as proflib
from .db import Db
from .mqttlink import Fleet, LinkError, NetBox
from .runner import FAIL, PASS, SKIP, StepFailed, StepResult
from .site import Site, mask

DEBUG_DIR = os.path.join(proflib.FLASHER, "records", "debug")


def dig(d: dict, path: str):
    """d["a"]["b"] for "a.b"; None when any part is missing."""
    for k in path.split("."):
        if not isinstance(d, dict) or k not in d:
            return None
        d = d[k]
    return d


def check_fields(d: dict, expect: dict | None = None, ranges: dict | None = None) -> list[str]:
    bad = []
    for k, want in (expect or {}).items():
        got = dig(d, k)
        if got != want:
            bad.append(f"{k} = {json.dumps(got, ensure_ascii=False)} (ต้องเป็น {json.dumps(want)})")
    if len(bad) > 1 and (expect or {}).get("ok") is True and dig(d, "ok") is not True:
        bad = [b for b in bad if not b.startswith("ok = ")]    # the overall verdict says nothing more
    for k, (lo, hi) in (ranges or {}).items():
        got = dig(d, k)
        if not isinstance(got, (int, float)) or isinstance(got, bool) or not lo <= got <= hi:
            bad.append(f"{k} = {got} (ต้องอยู่ใน {lo}..{hi})")
    return bad


@dataclass
class RackQuestion:
    id: int
    sn: str
    unit: int | None
    text: str
    repeat: bool = False
    asked: float = field(default_factory=time.time)
    answer: object = None
    done: threading.Event = field(default_factory=threading.Event)


class RackBox:
    def __init__(self, sn: str, mac: str = "", unit: int | None = None):
        self.sn, self.mac, self.unit = sn, mac, unit
        self.state = "offline"     # offline ready running ask pass fail stopped
        self.ip = ""
        self.step_idx = 0
        self.step_name = ""
        self.fail: dict | None = None
        self.started = self.finished = 0.0
        self.log: deque[str] = deque(maxlen=300)
        self.stop = threading.Event()
        self.thread: threading.Thread | None = None
        self.tried = False         # auto-run had its go
        self.rpc_ok = False        # the box answered a command over MQTT

    @property
    def busy(self) -> bool:
        return self.thread is not None and self.thread.is_alive()

    def say(self, msg: str) -> None:
        self.log.append(time.strftime("%H:%M:%S ") + msg)


class RemoteRunner:
    """The rack's steps on one box, recorded like the dock's."""

    def __init__(self, rack: "Rack", rb: RackBox, operator: str = ""):
        self.rack, self.rb = rack, rb
        self.p: proflib.Profile = rack.profile
        self.db: Db = rack.db
        self.site: Site = rack.site
        self.fleet: Fleet = rack.fleet
        self.operator = operator
        self._secrets = self.site.secrets()
        self.stop = rb.stop.is_set

    def net(self) -> NetBox:
        return self.fleet.box(self.rb.sn)

    def run(self) -> bool:
        rb, steps = self.rb, self.p.rack
        run_id = self.db.start_run(self.p.product, "rack", rb.mac, rb.sn,
                                   self.site.station or "rack", self.operator)
        ok = True
        for i, st in enumerate(steps):
            name = st["name"]
            if self.stop():
                self.db.add_step(run_id, i, name, st["type"], FAIL, "stopped by the operator", "")
                ok = False
                break
            rb.step_idx, rb.step_name = i, name
            if st.get("enabled", True) is False:
                res = StepResult(SKIP, "disabled in the profile")
            else:
                res = self._attempt(st, run_id)
            detail, out = mask(res.detail, self._secrets), mask(res.output, self._secrets)
            self.db.add_step(run_id, i, name, st["type"], res.result, detail, out)
            mark = {"PASS": "ok  ", "FAIL": "FAIL", "SKIP": "skip"}[res.result]
            rb.say(f"[{mark}] {name}" + (f" -- {detail}" if detail else ""))
            if res.result == FAIL and not st.get("optional"):
                rb.fail = {"step": name, "detail": detail, "help": st.get("help", "")}
                ok = False
                break
        rb.step_idx = len(steps) if ok else rb.step_idx
        self.db.finish_run(run_id, PASS if ok else FAIL, rb.mac, rb.sn)
        return ok

    def _attempt(self, st: dict, run_id: int) -> StepResult:
        tries = int(st.get("retries", 0)) + 1
        last = StepResult(FAIL, "not run")
        for n in range(tries):
            try:
                last = getattr(self, "step_" + st["type"])(st, run_id) or StepResult(PASS)
            except StepFailed as e:
                last = StepResult(FAIL, e.detail, e.output)
            except LinkError as e:
                last = StepResult(FAIL, f"broker: {e}")
            if last.result != FAIL or self.stop():
                return last
            if n + 1 < tries:
                time.sleep(float(st.get("retry_wait", 5)) * self.rack.time_scale)
        return last

    def _t(self, st: dict, default: float) -> float:
        return float(st.get("timeout", default)) * self.rack.time_scale

    def _rpc(self, cmd: str, args: dict | None, timeout: float) -> dict:
        r = self.fleet.rpc(self.rb.sn, cmd, args, timeout, self.stop)
        if r is None:
            if self.stop():
                raise StepFailed("stopped by the operator")
            raise StepFailed(f"{cmd}: ไม่มีคำตอบใน {timeout:.0f} s"
                             + ("" if self.rb.rpc_ok else
                                " (กล่องไม่เคยตอบคำสั่งผ่าน MQTT: firmware ยังไม่มี rpc หรือหลุดจาก broker)"))
        self.rb.rpc_ok = True
        if r.get("ok") is False and r.get("err"):
            raise StepFailed(f"{cmd}: {r['err']}", json.dumps(r, ensure_ascii=False))
        return r

    # ---- step types ----------------------------------------------------------

    def step_online(self, st: dict, run_id: int) -> StepResult:
        b = self.net()
        got = self.fleet.wait(lambda: b.online and b.status, self._t(st, 300), self.stop)
        if not got:
            raise StepFailed("ไม่ขึ้น broker (ไฟเลี้ยง? Wi-Fi โรงงาน? broker?)"
                             if b.online is None else "offline")
        bad = []
        want_ip = ""
        if st.get("ip") and self.site.factory.ip_base:
            u = self.p.unit_from_sn(self.rb.sn)
            want_ip = self.site.factory.ip_for(u) if u else ""
            if want_ip and b.ip() != want_ip:
                bad.append(f"IP {b.ip() or '-'} (เลขเครื่อง {u} ต้องเป็น {want_ip})")
        self.rb.ip = b.ip()
        kind = st.get("firmware")
        if kind:
            fid = self.db.values(self.rb.mac).get(f"firmware_{kind}", "") if self.rb.mac else ""
            want = fid.split("+", 1)[0]
            try:
                want = fwlib.resolve(self.p.product, kind, fid)["version"] if fid else want
            except fwlib.FirmwareError:
                pass
            if want and b.fw() != want:
                bad.append(f"firmware {b.fw() or '-'} (flash ไว้ {want})")
        out = json.dumps(b.status, ensure_ascii=False)
        if bad:
            raise StepFailed(", ".join(bad), out)
        return StepResult(PASS, f"{b.ip()} fw {b.fw()} rssi {dig(b.status, 'net.rssi')}", out)

    def step_status(self, st: dict, run_id: int) -> StepResult:
        b = self.net()
        bad = check_fields(b.status, st.get("expect"), st.get("range"))
        out = json.dumps(b.status, ensure_ascii=False)
        if bad:
            raise StepFailed(", ".join(bad), out)
        return StepResult(PASS, f"{dig(b.status, 'temp.c')} °C", out)

    def step_rpc(self, st: dict, run_id: int) -> StepResult:
        r = self._rpc(st["cmd"], st.get("args"), self._t(st, 30))
        out = json.dumps(r, ensure_ascii=False)
        bad = check_fields(r, st.get("expect"), st.get("range"))
        if bad:
            raise StepFailed(", ".join(bad), out)
        return StepResult(PASS, "", out)

    def step_identify_confirm(self, st: dict, run_id: int) -> StepResult:
        ctx = {"unit": f"{self.rb.unit:03d}" if self.rb.unit is not None else "?",
               "sn": self.rb.sn, "ip": self.rb.ip or self.net().ip() or "-"}
        text = st.get("ask", "กล่อง {unit} ไฟกะพริบและมีเสียง?").format(**ctx)
        for n in range(int(st.get("max_repeat", 10)) + 1):
            self._rpc("IDENTIFY", {"s": int(st.get("seconds", 10))}, self._t(st, 15))
            a = self.rack.ask(self.rb, text, repeat=True)
            if a is None:
                continue
            if not a:
                raise StepFailed("operator: no")
            return StepResult(PASS, f"after {n} repeats" if n else "")
        raise StepFailed("repeated too many times")

    def step_debug_pull(self, st: dict, run_id: int) -> StepResult:
        b = self.net()
        r = None
        try:
            r = self._rpc("GET_DEBUG", None, self._t(st, 15))
        except StepFailed:
            if st.get("need_rpc", True):
                raise
        with self.fleet.cond:
            doc = {"sn": self.rb.sn, "mac": self.rb.mac, "at": dt.datetime.now().astimezone().isoformat(),
                   "debug": r, "status": b.status, "settings": b.settings,
                   "config_state": b.config_state, "ota_state": b.ota_state}
        d = os.path.join(self.rack.debug_dir, self.rb.sn)
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, time.strftime("%Y%m%d-%H%M%S") + ".json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1, ensure_ascii=False)
        self.db.set_value(self.rb.mac or self.rb.sn, "debug_file", path, run_id)
        bad = check_fields(r or {}, st.get("expect"), st.get("range")) if r else []
        if bad:
            raise StepFailed(", ".join(bad), json.dumps(r, ensure_ascii=False))
        return StepResult(PASS, os.path.relpath(path, proflib.FLASHER), json.dumps(r or {}, ensure_ascii=False))

    def _next_rev(self) -> int:
        b = self.net()
        with self.fleet.cond:
            revs = [b.settings.get("server_rev"), b.settings.get("factory_rev"), b.config_state.get("rev")]
        revs.append(self.rack.last_rev.get(self.rb.sn))
        return max([r for r in revs if isinstance(r, int)] + [0]) + 1

    def _send_doc(self, doc: dict, timeout: float, gone_ok: bool = False) -> tuple[dict, bool]:
        """Publish a settings document and wait for config/state of its rev; with
        gone_ok, the box leaving the broker counts too (its network was removed)."""
        b = self.net()
        rev = self._next_rev()
        doc = {"rev": rev, **doc}
        if self.rb.rpc_ok:
            # the line's revisions, counted apart from the server's (docs/firmware-mqtt-rpc.md)
            doc["factory"] = True
        self.rack.last_rev[self.rb.sn] = rev
        if gone_ok and not b.online:
            raise StepFailed("กล่อง offline อยู่แล้ว ยืนยันการส่งมอบไม่ได้")
        sent = time.time()
        self.fleet.send_config(self.rb.sn, doc)

        def done():
            if b.config_state.get("rev") == rev:
                return "state"
            if gone_ok and b.online is False and b.online_at >= sent:
                return "gone"
            return None
        how = self.fleet.wait(done, timeout, self.stop)
        if not how:
            raise StepFailed(f"ไม่มี config/state ของ rev {rev} ใน {timeout:.0f} s")
        with self.fleet.cond:
            stt = dict(b.config_state) if how == "state" else {}
        if how == "state" and stt.get("state") != "ok":
            raise StepFailed(f"config {stt.get('state')}: {json.dumps(stt.get('errors'), ensure_ascii=False)}",
                             json.dumps(stt, ensure_ascii=False))
        return stt, how == "gone"

    def step_config_doc(self, st: dict, run_id: int) -> StepResult:
        doc = {}
        if self.site.config:
            doc["config"] = dict(self.site.config)
        if self.site.ota_base:
            doc["ota_base"] = self.site.ota_base
        if not doc:
            return StepResult(PASS, "ไม่มีค่าให้ส่ง")
        stt, _ = self._send_doc(doc, self._t(st, 120))
        return StepResult(PASS, f"rev {stt.get('rev')}: {len(stt.get('applied') or [])} ค่า",
                          json.dumps(stt, ensure_ascii=False))

    def step_ota(self, st: dict, run_id: int) -> StepResult:
        kind = st.get("firmware", "production")
        m = fwlib.resolve(self.p.product, kind, self.rack.fw_choice().get(kind, "approved"))
        b = self.net()
        if b.fw() == m["version"]:
            return StepResult(PASS, f"เป็น {m['version']} อยู่แล้ว")
        name = st.get("file", "mCOLD_{version}.bin").format(version=m["version"])
        self.fleet.send_firmware(self.rb.sn, name)

        def end():
            s = b.ota_state
            if s.get("state") in ("ok", "failed", "rolled_back", "skipped") and (
                    s.get("ver") == m["version"] or s.get("file") == name):
                return dict(s)
            return None
        s = self.fleet.wait(end, self._t(st, 600), self.stop)
        self.fleet.clear(self.rb.sn, "firmware")
        if not s:
            raise StepFailed(f"OTA {name}: ไม่จบใน {self._t(st, 600):.0f} s")
        if s["state"] != "ok":
            raise StepFailed(f"OTA {s['state']}: {s.get('reason', '')}", json.dumps(s))
        return StepResult(PASS, f"{s.get('from', '?')} → {m['version']}", json.dumps(s))

    def step_wifi_handover(self, st: dict, run_id: int) -> StepResult:
        """The customer's networks on, then the line's off. Once the second document is
        applied the box leaves the line's Wi-Fi, so it is always the last step."""
        f = self.site.factory
        out = []
        if self.rb.rpc_ok and st.get("factory_done", True):
            r = self._rpc("FACTORY_DONE", None, self._t(st, 15))
            out.append(json.dumps(r))
        add = [{"ssid": w.ssid, "pass": w.password, "dhcp": w.dhcp,
                **({} if w.dhcp else {"ip": w.ip, "gateway": w.gateway, "subnet": w.subnet,
                                      **({"dns": w.dns} if w.dns else {})})}
               for w in self.site.wifi if w.ssid != f.ssid]
        if add:
            stt, _ = self._send_doc({"wifi": {"add": add}}, self._t(st, 120))
            out.append(json.dumps(stt, ensure_ascii=False))
        how = "ไม่มี Wi-Fi โรงงานให้ลบ"
        if f.ssid:
            stt, gone = self._send_doc({"wifi": {"del": [f.ssid]}}, self._t(st, 120), gone_ok=True)
            out.append(json.dumps(stt, ensure_ascii=False) if stt else "(left the broker)")
            how = "ออกจาก Wi-Fi โรงงานแล้ว" + (" (ยืนยันจาก offline)" if gone else "")
        self.fleet.clear(self.rb.sn, "config")
        if self.rb.mac:
            self.db.del_value(self.rb.mac, "factory_ip")      # its address is free again
            self.db.set_value(self.rb.mac, "handover", dt.datetime.now().astimezone()
                              .isoformat(timespec="seconds"), run_id)
        return StepResult(PASS, f"{len(add)} เครือข่ายลูกค้า · {how}", "\n".join(out))


class Rack:
    """Every box of the lot that passed the quick route, wherever it is."""

    def __init__(self, station, fleet: Fleet, time_scale: float = 1.0,
                 debug_dir: str | None = None):
        self.st = station
        self.fleet = fleet
        self.time_scale = time_scale     # the demo runs its timeouts faster
        self.debug_dir = debug_dir or DEBUG_DIR
        self.boxes: dict[str, RackBox] = {}
        self.lock = threading.RLock()
        self.questions: list[RackQuestion] = []
        self._qid = 0
        self.auto = True                 # a box that comes online starts by itself
        self.operator = ""
        self.last_rev: dict[str, int] = {}
        self.counts = {"pass": 0, "fail": 0}
        fleet.on_change = lambda sn: self._seen(sn)

    # what the station knows, read each time (it may change product, lot or settings)
    @property
    def profile(self) -> proflib.Profile:
        return self.st.profile

    @property
    def db(self) -> Db:
        return self.st.db

    @property
    def site(self) -> Site:
        return self.st.site

    def fw_choice(self) -> dict:
        return dict(self.st.fw_choice)

    # ---- which boxes ---------------------------------------------------------

    def _lot_prefix(self) -> str:
        p, lot = self.profile, self.st.lot
        return p.make_sn(lot, 0)[:-int(p.sn.get("unit_digits", 3))] if p and lot else ""

    def refresh(self) -> None:
        """The lot's boxes that passed `quick` (the record) and are not handed over yet,
        and every box of the lot heard on the broker."""
        p = self.profile
        prefix = self._lot_prefix()
        if not p or not prefix:
            return
        rows = self.db.q("SELECT d.mac, d.sn FROM device d WHERE d.product=? AND d.sn LIKE ? AND "
                         "EXISTS (SELECT 1 FROM run r WHERE r.mac=d.mac AND r.stage='quick' AND r.result='PASS')",
                         (p.product, prefix + "%"))
        with self.lock:
            for r in rows:
                rb = self.boxes.get(r["sn"])
                if not rb:
                    rb = self.boxes[r["sn"]] = RackBox(r["sn"], r["mac"], p.unit_from_sn(r["sn"]))
                    last = self.db.q("SELECT result FROM run WHERE mac=? AND stage='rack' "
                                     "AND finished IS NOT NULL ORDER BY id DESC LIMIT 1", (r["mac"],))
                    if last:
                        rb.state = "pass" if last[0]["result"] == "PASS" else "fail"
                        rb.tried = True
                rb.mac = rb.mac or r["mac"]
            for sn in list(self.boxes):
                if not sn.startswith(prefix) and not self.boxes[sn].busy:
                    del self.boxes[sn]          # another lot
            for rb in self.boxes.values():
                self._update(rb)

    def _update(self, rb: RackBox) -> None:
        if rb.busy:
            return
        b = self.fleet.boxes.get(rb.sn)
        if b and b.status:
            rb.ip = b.ip() or rb.ip
        if rb.state in ("offline", "ready"):
            rb.state = "ready" if b and b.online else "offline"
        if self.auto and rb.state == "ready" and not rb.tried:
            rb.tried = True
            self.start(rb.sn)

    def _seen(self, sn: str) -> None:
        with self.lock:
            rb = self.boxes.get(sn)
        if rb:
            with self.lock:
                self._update(rb)
        elif self._lot_prefix() and sn.startswith(self._lot_prefix()):
            threading.Thread(target=self.refresh, daemon=True).start()

    def watch(self, every: float = 2.0) -> None:
        def loop():
            while True:
                try:
                    self.refresh()
                except Exception as e:
                    print(f"rack: {e!r}")
                time.sleep(every)
        threading.Thread(target=loop, daemon=True).start()

    # ---- running -------------------------------------------------------------

    def start(self, sn: str, operator: str = "") -> None:
        with self.lock:
            rb = self.boxes.get(sn)
            if not rb:
                raise RuntimeError(f"ไม่มีกล่อง {sn} บนชั้นวาง")
            if rb.busy:
                raise RuntimeError(f"{sn} กำลังทำงานอยู่")
            if not self.profile.rack:
                raise RuntimeError("profile ไม่มีขั้นชั้นวาง (rack:)")
            rb.stop.clear()
            rb.fail, rb.step_idx, rb.step_name = None, 0, ""
            rb.state, rb.started, rb.finished, rb.tried = "running", time.time(), 0.0, True
            rb.log.clear()
            rb.thread = threading.Thread(target=self._work, args=(rb, operator or self.operator),
                                         daemon=True)
            rb.thread.start()

    def start_all(self, operator: str = "") -> int:
        n = 0
        with self.lock:
            for rb in self.boxes.values():
                if rb.state in ("ready", "fail", "stopped") and not rb.busy:
                    self.start(rb.sn, operator)
                    n += 1
        return n

    def _work(self, rb: RackBox, operator: str) -> None:
        ok = False
        try:
            ok = RemoteRunner(self, rb, operator or "rack").run()
        except Exception as e:      # one box must not take the rack down
            rb.say(f"stopped: {e!r}")
            rb.fail = {"step": rb.step_name, "detail": repr(e), "help": ""}
        rb.finished = time.time()
        if rb.stop.is_set() and not ok:
            rb.state = "stopped"
        else:
            rb.state = "pass" if ok else "fail"
            self.counts["pass" if ok else "fail"] += 1

    def stop(self, sn: str) -> None:
        rb = self.boxes.get(sn)
        if rb:
            rb.stop.set()

    # ---- the person ----------------------------------------------------------

    def ask(self, rb: RackBox, text: str, repeat: bool = False) -> bool | None:
        with self.lock:
            self._qid += 1
            q = RackQuestion(self._qid, rb.sn, rb.unit, text, repeat)
            self.questions.append(q)
        rb.state = "ask"
        try:
            while not q.done.wait(0.2):
                if rb.stop.is_set():
                    raise StepFailed("stopped by the operator")
        finally:
            with self.lock:
                if q in self.questions:
                    self.questions.remove(q)
            rb.state = "running"
        if repeat and q.answer == "again":
            rb.say(f"? {text} -> ทำซ้ำ")
            return None
        rb.say(f"? {text} -> {'ใช่' if q.answer else 'ไม่'}")
        return bool(q.answer)

    def answer(self, qid: int, value) -> None:
        with self.lock:
            q = next((x for x in self.questions if x.id == qid), None)
        if not q or q.done.is_set():
            raise RuntimeError("คำถามนี้ตอบไปแล้ว")
        q.answer = value
        q.done.set()

    # ---- what the window shows -----------------------------------------------

    def state(self, log_sn: str = "") -> dict:
        steps = self.profile.rack if self.profile else []
        with self.lock:
            boxes = sorted(self.boxes.values(), key=lambda b: (b.unit is None, b.unit or 0, b.sn))
            qs = sorted(self.questions, key=lambda q: q.id)
        out = []
        for rb in boxes:
            nb = self.fleet.boxes.get(rb.sn)
            total = len(steps) or 1
            out.append({
                "sn": rb.sn, "mac": rb.mac, "unit": rb.unit, "ip": rb.ip, "state": rb.state,
                "online": bool(nb and nb.online), "fw": nb.fw() if nb else "",
                "step_idx": rb.step_idx, "step_name": rb.step_name,
                "progress": 1.0 if rb.state == "pass" else round(rb.step_idx / total, 3),
                "fail": rb.fail, "waiting": any(q.sn == rb.sn for q in qs),
                "elapsed": int((rb.finished or time.time()) - rb.started) if rb.started else 0,
                "log": list(rb.log)[-120:] if rb.sn == log_sn else None,
            })
        counts = {k: sum(1 for b in out if b["state"] == k) for k in
                  ("offline", "ready", "running", "ask", "pass", "fail", "stopped")}
        link = self.fleet.link
        return {
            "connected": bool(getattr(link, "connected", False)),
            "link_error": getattr(link, "error", ""),
            "lot": self.st.lot, "auto": self.auto,
            "steps": [s["name"] for s in steps],
            "questions": [{"id": q.id, "sn": q.sn, "unit": q.unit, "text": q.text, "repeat": q.repeat,
                           "waited": int(time.time() - q.asked)} for q in qs],
            "boxes": out, "counts": counts,
        }
