"""The line's web app, the part that is not HTTP: devices of a lot, adding
one over USB (the browser flashes it), the tests over MQTT, the debug stream.

Adding a device, as the browser does it:

    1. the person plugs a box in and presses "เพิ่ม"; the browser connects
       (Web Serial + esptool-js) and reads the chip's MAC
    2. prepare(mac, lot, unit, wifi)  -> SN, and the images to write:
       the firmware's from the library and an NVS image with the box's
       settings (nvsimage.py), each behind a one-time token
    3. the browser writes them in one session and resets the box
    4. done(token, ok, log)           -> recorded as stage `usb`

The box then boots with its SN, Wi-Fi and broker and appears on the
broker; `line:` in the profile runs on it there (remote.Rack, stage `line`).
"""
from __future__ import annotations

import datetime as dt
import json
import os
import secrets
import threading
import time
from collections import deque
from dataclasses import dataclass, field

from .. import firmware as fwlib
from .. import nvsimage
from .. import profile as proflib
from ..core import LineCore, stage_label
from ..db import Db
from ..mqttlink import Fleet, Link
from ..ports import norm_mac
from ..remote import Rack
from ..site import Site

PREPARE_TTL_S = 15 * 60          # a prepared flash not finished by then is forgotten


@dataclass
class Prepared:
    token: str
    mac: str
    sn: str
    unit: int | None
    existing: bool                  # the box already had this SN in the record
    firmware: dict                  # the library manifest flashed
    images: dict[str, tuple[int, str | bytes]] = field(default_factory=dict)   # name -> (offset, path | data)
    wifi: list[str] = field(default_factory=list)
    factory_ip: str = ""
    erase_all: bool = True
    operator: str = ""
    at: float = field(default_factory=time.time)


class DebugHub:
    """The boxes' debug streams, from the debug broker: the last messages of each SN."""

    def __init__(self, link: Link, product: str = "mcold", keep: int = 600):
        self.link = link
        self.up = product
        self.lock = threading.Lock()
        self.seq = 0
        self.streams: dict[str, deque] = {}
        self.state: dict[str, dict] = {}
        link.subscribe(f"{product}/+/debug/#", self._got)

    def _got(self, topic: str, payload: bytes, retained: bool) -> None:
        parts = topic.split("/")
        sn, ch = parts[1], "/".join(parts[3:])
        try:
            d = json.loads(payload.decode("utf-8", "replace") or "null")
        except ValueError:
            d = {"text": payload.decode("utf-8", "replace")}
        with self.lock:
            if ch == "state":
                self.state[sn] = d if isinstance(d, dict) else {}
                return
            self.seq += 1
            self.streams.setdefault(sn, deque(maxlen=600)).append(
                {"seq": self.seq, "ch": ch, "at": time.time(), "data": d})

    def since(self, sn: str, seq: int) -> list[dict]:
        with self.lock:
            return [m for m in self.streams.get(sn, ()) if m["seq"] > seq]

    def is_on(self, sn: str) -> bool:
        with self.lock:
            return bool(self.state.get(sn, {}).get("on"))


class Line(LineCore):
    def __init__(self, product: str, db: Db, site: Site, fleet: Fleet | None = None,
                 debug: DebugHub | None = None, state_path: str | None = None,
                 time_scale: float = 1.0, debug_dir: str | None = None, demo: bool = False,
                 link_error: str = ""):
        self.db, self.site = db, site
        self.profile = proflib.load(product)
        self.state_path = (state_path or os.environ.get("MFLASH_WEB_STATE")
                           or os.path.join(proflib.FLASHER, "records", "web.json"))
        self.demo = demo
        self.link_error = link_error
        self.lot = ""
        self.fw = {"usb": "approved", "final": "approved"}   # library ids, or the newest approved
        self.lock = threading.RLock()
        self.prepared: dict[str, Prepared] = {}
        self.fleet = fleet
        self.debug = debug
        self._load()
        debug_dir = debug_dir or os.environ.get("MFLASH_DEBUG_DIR")
        self.rack = Rack(self, fleet, time_scale=time_scale, debug_dir=debug_dir, entry_stage="usb",
                         auto=self._auto, stage="line") if fleet else None
        # shipping: a box that passed `line:` stays on the line's Wi-Fi (tested and debugged at
        # the company) until "ส่งมอบ" runs `ship:` -- the customer's Wi-Fi in, the line's out
        self.shipper = Rack(self, fleet, time_scale=time_scale, debug_dir=debug_dir, entry_stage="line",
                            auto=False, stage="ship") if fleet and self.profile.ship else None

    # ---- what the station remembers between restarts --------------------------

    def _load(self) -> None:
        try:
            with open(self.state_path, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError):
            d = {}
        lot = d.get("lot", "")
        if lot and self.profile.lot_ok(lot):
            self.lot = lot
        self.fw.update({k: v for k, v in (d.get("firmware") or {}).items() if k in self.fw})
        self._auto = bool(d.get("auto", False))

    def _save(self) -> None:
        os.makedirs(os.path.dirname(os.path.abspath(self.state_path)), exist_ok=True)
        tmp = self.state_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"lot": self.lot, "firmware": self.fw,
                       "auto": bool(self.rack and self.rack.auto)}, f, indent=2, ensure_ascii=False)
        os.replace(tmp, self.state_path)

    # what remote.Rack reads from its station
    @property
    def fw_choice(self) -> dict:
        return {self.profile.web.get("firmware", "production"): self.fw["final"]}

    # ---- configuration ----------------------------------------------------------

    @property
    def kind(self) -> str:
        return self.profile.web.get("firmware", "production")

    def firmware(self, which: str) -> dict:
        """The library manifest for `usb` or `final`, approved only."""
        m = fwlib.resolve(self.profile.product, self.kind, self.fw[which])
        if not m.get("approved"):
            raise RuntimeError(f"firmware {m['id']} ยังไม่ได้อนุมัติ")
        return m

    def set_firmware(self, usb: str | None = None, final: str | None = None) -> None:
        for k, v in (("usb", usb), ("final", final)):
            if v is None:
                continue
            if v != "approved":
                m = fwlib.resolve(self.profile.product, self.kind, v)
                if not m.get("approved"):
                    raise RuntimeError(f"firmware {m['id']} ยังไม่ได้อนุมัติ เลือกไม่ได้")
                v = m["id"]
            self.fw[k] = v
        self._save()

    def import_zip(self, data: bytes, approve_by: str = "") -> dict:
        """A build uploaded from the page: a zip of the PlatformIO build folder (or of just
        its images), into the library; approved at once when a name is given."""
        import io
        import tempfile
        import zipfile
        if len(data) > 64 * 1024 * 1024:
            raise RuntimeError("ไฟล์ใหญ่เกิน 64 MB")
        try:
            z = zipfile.ZipFile(io.BytesIO(data))
        except zipfile.BadZipFile as e:
            raise RuntimeError("ไม่ใช่ไฟล์ zip") from e
        want = {os.path.basename(i["file"]) for i in self.profile.firmware[self.kind].images}
        with tempfile.TemporaryDirectory() as d:
            found = set()
            for info in z.infolist():
                name = os.path.basename(info.filename)
                if name in want and not info.is_dir():      # by its base name: no path from the zip is used
                    with z.open(info) as src, open(os.path.join(d, name), "wb") as dst:
                        dst.write(src.read())
                    found.add(name)
            if want - found:
                raise RuntimeError(f"zip ไม่มี {', '.join(sorted(want - found))}")
            m = fwlib.import_build(self.profile, self.kind, d, {"upload": "web"})
        if approve_by.strip() and not m.get("approved"):
            m = fwlib.approve(self.profile.product, self.kind, m["id"], approve_by.strip())
        return {"id": m["id"], "version": m["version"], "approved": bool(m.get("approved")),
                "signed": m.get("signed", False)}

    def approve(self, fw_id: str, by: str) -> dict:
        if not by.strip():
            raise RuntimeError("ใส่ชื่อผู้อนุมัติ")
        m = fwlib.approve(self.profile.product, self.kind, fw_id, by.strip())
        return {"id": m["id"], "approved": True}

    def library(self) -> list[dict]:
        return [{"id": m["id"], "version": m["version"], "approved": bool(m.get("approved")),
                 "by": (m.get("approved") or {}).get("by", ""), "signed": m.get("signed", False),
                 "built": m.get("built", "")}
                for m in reversed(fwlib.list_firmware(self.profile.product, self.kind))]

    def set_lot(self, lot: str) -> None:
        lot = lot.strip()
        if lot and not self.profile.lot_ok(lot):
            raise RuntimeError(f"เลขล็อต '{lot}' ไม่ตรงรูปแบบ (เช่น {self.profile.sn.get('lot_example', '')})")
        self.lot = lot
        self._save()

    def set_auto(self, on: bool, operator: str = "") -> None:
        if self.rack:
            self.rack.auto = bool(on)
            self.rack.operator = operator
        self._save()

    def wifi_options(self) -> list[dict]:
        f = self.site.factory
        out = []
        if f.ssid:
            out.append({"ssid": f.ssid, "kind": "line", "fixed_ip": bool(f.ip_base),
                        "label": "Wi-Fi โรงงาน" + (" (IP ตามเลขเครื่อง)" if f.ip_base else "")})
        for w in self.site.wifi:
            if w.ssid != f.ssid:
                out.append({"ssid": w.ssid, "kind": "customer", "fixed_ip": not w.dhcp,
                            "label": "Wi-Fi ลูกค้า"})
        return out

    def config(self) -> dict:
        p = self.profile
        opts = [{"id": m["id"], "version": m["version"], "by": m["approved"]["by"],
                 "signed": m.get("signed", False)}
                for m in reversed(fwlib.list_firmware(p.product, self.kind)) if m.get("approved")]
        chosen = {}
        for k in ("usb", "final"):
            try:
                m = self.firmware(k)
                chosen[k] = {"ok": True, "id": m["id"], "version": m["version"], "which": self.fw[k]}
            except (fwlib.FirmwareError, RuntimeError) as e:
                chosen[k] = {"ok": False, "msg": str(e), "which": self.fw[k]}
        return {
            "product": p.product, "product_name": p.name, "station": self.site.station,
            "lot": self.lot, "lot_example": p.sn.get("lot_example", ""),
            "sn_example": p.sn.get("example", ""), "unit_digits": int(p.sn.get("unit_digits", 3)),
            "unit_max": p.unit_max, "next_unit": self.next_unit(),
            "wifi": self.wifi_options(), "firmware": chosen, "firmware_options": opts,
            "erase_all": bool(p.web.get("erase_all", True)),
            "flash": {"mode": p.flash.get("mode", "dio"), "freq": p.flash.get("freq", "80m"),
                      "size": p.flash.get("size", "16MB")},
            "usb": {"vid": int(p.usb["vid"]), "rom_pids": [int(x) for x in p.usb.get("rom_pids", [])],
                    "app_pids": [int(x) for x in p.usb.get("app_pids", [])],
                    "baud": int(p.console.get("baud", 115200)),
                    "download_command": p.console.get("download_command", "flash")},
            "line_steps": [s["name"] for s in p.line],
            "broker": {"connected": bool(self.fleet and getattr(self.fleet.link, "connected", False)),
                       "error": self.link_error or (getattr(self.fleet.link, "error", "") if self.fleet else "")},
            "debug_available": self.debug is not None,
            "auto": bool(self.rack and self.rack.auto),
            "demo": self.demo,
        }

    # ---- unit numbers ---------------------------------------------------------

    def _prefix(self, lot: str) -> str:
        return self.profile.make_sn(lot, 0)[:-int(self.profile.sn.get("unit_digits", 3))]

    def used_units(self, lot: str | None = None) -> dict[int, str]:
        lot = self.lot if lot is None else lot
        used: dict[int, str] = {}
        if not lot:
            return used
        for r in self.db.q("SELECT sn, mac FROM device WHERE product=? AND sn LIKE ?",
                           (self.profile.product, self._prefix(lot) + "%")):
            u = self.profile.unit_of(r["sn"], lot)
            if u is not None:
                used[u] = f"{r['sn']} (เครื่อง {r['mac']})"
        with self.lock:
            for p in self.prepared.values():
                u = self.profile.unit_of(p.sn, lot)
                if u is not None:
                    used.setdefault(u, f"{p.sn} (กำลัง flash)")
        return used

    def next_unit(self, lot: str | None = None, after: int = 0) -> int | None:
        used = self.used_units(lot)
        return next((u for u in range(max(after, 0) + 1, self.profile.unit_max + 1) if u not in used), None)

    # ---- adding a device over USB ---------------------------------------------

    def _expire(self) -> None:
        now = time.time()
        with self.lock:
            for t in [t for t, p in self.prepared.items() if now - p.at > PREPARE_TTL_S]:
                del self.prepared[t]

    def prepare(self, mac: str, lot: str, unit: int | None, wifi: list[str],
                erase_all: bool | None = None, operator: str = "", chip: str = "") -> dict:
        """The SN for the box with this MAC and the images to write it with."""
        self._expire()
        p = self.profile
        mac = norm_mac(mac)
        if len(mac) != 17:
            raise RuntimeError(f"MAC '{mac}' อ่านไม่ได้")
        lot = (lot or self.lot).strip()
        if not p.lot_ok(lot):
            raise RuntimeError(f"เลขล็อต '{lot}' ไม่ตรงรูปแบบ (เช่น {p.sn.get('lot_example', '')})")
        known = self.db.device(mac)
        with self.lock:
            for t, pr in list(self.prepared.items()):
                if pr.mac == mac:
                    del self.prepared[t]          # the same box again: the last attempt is over
            existing = bool(known and known["sn"] and p.sn_ok(known["sn"]))
            if existing:
                # a box already made keeps its SN, whatever number was in the form
                sn = known["sn"]
                unit = p.unit_from_sn(sn)
            else:
                if unit is None:
                    unit = self.next_unit(lot)
                if unit is None or not 1 <= int(unit) <= p.unit_max:
                    raise RuntimeError(f"เลขเครื่องต้องอยู่ระหว่าง 1–{p.unit_max}")
                unit = int(unit)
                who = self.used_units(lot).get(unit)
                if who:
                    raise RuntimeError(f"เลข {unit:0{int(p.sn.get('unit_digits', 3))}d} ใช้แล้ว: {who}")
                sn = p.make_sn(lot, unit)
            names = {w["ssid"] for w in self.wifi_options()}
            bad = [w for w in wifi if w not in names]
            if bad:
                raise RuntimeError(f"ไม่มี Wi-Fi {', '.join(bad)} ในหน้าตั้งค่า")
            if not wifi:
                raise RuntimeError("เลือก Wi-Fi อย่างน้อย 1 เครือข่าย (กล่องต้องต่อ broker ได้เพื่อทดสอบ)")
            if len(wifi) > nvsimage.NET_MAX:
                raise RuntimeError(f"กล่องจำ Wi-Fi ได้ {nvsimage.NET_MAX} เครือข่าย เลือกไว้ {len(wifi)}")
            if not self.site.mqtt.get("host"):
                raise RuntimeError("ยังไม่ได้ตั้ง MQTT broker ในหน้าตั้งค่า")
            settings = nvsimage.from_site(self.site, sn, wifi=wifi,
                                          factory=bool(p.web.get("factory_mode", True)), unit=unit)
            f = self.site.factory
            factory_ip = ""
            if f.ssid in wifi and f.ip_base:
                if not unit or unit > f.capacity():
                    raise RuntimeError(f"เลขเครื่อง {unit} เกิน IP pool ({f.capacity()} เครื่อง)")
                factory_ip = f.ip_for(unit)
                for r in self.db.q("SELECT v.mac, d.sn FROM value v LEFT JOIN device d ON d.mac=v.mac "
                                   "WHERE v.key='factory_ip' AND v.val=? AND v.mac<>?", (factory_ip, mac)):
                    raise RuntimeError(f"IP {factory_ip} ยังเป็นของกล่อง {r['sn'] or r['mac']} ที่ยังไม่ส่งมอบ Wi-Fi")
            m = self.firmware("usb")
            images: dict[str, tuple[int, str | bytes]] = {}
            for off, path in fwlib.verify_files(m):
                images[os.path.basename(path)] = (off, path)
            nv = p.web.get("nvs") or {}
            images["nvs.bin"] = (int(nv.get("offset", nvsimage.NVS_OFFSET)),
                                 nvsimage.build(settings, int(nv.get("size", nvsimage.NVS_SIZE))))
            token = secrets.token_urlsafe(18)
            pr = Prepared(token=token, mac=mac, sn=sn, unit=unit, existing=existing, firmware=m,
                          images=images, wifi=list(wifi), factory_ip=factory_ip,
                          erase_all=bool(p.web.get("erase_all", True)) if erase_all is None else bool(erase_all),
                          operator=operator)
            self.prepared[token] = pr
        self.db.seen(mac, p.product, chip)
        order = sorted(images.items(), key=lambda kv: kv[1][0])
        return {"token": token, "sn": sn, "unit": unit, "existing": existing, "mac": mac,
                "firmware": {"id": m["id"], "version": m["version"]},
                "erase_all": pr.erase_all, "factory_ip": factory_ip,
                "images": [{"name": n, "offset": off,
                            "size": len(d) if isinstance(d, bytes) else os.path.getsize(d),
                            "url": f"/api/usb/image/{token}/{n}"} for n, (off, d) in order]}

    def image(self, token: str, name: str) -> bytes:
        with self.lock:
            pr = self.prepared.get(token)
        if not pr or name not in pr.images:
            raise KeyError("ไม่มีไฟล์นี้ (หมดเวลาแล้ว? กดเพิ่มใหม่)")
        data = pr.images[name][1]
        if isinstance(data, bytes):
            return data
        with open(data, "rb") as f:
            return f.read()

    def done(self, token: str, ok: bool, log: str = "", error: str = "") -> dict:
        """The browser's word on a flash: recorded as stage `usb` of the box."""
        with self.lock:
            pr = self.prepared.pop(token, None)
        if not pr:
            raise RuntimeError("ไม่รู้จักงาน flash นี้ (หมดเวลา หรือส่งซ้ำ)")
        p = self.profile
        run_id = self.db.start_run(p.product, "usb", pr.mac, pr.sn, self.site.station or "web",
                                   pr.operator or "web", firmware=f"{self.kind} {pr.firmware['id']}")
        detail = (f"{pr.firmware['version']} + NVS ({', '.join(pr.wifi)})"
                  + (" · ลบทั้งชิป" if pr.erase_all else "")) if ok else (error or "ไม่สำเร็จ")
        self.db.add_step(run_id, 0, "Flash firmware + ตั้งค่า (เบราว์เซอร์)", "web_usb",
                         "PASS" if ok else "FAIL", detail[:500], self._mask(log)[-20000:])
        if ok:
            if not pr.existing:
                self.db.set_sn(pr.mac, pr.sn)
            self.db.set_value(pr.mac, f"firmware_{self.kind}", pr.firmware["id"], run_id)
            self.db.set_value(pr.mac, "wifi", json.dumps(pr.wifi), run_id)
            if pr.factory_ip:
                self.db.set_value(pr.mac, "factory_ip", pr.factory_ip, run_id)
        self.db.finish_run(run_id, "PASS" if ok else "FAIL", pr.mac, pr.sn if ok else "")
        if ok and self.rack:
            threading.Thread(target=self.rack.refresh, daemon=True).start()
        return {"sn": pr.sn, "mac": pr.mac, "ok": ok,
                "next_unit": self.next_unit(after=pr.unit or 0) if not pr.existing else self.next_unit()}

    def _mask(self, s: str) -> str:
        from ..site import mask
        return mask(s or "", self.site.secrets())

    # ---- the devices page ---------------------------------------------------------

    def devices(self, lot: str | None = None, log_sn: str = "") -> dict:
        lot = self.lot if lot is None else lot
        p = self.profile
        rack = self.rack.state(log_sn=log_sn) if self.rack else {"boxes": [], "questions": []}
        ship = self.shipper.state(log_sn=log_sn) if self.shipper else {"boxes": []}
        for r in (self.rack, self.shipper):
            if r:
                r.refresh()
        by_sn = {b["sn"]: b for b in rack["boxes"]}
        ship_sn = {b["sn"]: b for b in ship["boxes"]}
        rows = []
        if lot:
            devs = self.db.q("SELECT * FROM device WHERE product=? AND sn LIKE ? ORDER BY sn",
                             (p.product, self._prefix(lot) + "%"))
        else:
            devs = self.db.q("SELECT * FROM device WHERE product=? AND sn IS NOT NULL ORDER BY sn",
                             (p.product,))
        for d in devs:
            vals = self.db.values(d["mac"])
            usb = self._last(d["mac"], "usb")
            line = self._last(d["mac"], "line")
            shipped = self._last(d["mac"], "ship")
            b = by_sn.get(d["sn"]) or {}
            sb = ship_sn.get(d["sn"]) or {}
            nb = self.fleet.boxes.get(d["sn"]) if self.fleet else None
            if b.get("state") in ("running", "ask"):
                state = b["state"]
            elif sb.get("state") in ("running", "ask"):
                state, b = "shipping", sb
            elif line and line["result"] == "PASS" and shipped and shipped["id"] > line["id"]:
                state = "shipped" if shipped["result"] == "PASS" else "ship_fail"
                if state == "ship_fail":
                    b = sb or b
            elif line and line["result"]:
                state = "pass" if line["result"] == "PASS" else "fail"
                if b.get("state") == "stopped":
                    state = "stopped"
            elif usb and usb["result"] == "FAIL":
                state = "usb_fail"
            elif usb:
                state = "online" if nb and nb.online else "waiting"
            else:
                state = "other"           # made by the dock, not by this page
            fail = b.get("fail") if state not in ("pass", "shipped") else None
            if not fail and state in ("fail", "ship_fail") and line:
                bad = self.db.q("SELECT name, detail FROM step WHERE run_id=? AND result='FAIL' "
                                "ORDER BY idx DESC LIMIT 1", ((shipped if state == "ship_fail" else line)["id"],))
                fail = {"step": bad[0]["name"], "detail": bad[0]["detail"]} if bad else None
            if not fail and state == "usb_fail":
                bad = self.db.q("SELECT name, detail FROM step WHERE run_id=? ORDER BY idx DESC LIMIT 1",
                                (usb["id"],))
                fail = {"step": "Flash ผ่าน USB", "detail": bad[0]["detail"] if bad else ""}
            rows.append({
                "sn": d["sn"], "mac": d["mac"], "unit": p.unit_from_sn(d["sn"] or ""),
                "state": state, "online": bool(nb and nb.online),
                "ip": (nb.ip() if nb else "") or b.get("ip") or vals.get("factory_ip", ""),
                "fw": (nb.fw() if nb else "") or vals.get(f"firmware_{self.kind}", "").split("+", 1)[0],
                "progress": 1.0 if state in ("pass", "shipped") else b.get("progress", 0.0),
                "step_name": b.get("step_name", ""), "step_idx": b.get("step_idx", 0),
                "fail": fail, "waiting": b.get("waiting", False),
                "elapsed": b.get("elapsed", 0), "log": b.get("log"),
                "debug": self.debug_on(d["sn"]),
                "usb_at": usb["finished"] if usb else "", "line_at": line["finished"] if line else "",
                "ship_at": shipped["finished"] if shipped and state == "shipped" else "",
                "handover": vals.get("handover", ""), "debug_file": bool(vals.get("debug_file")),
            })
        with self.lock:
            flashing = [{"sn": pr.sn, "mac": pr.mac, "unit": pr.unit, "state": "flashing"}
                        for pr in self.prepared.values()
                        if not lot or pr.sn.startswith(self._prefix(lot))]
        counts = {}
        for r in rows:
            counts[r["state"]] = counts.get(r["state"], 0) + 1
        return {"lot": lot, "rows": rows, "flashing": flashing, "questions": rack["questions"],
                "counts": counts, "steps": [s["name"] for s in p.line],
                "broker": {"connected": rack.get("connected", False), "error": rack.get("link_error", "")
                           or self.link_error},
                "auto": bool(self.rack and self.rack.auto),
                "now": dt.datetime.now().strftime("%H:%M:%S")}

    def _last(self, mac: str, stage: str):
        r = self.db.q("SELECT * FROM run WHERE mac=? AND stage=? AND finished IS NOT NULL "
                      "ORDER BY id DESC LIMIT 1", (mac, stage))
        return r[0] if r else None

    def device_log(self, mac: str) -> list[dict]:
        out = []
        for r in self.db.q("SELECT * FROM run WHERE mac=? ORDER BY id", (mac,)):
            out.append({"id": r["id"], "stage": r["stage"], "stage_label": stage_label(r["stage"]),
                        "result": r["result"] or "…", "started": r["started"], "finished": r["finished"],
                        "firmware": r["firmware"], "operator": r["operator"],
                        "steps": [{"name": s["name"], "result": s["result"], "detail": s["detail"]}
                                  for s in self.db.steps(r["id"])]})
        return out

    # ---- the tests --------------------------------------------------------------

    def _need_rack(self) -> Rack:
        if not self.rack:
            raise RuntimeError(self.link_error or "ยังไม่ได้ต่อ MQTT broker")
        return self.rack

    def start(self, sns: list[str] | None = None, operator: str = "") -> int:
        rack = self._need_rack()
        rack.refresh()
        if not sns:
            return rack.start_all(operator)
        for sn in sns:
            rack.start(sn, operator)
        return len(sns)

    def stop(self, sn: str) -> None:
        self._need_rack().stop(sn)
        if self.shipper:
            self.shipper.stop(sn)

    def ship(self, sns: list[str] | None = None, operator: str = "") -> int:
        """Hand boxes that passed over to the customer's Wi-Fi: those named, or every one
        that passed, is online and is not shipped yet."""
        if not self.shipper:
            raise RuntimeError(self.link_error or "ยังไม่ได้ต่อ MQTT broker หรือ profile ไม่มีขั้น ship:")
        self.shipper.refresh()
        if not sns:
            return self.shipper.start_all(operator)
        for sn in sns:
            rb = self.shipper.boxes.get(sn)
            if not rb:
                raise RuntimeError(f"{sn} ยังไม่ผ่านการทดสอบ ส่งมอบไม่ได้")
            self.shipper.start(sn, operator)
        return len(sns)

    def answer(self, qid: int, value) -> None:
        self._need_rack().answer(qid, value)

    def debug_on(self, sn: str) -> bool:
        b = self.fleet.boxes.get(sn) if self.fleet else None
        return bool(b and b.debug_state.get("on"))

    def set_debug(self, sn: str, on: bool, minutes: int = 60) -> dict:
        """The box's debug mode: switched by client 1 (`debug/ctl` on the production broker);
        its stream comes on client 2, to the debug broker (notes/firmware.md, 2026-11-02)."""
        if not self.fleet:
            raise RuntimeError(self.link_error or "ยังไม่ได้ต่อ MQTT broker")
        if on and not self.debug:
            raise RuntimeError("ยังไม่ได้ตั้ง debug broker ในหน้าตั้งค่า")
        b = self.fleet.box(sn)
        sent = time.time()
        self.fleet.debug_ctl(sn, on, minutes)
        st = self.fleet.wait(lambda: b.debug_state if b.debug_state.get("on") == bool(on)
                             and b.seen_at >= sent else None, 10)
        if not st:
            raise RuntimeError(f"{sn} ไม่ตอบ debug/ctl (offline หรือ firmware ยังไม่มี debug mode)")
        why = {"not set": "กล่องไม่มี debug broker (dbg/* ใน NVS: เพิ่มเครื่องใหม่หลังตั้ง debug broker)",
               "unreachable": "กล่องต่อ debug broker ไม่ได้"}
        if on and st.get("broker") not in (None, "connected", "connecting"):
            raise RuntimeError(f"{sn}: {why.get(st.get('broker'), 'debug broker ' + str(st.get('broker')))}")
        return dict(st)

    def debug_since(self, sn: str, seq: int) -> dict:
        """What the debug view shows: the status the box sends all the time on broker 1
        (every 5 min, every 30 s in factory mode), and the stream since `seq` when it is on."""
        nb = self.fleet.boxes.get(sn) if self.fleet else None
        base = {"status": dict(nb.status) if nb else {}, "status_at": nb.status_at if nb else 0,
                "online": bool(nb and nb.online), "state": dict(nb.debug_state) if nb else {}}
        if not self.debug:
            return {"on": False, "messages": [], **base}
        return {"on": self.debug_on(sn), "messages": self.debug.since(sn, seq), **base}

    def get_debug(self, sn: str) -> dict:
        """GET_DEBUG over the rpc channel: it only reads, so a box answers it in or out of factory mode."""
        if not self.fleet:
            raise RuntimeError(self.link_error or "ยังไม่ได้ต่อ MQTT broker")
        r = self.fleet.rpc(sn, "GET_DEBUG", None, timeout=10)
        if r is None:
            raise RuntimeError(f"{sn} ไม่ตอบ GET_DEBUG (offline?)")
        if r.get("ok") is False:
            raise RuntimeError(f"{sn}: {r.get('err', '')}")
        return r

    def delete(self, mac: str, by: str = "") -> dict:
        with self.lock:
            if self.rack:
                for rb in self.rack.boxes.values():
                    if rb.mac == mac and rb.busy:
                        raise RuntimeError("กล่องนี้กำลังทดสอบอยู่ ลบไม่ได้")
            r = self.db.delete_device(mac, by=by, reason="web")
            if not r:
                raise RuntimeError("ไม่มีบันทึกของกล่องนี้แล้ว")
            if self.rack and r.get("sn"):
                self.rack.boxes.pop(r["sn"], None)
            return r

    def history(self, limit: int = 60) -> list[dict]:
        return [dict(r) for r in self.db.history("", limit)]
