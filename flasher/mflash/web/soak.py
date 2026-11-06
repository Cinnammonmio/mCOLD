"""The long test: a box left running, before or after shipping, read every minute
until the person stops it -- then a summary of how its readings held up.

Only reads, so it works with factory mode on or off: the box's `status` (sent all
the time on broker 1) and `GET_DEBUG` on the rpc channel. Each reading is one row
of `soak_sample`; the run is stage `soak`, open while it runs. A run still open
when the web app starts again (a restart, a power cut) carries on by itself.

Findings, each a step of the run when it stops (FAIL = not normal):
    online     share of readings online; the longest time offline (> 5 min: FAIL)
    restarts   boot count up or uptime back to zero, with the reset reasons
    core dump  one that appeared during the test
    temperature readings that failed (any: FAIL); min / avg / max
    battery    %, voltage, current, charging states; external power lost
    Wi-Fi      signal min / avg / max
    memory     internal RAM, the lowest seen (< 10 KB: FAIL -- notes/firmware.md 13:40)
"""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field

OFFLINE_MAX_S = 5 * 60
INTERNAL_MIN = 10 * 1024


@dataclass
class SoakRun:
    sn: str
    mac: str
    run_id: int
    started: float
    stop: threading.Event = field(default_factory=threading.Event)
    thread: threading.Thread | None = None
    samples: int = 0
    last: dict = field(default_factory=dict)


def _num(v):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def _stats(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return None
    return min(xs), sum(xs) / len(xs), max(xs)


def _dur(s: float) -> str:
    s = int(s)
    d, h, m = s // 86400, s % 86400 // 3600, s % 3600 // 60
    if s < 3600:
        return f"{m} นาที {s % 60} วินาที" if m else f"{s} วินาที"
    return (f"{d} วัน " if d else "") + f"{h} ชม. {m} นาที"


def summarize(samples: list[dict], interval: float) -> list[tuple[str, str, str]]:
    """(name, PASS|FAIL, detail) for each finding, from the run's readings in time order."""
    out: list[tuple[str, str, str]] = []
    if not samples:
        return [("ข้อมูล", "FAIL", "ไม่มีการอ่านค่าเลย")]
    span = samples[-1]["at"] - samples[0]["at"] + interval
    out.append(("ระยะเวลา", "PASS", f"{_dur(span)} · อ่าน {len(samples)} ครั้ง ทุก {int(interval)} วินาที"))

    on = [s["online"] for s in samples]
    longest = cur = 0.0
    for i, s in enumerate(samples):
        if not s["online"]:
            cur += (samples[i + 1]["at"] - s["at"]) if i + 1 < len(samples) else interval
            longest = max(longest, cur)
        else:
            cur = 0.0
    pct = 100 * sum(on) / len(on)
    out.append(("online", "FAIL" if longest > OFFLINE_MAX_S else "PASS",
                f"{pct:.1f}% ของเวลา · หลุดนานสุด {int(longest // 60)} นาที {int(longest % 60)} วินาที"))

    dbg = [s["debug"] for s in samples if s["debug"]]
    restarts, reasons = 0, []
    for a, b in zip(dbg, dbg[1:]):
        if (_num(b.get("boot")) or 0) > (_num(a.get("boot")) or 0) or \
                (_num(b.get("uptime_s")) is not None and _num(a.get("uptime_s")) is not None
                 and b["uptime_s"] < a["uptime_s"]):
            restarts += 1
            reasons.append(str(b.get("reset", "?")))
    out.append(("รีสตาร์ท", "FAIL" if restarts else "PASS",
                f"{restarts} ครั้ง" + (f" ({', '.join(reasons)})" if reasons else "") if dbg else "ไม่มี GET_DEBUG"))
    new_dump = bool(dbg) and not dbg[0].get("coredump") and any(d.get("coredump") for d in dbg)
    out.append(("core dump", "FAIL" if new_dump else "PASS",
                "เกิดขึ้นระหว่างทดสอบ" if new_dump else ("มีมาก่อนเริ่ม" if dbg and dbg[0].get("coredump") else "ไม่มี")))

    st = [s["status"] for s in samples if s["status"]]
    temps = [(t.get("temp") or {}) for t in st]
    bad_t = sum(1 for t in temps if not t.get("ok"))
    ts = _stats([_num(t.get("c")) for t in temps if t.get("ok")])
    out.append(("อุณหภูมิ", "FAIL" if bad_t else "PASS",
                (f"{ts[0]:.2f} / {ts[1]:.2f} / {ts[2]:.2f} °C (ต่ำ/เฉลี่ย/สูง)" if ts else "ไม่มีค่า")
                + (f" · อ่านไม่ได้ {bad_t} ครั้ง" if bad_t else "")))

    pw = [(t.get("power") or {}) for t in st]
    soc, mv, ma = _stats([_num(p.get("soc")) for p in pw]), _stats([_num(p.get("mv")) for p in pw]), \
        _stats([_num(p.get("ma")) for p in pw])
    lost = sum(1 for a, b in zip(pw, pw[1:]) if a.get("external") and b.get("external") is False)
    charges = sorted({str(p.get("charge")) for p in pw if p.get("charge") is not None})
    out.append(("แบต / ชาร์จ", "PASS",
                (f"{soc[0]:.0f}–{soc[2]:.0f} %" if soc else "—") + " · "
                + (f"{mv[0] / 1000:.2f}–{mv[2] / 1000:.2f} V" if mv else "—") + " · "
                + (f"กระแสเฉลี่ย {ma[1]:+.0f} mA" if ma else "—")
                + (f" · {', '.join(charges)}" if charges else "")
                + (f" · ไฟเลี้ยงหลุด {lost} ครั้ง" if lost else "")))

    rs = _stats([_num((t.get("net") or {}).get("rssi")) for t in st])
    out.append(("สัญญาณ Wi-Fi", "PASS", f"{rs[0]:.0f} / {rs[1]:.0f} / {rs[2]:.0f} dBm" if rs else "ไม่มีค่า"))

    ints = [_num(d.get("min_internal")) or _num(d.get("internal")) for d in dbg]
    ints = [x for x in ints if x is not None]
    low = min(ints) if ints else None
    out.append(("RAM ภายใน", "FAIL" if low is not None and low < INTERNAL_MIN else "PASS",
                f"ต่ำสุด {low / 1024:.1f} KB" if low is not None else "ไม่มีค่า"))
    return out


class Soak:
    def __init__(self, line, interval: float = 60.0):
        self.line = line
        self.interval = interval
        self.runs: dict[str, SoakRun] = {}
        self.lock = threading.Lock()
        self._resume()

    def _resume(self) -> None:
        """Runs left open by a restart carry on."""
        for r in self.line.db.q("SELECT * FROM run WHERE stage='soak' AND finished IS NULL AND product=?",
                                (self.line.profile.product,)):
            if r["sn"]:
                first = self.line.db.q("SELECT MIN(at) AS t FROM soak_sample WHERE run_id=?", (r["id"],))
                run = SoakRun(r["sn"], r["mac"], r["id"], (first[0]["t"] if first and first[0]["t"] else time.time()))
                run.samples = self.line.db.q("SELECT COUNT(*) AS n FROM soak_sample WHERE run_id=?",
                                             (r["id"],))[0]["n"]
                self._go(run)

    def _go(self, run: SoakRun) -> None:
        with self.lock:
            self.runs[run.sn] = run
        run.thread = threading.Thread(target=self._loop, args=(run,), daemon=True)
        run.thread.start()

    def start(self, sn: str, operator: str = "") -> None:
        line = self.line
        if not line.fleet:
            raise RuntimeError(line.link_error or "ยังไม่ได้ต่อ MQTT broker")
        with self.lock:
            if sn in self.runs:
                raise RuntimeError(f"{sn} ทดสอบนานอยู่แล้ว")
        d = line.db.q("SELECT * FROM device WHERE product=? AND sn=?", (line.profile.product, sn))
        if not d:
            raise RuntimeError(f"ไม่รู้จักเครื่อง {sn}")
        run_id = line.db.start_run(line.profile.product, "soak", d[0]["mac"], sn,
                                   line.site.station or "web", operator or "web")
        self._go(SoakRun(sn, d[0]["mac"], run_id, time.time()))

    def _loop(self, run: SoakRun) -> None:
        while not run.stop.is_set():
            self.sample(run)
            run.stop.wait(self.interval)

    def sample(self, run: SoakRun) -> None:
        fleet = self.line.fleet
        nb = fleet.box(run.sn)
        debug = None
        if nb.online:
            debug = fleet.rpc(run.sn, "GET_DEBUG", None, timeout=min(10.0, self.interval), should_stop=run.stop.is_set)
            if debug and debug.get("ok") is False:
                debug = None
        with fleet.cond:
            status = dict(nb.status)
        online = bool(nb.online)
        self.line.db.add_sample(run.run_id, time.time(), online, status, debug)
        run.samples += 1
        t, p, n = status.get("temp") or {}, status.get("power") or {}, status.get("net") or {}
        run.last = {"online": online, "temp": t.get("c") if t.get("ok") else None, "soc": p.get("soc"),
                    "rssi": n.get("rssi"), "internal": (debug or {}).get("internal"),
                    "uptime_s": (debug or {}).get("uptime_s")}

    def stop(self, sn: str) -> dict:
        with self.lock:
            run = self.runs.pop(sn, None)
        if not run:
            raise RuntimeError(f"{sn} ไม่ได้ทดสอบนานอยู่")
        run.stop.set()
        if run.thread:
            run.thread.join(15)
        findings = summarize(self.line.db.samples(run.run_id), self.interval)
        db = self.line.db
        for i, (name, res, detail) in enumerate(findings):
            db.add_step(run.run_id, i, name, "soak", res, detail, "")
        result = "FAIL" if any(r == "FAIL" for _, r, _ in findings) else "PASS"
        db.finish_run(run.run_id, result, run.mac, run.sn)
        return {"sn": sn, "result": result, "findings": [{"name": n, "result": r, "detail": d}
                                                          for n, r, d in findings]}

    def state(self, sn: str) -> dict | None:
        """For the tile: running (with the latest reading and what is not normal so far), or
        the last finished run's result."""
        with self.lock:
            run = self.runs.get(sn)
        if run:
            live = summarize(self.line.db.samples(run.run_id), self.interval) if run.samples else []
            bad = [f"{n}: {d}" for n, r, d in live if r == "FAIL"]
            return {"running": True, "elapsed": int(time.time() - run.started), "samples": run.samples,
                    "last": run.last, "problems": bad}
        r = self.line.db.q("SELECT id, result, finished FROM run WHERE sn=? AND stage='soak' AND finished IS NOT NULL "
                           "ORDER BY id DESC LIMIT 1", (sn,))
        if not r:
            return None
        bad = [f"{s['name']}: {s['detail']}" for s in self.line.db.steps(r[0]["id"]) if s["result"] == "FAIL"]
        return {"running": False, "result": r[0]["result"], "finished": r[0]["finished"], "problems": bad}

    def series(self, sn: str) -> dict:
        """The readings of the running (or last) long test, for its charts."""
        with self.lock:
            run = self.runs.get(sn)
        rid = run.run_id if run else None
        if rid is None:
            r = self.line.db.q("SELECT id FROM run WHERE sn=? AND stage='soak' ORDER BY id DESC LIMIT 1", (sn,))
            rid = r[0]["id"] if r else None
        if rid is None:
            return {"samples": []}
        out = []
        for s in self.line.db.samples(rid):
            st, dg = s["status"], s["debug"] or {}
            out.append({"at": s["at"], "online": s["online"],
                        "temp": (st.get("temp") or {}).get("c") if (st.get("temp") or {}).get("ok") else None,
                        "soc": (st.get("power") or {}).get("soc"), "ma": (st.get("power") or {}).get("ma"),
                        "rssi": (st.get("net") or {}).get("rssi"), "internal": dg.get("internal"),
                        "uptime_s": dg.get("uptime_s")})
        return {"samples": out, "interval": self.interval}
