"""The six-slot station behind the window: what each slot holds, what it is
doing, and what it is waiting for the person to answer.

A slot is a physical place -- a dock bay, a cable on the bench -- and is
known by the USB location of that place, which stays the same while the
box in it changes port and PID with every restart. The runner itself
still follows the box by MAC and SN; the location only says which slot
to show it in.

Each running slot has its own thread. A question from a slot is shown in
that slot and waits for an answer there, so six boxes can each ask at
once without taking turns.
"""
from __future__ import annotations

import csv
import datetime as dt
import io
import json
import os
import threading
import time
from collections import deque
from dataclasses import dataclass, field

from . import firmware as fwlib
from . import profile as proflib
from .db import Db
from .ports import Port, UsbIds, norm_mac
from .runner import Box, Hardware, Runner, StepFailed
from .site import Site
from .ui import Ui

STAGE_LABELS = {"test": "ทดสอบโมดูล", "production": "ลง firmware จริง", "setup": "ตั้งค่า",
                "verify": "ตรวจก่อนส่ง"}


def stage_label(s: str) -> str:
    return STAGE_LABELS.get(s, s)


def location_key(p: Port) -> str:
    """The physical place: the USB location without the interface number,
    which differs between the ROM's port and the firmware's."""
    return p.location.split(":", 1)[0] if p.location else p.device


@dataclass
class Question:
    id: int
    kind: str                     # "yn" or "text"
    text: str
    default: str = ""
    answer: object = None
    done: threading.Event = field(default_factory=threading.Event)


class Slot:
    def __init__(self, index: int):
        self.index = index
        self.location = ""        # learned or saved
        self.port: Port | None = None
        self.state = "empty"      # empty ready running ask pass fail stopped
        self.box: Box | None = None
        self.stages: list[str] = []
        self.results: dict[str, str] = {}
        self.stage = ""
        self.step_idx = 0
        self.step_total = 0
        self.step_name = ""
        self.fail: dict | None = None
        self.question: Question | None = None
        self.log: deque[str] = deque(maxlen=400)
        self.stop = threading.Event()
        self.thread: threading.Thread | None = None
        self.started = 0.0
        self.finished = 0.0
        self.left = False         # the finished box has been taken out
        self.unit: int | None = None   # the unit number this slot gives its box
        self.unit_auto = True          # suggested by the station, not typed
        self.existing_sn = ""          # the box came with an SN already (a rework)

    def forget_unit(self) -> None:
        self.unit, self.unit_auto, self.existing_sn = None, True, ""

    @property
    def busy(self) -> bool:
        return self.thread is not None and self.thread.is_alive()

    def reset(self) -> None:
        self.box, self.stages, self.results = None, [], {}
        self.stage, self.step_idx, self.step_total, self.step_name = "", 0, 0, ""
        self.fail, self.question = None, None
        self.started = self.finished = 0.0
        self.left = False
        self.log.clear()
        self.stop.clear()


class SlotUi(Ui):
    """The runner's voice, routed to the slot it is about (the box label is the slot index)."""

    def __init__(self, station: "Station"):
        self.st = station
        self._qid = 0
        self._lock = threading.Lock()

    def _slot(self, who: str) -> Slot:
        return self.st.slots[int(who)]

    def info(self, who: str, msg: str) -> None:
        s = self._slot(who)
        for line in msg.splitlines() or [""]:
            s.log.append(time.strftime("%H:%M:%S ") + line)

    def _ask(self, who: str, kind: str, text: str, default: str = ""):
        s = self._slot(who)
        with self._lock:
            self._qid += 1
            q = Question(self._qid, kind, text, default)
        s.question, s.state = q, "ask"
        self.st.questions_asked += 1
        while not q.done.wait(0.2):
            if s.stop.is_set():
                s.question, s.state = None, "running"
                raise StepFailed("stopped by the operator")
        s.question, s.state = None, "running"
        return q.answer

    def yes_no(self, who: str, question: str) -> bool:
        a = bool(self._ask(who, "yn", question))
        self.info(who, f"  ? {question} -> {'ใช่' if a else 'ไม่'}")
        return a

    def text(self, who: str, question: str, default: str = "") -> str:
        a = str(self._ask(who, "text", question, default) or "")
        self.info(who, f"  ? {question} -> {a}")
        return a

    def stage_started(self, who: str, stage: str, steps: int) -> None:
        s = self._slot(who)
        s.stage, s.step_total, s.step_idx, s.step_name = stage, steps, 0, ""
        s.results[stage] = "running"

    def step_started(self, who: str, idx: int, name: str) -> None:
        s = self._slot(who)
        s.step_idx, s.step_name = idx, name

    def step_done(self, who: str, idx: int, name: str, result: str, detail: str, help: str) -> None:
        s = self._slot(who)
        s.step_idx = idx + 1
        if result == "FAIL":
            s.fail = {"stage": s.stage, "step": name, "detail": detail, "help": help}

    def stage_done(self, who: str, stage: str, result: str) -> None:
        self._slot(who).results[stage] = result


class Station:
    def __init__(self, product: str, db: Db, site: Site, hw: Hardware | None = None,
                 n_slots: int = 6, layout_path: str | None = None, demo: bool = False):
        self.db, self.site = db, site
        self.hw = hw or Hardware()
        self.demo = demo
        self.slots = [Slot(i) for i in range(n_slots)]
        self.ui = SlotUi(self)
        self.lock = threading.RLock()
        self.layout_path = layout_path or os.path.join(proflib.FLASHER, "records", "slots.json")
        self.stage_choice = "all"
        self.lot = ""
        self.counts = {"pass": 0, "fail": 0}
        self.questions_asked = 0
        self.profile: proflib.Profile | None = None
        self.profile_error = ""
        self._fw_cache: tuple[float, str, list] = (0.0, "", [])
        self.on_clear = None      # demo: a fresh simulated board in the slot
        self.set_product(product)
        self._load_layout()

    # ---- configuration --------------------------------------------------

    def set_product(self, product: str) -> None:
        with self.lock:
            if any(s.busy for s in self.slots):
                raise RuntimeError("มีช่องที่กำลังทำงานอยู่ เปลี่ยนสินค้าไม่ได้")
            try:
                self.profile = proflib.load(product)
                self.profile_error = ""
            except proflib.ProfileError as e:
                self.profile, self.profile_error = None, str(e)
            self.ids = UsbIds(self.profile.usb) if self.profile else None
            if self.profile and self.stage_choice != "all" and self.stage_choice not in self.profile.stages:
                self.stage_choice = "all"

    def set_stage(self, stage: str) -> None:
        if not self.profile:
            raise RuntimeError(self.profile_error)
        if stage != "all":
            for s in stage.split(","):
                self.profile.stage(s)
        self.stage_choice = stage

    def stages(self) -> list[str]:
        if not self.profile:
            return []
        return list(self.profile.sequence) if self.stage_choice == "all" else self.stage_choice.split(",")

    def firmware_status(self) -> list[dict]:
        key = f"{self.profile.product if self.profile else ''}/{self.stage_choice}"
        t, k, cached = self._fw_cache
        if k == key and time.monotonic() - t < 3:
            return cached
        out = self._firmware_status()
        self._fw_cache = (time.monotonic(), key, out)
        return out

    def _firmware_status(self) -> list[dict]:
        out = []
        if not self.profile:
            return out
        needed = []
        for s in self.stages():
            for st in self.profile.stage(s):
                k = st.get("firmware")
                if k and st["type"] in ("flash", "version") and k not in needed:
                    needed.append(k)
        for k in needed:
            try:
                m = fwlib.resolve(self.profile.product, k, "approved")
                out.append({"kind": k, "ok": True, "id": m["id"], "version": m["version"],
                            "by": m["approved"]["by"]})
            except fwlib.FirmwareError as e:
                out.append({"kind": k, "ok": False, "msg": str(e)})
        return out

    def _read_settings(self) -> dict:
        try:
            with open(self.layout_path, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def _write_settings(self, **kw) -> None:
        d = self._read_settings()
        d.update(kw)
        os.makedirs(os.path.dirname(os.path.abspath(self.layout_path)), exist_ok=True)
        with open(self.layout_path, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2, ensure_ascii=False)

    def _load_layout(self) -> None:
        d = self._read_settings()
        for s, loc in zip(self.slots, d.get("slots", [])):
            s.location = loc or ""
        lot = d.get("lot", "")
        if lot and self.profile and self.profile.lot_ok(lot):
            self.lot = lot

    def save_layout(self) -> str:
        self._write_settings(slots=[s.location for s in self.slots])
        return self.layout_path

    # ---- the lot and the unit numbers --------------------------------------

    def set_lot(self, lot: str) -> None:
        lot = lot.strip()
        if not self.profile:
            raise RuntimeError(self.profile_error)
        if lot and not self.profile.lot_ok(lot):
            raise RuntimeError(f"เลขล็อต '{lot}' ไม่ตรงรูปแบบ (เช่น {self.profile.sn.get('lot_example', '')})")
        with self.lock:
            if lot != self.lot:
                self.lot = lot
                for s in self.slots:
                    if not s.busy and s.state not in ("pass", "fail", "stopped"):
                        s.unit, s.unit_auto = None, True
                self._suggest_units()
        self._write_settings(lot=self.lot)

    def _used_units(self, but: Slot | None = None) -> dict[int, str]:
        """unit -> who has it: an SN in the record, or another slot."""
        used: dict[int, str] = {}
        if not self.lot or not self.profile:
            return used
        prefix = self.profile.make_sn(self.lot, 0)[:-int(self.profile.sn.get("unit_digits", 3))]
        for r in self.db.q("SELECT sn, mac FROM device WHERE product=? AND sn LIKE ?",
                           (self.profile.product, prefix + "%")):
            u = self.profile.unit_of(r["sn"], self.lot)
            if u is not None:
                used[u] = f"{r['sn']} (เครื่อง {r['mac']})"
        for o in self.slots:
            if o is not but and o.unit is not None:
                used.setdefault(o.unit, f"ช่อง {o.index + 1}")
            if o is not but and o.box and o.box.sn:
                u = self.profile.unit_of(o.box.sn, self.lot)
                if u is not None:
                    used.setdefault(u, f"ช่อง {o.index + 1}")
        return used

    def _suggest_units(self) -> None:
        """Every ready slot without a unit gets the smallest one nobody has."""
        if not self.lot or not self.profile:
            return
        for s in self.slots:
            if s.state == "ready" and s.unit is None and not s.existing_sn:
                used = self._used_units(but=s)
                n = next((u for u in range(1, self.profile.unit_max + 1) if u not in used), None)
                s.unit, s.unit_auto = n, True

    def set_unit(self, i: int, unit: int) -> None:
        if not self.profile:
            raise RuntimeError(self.profile_error)
        with self.lock:
            s = self.slots[i]
            if s.busy:
                raise RuntimeError(f"ช่อง {i + 1} กำลังทำงานอยู่")
            if not self.lot:
                raise RuntimeError("กรอกเลขล็อตก่อน")
            if not 1 <= unit <= self.profile.unit_max:
                raise RuntimeError(f"เลขเครื่องต้องอยู่ระหว่าง 1–{self.profile.unit_max}")
            who = self._used_units(but=s).get(unit)
            if who:
                raise RuntimeError(f"เลข {unit:0{int(self.profile.sn.get('unit_digits', 3))}d} ใช้แล้ว: {who}")
            s.unit, s.unit_auto = unit, False

    def step_unit(self, i: int, delta: int) -> None:
        """The next unit number nobody has, up (+1) or down (-1) from the slot's."""
        if not self.profile or not self.lot:
            raise RuntimeError("กรอกเลขล็อตก่อน")
        with self.lock:
            s = self.slots[i]
            used = self._used_units(but=s)
            u = (s.unit or 0) + delta
            while 1 <= u <= self.profile.unit_max and u in used:
                u += delta
            if not 1 <= u <= self.profile.unit_max:
                raise RuntimeError("ไม่มีเลขว่างทางนั้นแล้ว")
            self.set_unit(i, u)

    def planned_sn(self, s: Slot) -> str:
        if s.existing_sn:
            return s.existing_sn
        if self.lot and s.unit is not None and self.profile:
            return self.profile.make_sn(self.lot, s.unit)
        return ""

    def summary(self, lot: str | None = None) -> dict:
        """The lot so far: every SN, its box, its result, and the unit numbers not yet used."""
        if not self.profile:
            raise RuntimeError(self.profile_error)
        lot = self.lot if lot is None else lot.strip()
        p = self.profile
        prefix = p.make_sn(lot, 0)[:-int(p.sn.get("unit_digits", 3))] if lot else ""
        today = dt.datetime.now().astimezone().replace(hour=0, minute=0, second=0,
                                                       microsecond=0).isoformat(timespec="seconds")
        rows = self.db.summary(p.product, list(p.sequence), prefix=prefix, since=today)
        for r in rows:
            r["unit"] = p.unit_of(r["sn"], lot) if lot else None
            r["stage_labels"] = {k: stage_label(k) for k in r["stages"]}
            if r["fail"]:
                r["fail"]["stage_label"] = stage_label(r["fail"]["stage"])
        units = sorted(r["unit"] for r in rows if r["unit"] is not None)
        missing = [u for u in range(1, units[-1]) if u not in set(units)] if units else []
        counts = {k: sum(1 for r in rows if r["result"] == k) for k in ("PASS", "FAIL", "INCOMPLETE")}
        return {"lot": lot, "product": p.product, "rows": rows, "counts": counts, "missing": missing,
                "sequence": [{"name": x, "label": stage_label(x)} for x in p.sequence],
                "made": dt.datetime.now().astimezone().isoformat(timespec="seconds")}

    def summary_csv(self, lot: str | None = None) -> str:
        sm = self.summary(lot)
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow(["sn", "unit", "result", "mac", "firmware", *[s["name"] for s in sm["sequence"]],
                    "failed_stage", "failed_step", "failed_detail", "finished", "operator", "station",
                    "unapproved_firmware"])
        for r in sm["rows"]:
            f = r["fail"] or {}
            w.writerow([r["sn"], r["unit"] if r["unit"] is not None else "", r["result"], r["mac"],
                        r["firmware"], *[r["stages"].get(s["name"], "") for s in sm["sequence"]],
                        f.get("stage", ""), f.get("step", ""), f.get("detail", ""), r["finished"],
                        r["operator"], r["station"], "yes" if r["unapproved"] else ""])
        return buf.getvalue()

    def forget_layout(self) -> None:
        with self.lock:
            for s in self.slots:
                if not s.busy:
                    s.location = ""
        try:
            os.remove(self.layout_path)
        except OSError:
            pass

    # ---- the bus ----------------------------------------------------------

    def refresh(self) -> None:
        """Which slot each box on the bus is in; a new place goes to the first free slot."""
        if not self.ids:
            return
        ports = [p for p in self.hw.scan() if self.ids.ours(p)]
        by_loc: dict[str, Port] = {}
        for p in ports:
            by_loc.setdefault(location_key(p), p)
        with self.lock:
            known = {s.location for s in self.slots if s.location}
            for loc in sorted(by_loc):
                if loc not in known:
                    free = [s for s in self.slots if not s.location]
                    if free:
                        free[0].location = loc
                        known.add(loc)
            for s in self.slots:
                p = by_loc.get(s.location) if s.location else None
                s.port = p
                if s.busy:
                    continue
                if s.state in ("pass", "fail", "stopped"):
                    if not p:
                        s.left = True
                    elif s.left:             # a new box where the finished one was
                        s.reset()
                        s.forget_unit()
                        s.state = "ready"
                    continue
                s.state = "ready" if p else "empty"
                if not p:
                    s.forget_unit()
                elif self.profile and self.ids.is_app(p) and self.profile.sn_ok(p.serial):
                    s.existing_sn, s.unit = p.serial, None      # it has its SN already
                else:
                    s.existing_sn = ""
            self._suggest_units()

    def watch(self, every: float = 1.0) -> None:
        """Keep the slots up to date with the bus, from a thread of its own."""
        def loop():
            while True:
                try:
                    self.refresh()
                except Exception as e:   # a bus scan that fails once must not stop the watch
                    print(f"port scan: {e!r}")
                time.sleep(every)
        threading.Thread(target=loop, daemon=True).start()

    def port_kind(self, p: Port | None) -> str:
        if not p or not self.ids:
            return ""
        if self.ids.is_app(p):
            return "firmware"
        if self.ids.is_usj(p):
            return "usj"
        if self.ids.is_rom(p):
            return "rom"
        return ""

    # ---- running ------------------------------------------------------------

    def start(self, i: int, operator: str = "") -> None:
        with self.lock:
            s = self.slots[i]
            if not self.profile:
                raise RuntimeError(self.profile_error)
            if s.busy:
                raise RuntimeError(f"ช่อง {i + 1} กำลังทำงานอยู่")
            if not s.port:
                raise RuntimeError(f"ช่อง {i + 1} ไม่มีบอร์ด")
            if self.profile.incomplete:
                raise RuntimeError(f"profile {self.profile.product} ยังไม่ครบ")
            bad = [f for f in self.firmware_status() if not f["ok"]]
            if bad:
                raise RuntimeError(bad[0]["msg"])
            if s.unit is not None and not s.existing_sn:
                who = self._used_units(but=s).get(s.unit)
                if who:
                    raise RuntimeError(f"ช่อง {i + 1}: เลข {s.unit} ใช้แล้ว ({who}) เลือกเลขใหม่")
            s.reset()
            s.stages = self.stages()
            s.results = {st: "" for st in s.stages}
            s.state = "running"
            s.started = time.time()
            s.box = Box(label=str(i), port=s.port)
            s.thread = threading.Thread(target=self._work, args=(s, operator), daemon=True)
            s.thread.start()

    def start_all(self, operator: str = "") -> int:
        n = 0
        for s in self.slots:
            if s.port and not s.busy and s.state == "ready":
                self.start(s.index, operator)
                n += 1
        return n

    def _work(self, s: Slot, operator: str) -> None:
        r = Runner(self.profile, self.db, self.ui, self.site, hw=self.hw,
                   operator=operator or "station", station=self.site.station or "gui",
                   should_stop=s.stop.is_set, sn=self.planned_sn(s), keep_existing_sn=True)
        ok = False
        try:
            ok = r.run(s.box, s.stages)
        except Exception as e:      # a crash in one slot must not take the others down
            s.log.append(time.strftime("%H:%M:%S ") + f"stopped: {e!r}")
            s.fail = {"stage": s.stage, "step": s.step_name, "detail": repr(e), "help": ""}
        s.finished = time.time()
        if s.stop.is_set() and not ok:
            s.state = "stopped"
        else:
            s.state = "pass" if ok else "fail"
            self.counts["pass" if ok else "fail"] += 1

    def stop(self, i: int) -> None:
        self.slots[i].stop.set()

    def clear(self, i: int) -> None:
        with self.lock:
            s = self.slots[i]
            if s.busy:
                raise RuntimeError(f"ช่อง {i + 1} กำลังทำงานอยู่")
            s.reset()
            s.forget_unit()
            if self.on_clear:
                self.on_clear(i)
            s.state = "ready" if s.port else "empty"

    def answer(self, i: int, qid: int, value) -> None:
        q = self.slots[i].question
        if not q or q.id != qid:
            raise RuntimeError("คำถามนี้ตอบไปแล้ว")
        q.answer = value
        q.done.set()

    # ---- what the window shows -------------------------------------------

    def state(self, log_slots: set[int] | None = None) -> dict:
        slots = []
        for s in self.slots:
            box = s.box
            port = (box.port if box and box.port and s.busy else s.port)
            fw = ""
            if box and box.flashed:
                # what the box runs now, not everything it ran on the way
                m = box.flashed.get(box.running) or list(box.flashed.values())[-1]
                fw = m["version"] + (" (ทดสอบ)" if m.get("kind") == "test" else "")
            q = s.question
            slots.append({
                "index": s.index, "location": s.location, "state": s.state,
                "port": port.device if port else "", "port_kind": self.port_kind(port),
                "port_serial": port.serial if port else "",
                # what is known about the box, kept on show for as long as it is in the slot:
                # before a run the MAC comes from the chip's USB serial, the SN is the one
                # it will get (or already has)
                "mac": (box.mac if box and box.mac else
                        (norm_mac(port.serial) if port and self.ids.is_usj(port) and port.serial else "")),
                "sn": (box.sn if box and box.sn else self.planned_sn(s)),
                "sn_planned": not (box and box.sn) and not s.existing_sn and bool(self.planned_sn(s)),
                "unit": s.unit, "unit_auto": s.unit_auto, "existing_sn": s.existing_sn,
                "firmware": fw,
                # before a run, the stages it would go through, so the slot shows the plan
                "stages": [{"name": st, "label": stage_label(st), "result": s.results.get(st, "")}
                           for st in (s.stages or (self.stages() if s.state == "ready" else []))],
                "stage": s.stage, "stage_label": stage_label(s.stage),
                "step_idx": s.step_idx, "step_total": s.step_total, "step_name": s.step_name,
                "fail": s.fail, "stopping": s.stop.is_set() and s.busy,
                "question": {"id": q.id, "kind": q.kind, "text": q.text, "default": q.default}
                if q else None,
                "elapsed": int((s.finished or time.time()) - s.started) if s.started else 0,
                "progress": self._progress(s),
                "log": list(s.log)[-150:] if log_slots is None or s.index in log_slots else None,
            })
        p = self.profile
        return {
            "product": p.product if p else "", "product_name": p.name if p else "",
            "profile_error": self.profile_error,
            "incomplete": p.incomplete if p else [],
            "products": proflib.list_profiles(),
            "stage": self.stage_choice,
            "lot": self.lot,
            "lot_example": p.sn.get("lot_example", "") if p else "",
            "sn_example": p.sn.get("example", "") if p else "",
            "unit_digits": int(p.sn.get("unit_digits", 3)) if p else 3,
            "unit_max": p.unit_max if p else 999,
            "stage_options": ([{"value": "all", "label": f"ทุกขั้น ({len(p.sequence)} ขั้น)"}]
                + [{"value": x, "label": stage_label(x)} for x in p.stages]) if p else [],
            "firmware": self.firmware_status(),
            "busy": any(s.busy for s in self.slots),
            "counts": self.counts, "station": self.site.station, "demo": self.demo,
            "now": dt.datetime.now().strftime("%H:%M:%S"),
            "slots": slots,
        }

    def _progress(self, s: Slot) -> float:
        """Steps done over steps in the whole run, 0..1, for the slot's ring."""
        if not self.profile or not s.stages:
            return 0.0
        if s.state == "pass":
            return 1.0
        total = done = 0
        for st in s.stages:
            n = len(self.profile.stages.get(st) or [])
            total += n
            r = s.results.get(st, "")
            if r == "PASS":
                done += n
            elif st == s.stage and r in ("running", "FAIL"):
                done += min(s.step_idx, n)
        return round(done / total, 3) if total else 0.0

    def history(self, limit: int = 40) -> list[dict]:
        return [dict(r) for r in self.db.history("", limit)]
