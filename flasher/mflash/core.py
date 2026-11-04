"""What the station and the line's web app share: the site's settings page
and the SN summary. A subclass gives `profile`, `site`, `db` and `lot`."""
from __future__ import annotations

import csv
import datetime as dt
import io

from . import site as sitelib
from .site import Factory, Site, Wifi

STAGE_LABELS = {"test": "ทดสอบโมดูล", "production": "ลง firmware จริง", "setup": "ตั้งค่า",
                "verify": "ตรวจก่อนส่ง", "update": "Flash ใหม่", "quick": "USB เร็ว",
                "rack": "ชั้นวาง (MQTT)", "usb": "Flash ผ่าน USB", "line": "ทดสอบ + firmware จริง",
                "ship": "ส่งมอบ Wi-Fi ลูกค้า", "retest": "ทดสอบซ้ำ", "soak": "ทดสอบนาน",
                "config": "ตั้งค่าเครื่อง"}


def stage_label(s: str) -> str:
    return STAGE_LABELS.get(s, s)


class LineCore:
    # ---- the site's settings (site.ini) ---------------------------------------

    def settings(self) -> dict:
        """What the settings page edits. Passwords never leave this machine: only
        whether one is set."""
        st = self.site
        keys = self.profile.config_keys if self.profile else {}
        return {
            "station": st.station, "path": st.path,
            "wifi": [{"ssid": w.ssid, "has_password": bool(w.password), "dhcp": w.dhcp, "ip": w.ip,
                      "gateway": w.gateway, "subnet": w.subnet, "dns": w.dns} for w in st.wifi],
            "mqtt": {"host": st.mqtt.get("host", ""), "port": st.mqtt.get("port", 1883),
                     "username": st.mqtt.get("username", ""),
                     "has_password": bool(st.mqtt.get("password"))},
            "ota_base": st.ota_base,
            "config": dict(st.config),
            "config_keys": keys,
            "factory": {"ssid": st.factory.ssid, "has_password": bool(st.factory.password),
                        "ip_base": st.factory.ip_base, "gateway": st.factory.gateway,
                        "subnet": st.factory.subnet, "dns": st.factory.dns,
                        "first": st.factory.ip_for(1) if st.factory.ip_base else "",
                        "capacity": self._capacity(st.factory)},
            "station_mqtt": {"username": st.station_mqtt.get("username", ""),
                             "has_password": bool(st.station_mqtt.get("password"))},
            "debug_mqtt": {"host": st.debug_mqtt.get("host", ""), "port": st.debug_mqtt.get("port", 1883),
                           "username": st.debug_mqtt.get("username", ""),
                           "has_password": bool(st.debug_mqtt.get("password"))},
        }

    @staticmethod
    def _capacity(f: Factory) -> int:
        try:
            return f.capacity()
        except ValueError:
            return 0

    def save_settings(self, d: dict) -> str:
        """The settings page's form back into site.ini. A password left out (None)
        keeps the one already saved for that network or broker."""
        old = self.site
        old_pw = {w.ssid: w.password for w in old.wifi}
        wifi = []
        for w in d.get("wifi", []):
            ssid = str(w.get("ssid", "")).strip()
            if not ssid:
                continue
            pw = w.get("password")
            if pw is None:
                pw = old_pw.get(str(w.get("old_ssid") or ssid), "")
            dhcp = bool(w.get("dhcp", True))
            wifi.append(Wifi(ssid=ssid, password=str(pw), dhcp=dhcp,
                             ip="" if dhcp else str(w.get("ip", "")).strip(),
                             gateway="" if dhcp else str(w.get("gateway", "")).strip(),
                             subnet="" if dhcp else str(w.get("subnet", "")).strip(),
                             dns="" if dhcp else str(w.get("dns", "")).strip()))
        m = d.get("mqtt") or {}
        mqtt = {}
        if str(m.get("host", "")).strip():
            pw = m.get("password")
            mqtt = {"host": str(m["host"]).strip(), "port": int(m.get("port") or 1883),
                    "username": str(m.get("username", "")).strip(),
                    "password": old.mqtt.get("password", "") if pw is None else str(pw)}
        config = {}
        keys = self.profile.config_keys if self.profile else {}
        bad = []
        for k, v in (d.get("config") or {}).items():
            if v is None or str(v).strip() == "":
                continue
            try:
                n = int(str(v).strip(), 0)
            except ValueError:
                bad.append(f"ค่าตั้ง {k}: '{v}' ไม่ใช่ตัวเลข")
                continue
            r = keys.get(k)
            if r and not r["min"] <= n <= r["max"]:
                bad.append(f"ค่าตั้ง {k}: {n} อยู่นอกช่วง {r['min']}–{r['max']}")
            config[k] = n
        f = d.get("factory") or {}
        fpw = f.get("password")
        factory = Factory(ssid=str(f.get("ssid", "")).strip(),
                          password=old.factory.password if fpw is None else str(fpw),
                          **{k: str(f.get(k, "")).strip() for k in ("ip_base", "gateway", "subnet", "dns")})
        sm = d.get("station_mqtt") or {}
        spw = sm.get("password")
        station_mqtt = {"username": str(sm.get("username", "")).strip(),
                        "password": old.station_mqtt.get("password", "") if spw is None else str(spw)}
        # no login of its own: the boxes' login, password and all
        station_mqtt = {k: v for k, v in station_mqtt.items() if v} if station_mqtt["username"] else {}
        dm = d.get("debug_mqtt") or {}
        debug_mqtt = {}
        if str(dm.get("host", "")).strip():
            dpw = dm.get("password")
            debug_mqtt = {"host": str(dm["host"]).strip(), "port": int(dm.get("port") or 1883),
                          "username": str(dm.get("username", "")).strip(),
                          "password": old.debug_mqtt.get("password", "") if dpw is None else str(dpw)}
        elif "debug_mqtt" not in d:
            debug_mqtt = dict(old.debug_mqtt)     # a form without the section keeps it
        new = Site(station=str(d.get("station", "")).strip(), wifi=wifi, mqtt=mqtt,
                   ota_base=str(d.get("ota_base", "")).strip(), config=config, factory=factory,
                   station_mqtt=station_mqtt, debug_mqtt=debug_mqtt, path=old.path)
        bad = sitelib.validate(new, self.profile.unit_max if self.profile else 999) + bad
        if bad:
            raise RuntimeError("\n".join(bad))
        path = sitelib.save(new)
        self.site = new           # runs started from now on use it; running ones keep theirs
        return path

    def summary(self, lot: str | None = None) -> dict:
        """The lot so far: every SN, its box, its result, and the unit numbers not yet used."""
        if not self.profile:
            raise RuntimeError(self.profile_error)
        lot = self.lot if lot is None else lot.strip()
        p = self.profile
        prefix = p.make_sn(lot, 0)[:-int(p.sn.get("unit_digits", 3))] if lot else ""
        today = dt.datetime.now().astimezone().replace(hour=0, minute=0, second=0,
                                                       microsecond=0).isoformat(timespec="seconds")
        rows = self.db.summary(p.product, p.routes, prefix=prefix, since=today)
        for r in rows:
            r["unit"] = p.unit_of(r["sn"], lot) if lot else None
            r["stage_labels"] = {k: stage_label(k) for k in r["stages"]}
            if r["fail"]:
                r["fail"]["stage_label"] = stage_label(r["fail"]["stage"])
        units = sorted(r["unit"] for r in rows if r["unit"] is not None)
        missing = [u for u in range(1, units[-1]) if u not in set(units)] if units else []
        counts = {k: sum(1 for r in rows if r["result"] == k) for k in ("PASS", "FAIL", "INCOMPLETE")}
        return {"lot": lot, "product": p.product, "rows": rows, "counts": counts, "missing": missing,
                "sequence": [{"name": x, "label": stage_label(x)} for x in p.stage_names()],
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

