"""The site's settings for provisioning: Wi-Fi, broker, OTA server, defaults.

They live in flasher/site.ini, which is gitignored (shape in
site.example.ini): a login in the repository is a login in every clone.
The station masks every password in what it prints and records.
"""
from __future__ import annotations

import configparser
import ipaddress
import os
import re
from dataclasses import dataclass, field

from .profile import FLASHER


@dataclass
class Wifi:
    ssid: str
    password: str = ""
    dhcp: bool = True
    ip: str = ""
    gateway: str = ""
    subnet: str = ""
    dns: str = ""


@dataclass
class Factory:
    """The line's own Wi-Fi: on the box from the USB step until its hand-over,
    with a fixed address per unit number so a box can be found by its label."""
    ssid: str = ""
    password: str = ""
    ip_base: str = ""      # unit n gets ip_base + n
    gateway: str = ""
    subnet: str = ""
    dns: str = ""

    def ip_for(self, unit: int) -> str:
        if not self.ip_base:
            return ""
        return str(ipaddress.IPv4Address(self.ip_base) + unit)

    def network(self) -> ipaddress.IPv4Network:
        return ipaddress.IPv4Network(f"{self.gateway}/{self.subnet}", strict=False)

    def capacity(self) -> int:
        """How many unit numbers (1..n) the pool has room for in its network."""
        if not self.ip_base:
            return 0
        return int(self.network().broadcast_address) - int(ipaddress.IPv4Address(self.ip_base)) - 1


@dataclass
class Site:
    station: str = ""
    wifi: list[Wifi] = field(default_factory=list)
    mqtt: dict = field(default_factory=dict)
    ota_base: str = ""
    config: dict[str, int] = field(default_factory=dict)
    factory: Factory = field(default_factory=Factory)
    # the station's own login to the broker (the rack); none = the boxes' login
    station_mqtt: dict = field(default_factory=dict)
    path: str = ""

    def secrets(self) -> list[str]:
        out = [w.password for w in self.wifi if w.password]
        for p in (self.mqtt.get("password"), self.station_mqtt.get("password"), self.factory.password):
            if p:
                out.append(p)
        return out

    def rack_broker(self) -> dict:
        """Where and as whom the station listens to the boxes."""
        b = dict(self.mqtt)
        b.update({k: v for k, v in self.station_mqtt.items() if v not in ("", None)})
        return b


def default_path() -> str:
    return os.environ.get("MFLASH_SITE") or os.path.join(FLASHER, "site.ini")


def load(path: str | None = None) -> Site:
    path = path or default_path()
    if not os.path.isfile(path):
        return Site(path=path)
    cp = configparser.ConfigParser(interpolation=None)
    cp.optionxform = str
    cp.read(path, encoding="utf-8")
    s = Site(path=path)
    s.station = cp.get("station", "name", fallback="")
    for sec in cp.sections():
        if sec.startswith("wifi"):
            c = cp[sec]
            if not c.get("ssid", "").strip():
                continue
            s.wifi.append(Wifi(
                ssid=c.get("ssid").strip(), password=c.get("password", "").strip(),
                dhcp=c.getboolean("dhcp", fallback=not c.get("ip", "").strip()),
                ip=c.get("ip", "").strip(), gateway=c.get("gateway", "").strip(),
                subnet=c.get("subnet", "").strip(), dns=c.get("dns", "").strip()))
    if cp.has_section("mqtt") and cp.get("mqtt", "host", fallback="").strip():
        m = cp["mqtt"]
        s.mqtt = {"host": m.get("host").strip(), "port": int(m.get("port", "1883") or 1883),
                  "username": m.get("username", "").strip(), "password": m.get("password", "").strip()}
    s.ota_base = cp.get("ota", "base", fallback="").strip()
    if cp.has_section("config"):
        s.config = {k: int(v, 0) for k, v in cp["config"].items() if v.strip()}
    if cp.has_section("factory"):
        f = cp["factory"]
        s.factory = Factory(**{k: f.get(k, "").strip() for k in
                               ("ssid", "password", "ip_base", "gateway", "subnet", "dns")})
    if cp.has_section("station_mqtt"):
        m = cp["station_mqtt"]
        s.station_mqtt = {k: m.get(k, "").strip() for k in ("host", "port", "username", "password")
                          if m.get(k, "").strip()}
        if s.station_mqtt.get("port"):
            s.station_mqtt["port"] = int(s.station_mqtt["port"])
    return s


CONFIG_KEY = re.compile(r"[a-z][a-z0-9_]{0,22}")
IPV4 = re.compile(r"(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}")


def validate(site: Site, unit_max: int = 999) -> list[str]:
    """What the box's console would refuse, said before anything is saved."""
    bad = []
    f = site.factory
    if f.ssid:
        if not 1 <= len(f.ssid.encode()) <= 32 or " " in f.ssid:
            bad.append("Wi-Fi โรงงาน: ชื่อ 1–32 ตัว ไม่มีช่องว่าง (ใช้กับ IP คงที่)")
        if f.password and not 8 <= len(f.password) <= 63:
            bad.append("Wi-Fi โรงงาน: รหัสผ่าน 8–63 ตัว หรือว่าง")
        if any(w.ssid == f.ssid for w in site.wifi):
            bad.append("Wi-Fi โรงงาน: ชื่อซ้ำกับเครือข่ายที่จะส่งให้ลูกค้า (มันจะถูกลบตอนส่งมอบ)")
    if f.ip_base:
        try:
            n = f.capacity()
            if n < 1:
                bad.append(f"IP pool: {f.ip_base} + 1 ไม่อยู่ในวง {f.network()}")
            gw = ipaddress.IPv4Address(f.gateway)
            if ipaddress.IPv4Address(f.ip_base) < gw <= ipaddress.IPv4Address(f.ip_base) + max(n, 0):
                bad.append(f"IP pool: gateway {gw} อยู่ในช่วงที่จะให้กล่อง ({f.ip_for(1)}–{f.ip_for(n)})")
        except ValueError as e:
            bad.append(f"IP pool: {e}")
        if f.dns and not IPV4.fullmatch(f.dns):
            bad.append("IP pool: DNS ไม่ใช่ IPv4")
    elif f.ssid and (f.gateway or f.subnet):
        bad.append("IP pool: ใส่ IP เริ่มต้นด้วย (หรือเว้นว่างทั้งหมดเพื่อใช้ DHCP)")
    if len(site.wifi) > 5:
        bad.append("Wi-Fi ได้ไม่เกิน 5 เครือข่าย")
    seen = set()
    for i, w in enumerate(site.wifi, 1):
        if not 1 <= len(w.ssid.encode()) <= 32:
            bad.append(f"Wi-Fi {i}: ชื่อเครือข่าย (SSID) 1–32 ตัวอักษร")
        if w.ssid in seen:
            bad.append(f"Wi-Fi {i}: {w.ssid} ซ้ำ")
        seen.add(w.ssid)
        if w.password and not 8 <= len(w.password) <= 63:
            bad.append(f"Wi-Fi {i}: รหัสผ่าน 8–63 ตัว หรือว่างสำหรับเครือข่ายเปิด")
        if not w.dhcp:
            if " " in w.ssid:
                bad.append(f"Wi-Fi {i}: IP คงที่ใช้กับชื่อเครือข่ายที่มีช่องว่างไม่ได้")
            for name, v in (("IP", w.ip), ("gateway", w.gateway), ("subnet", w.subnet)):
                if not IPV4.fullmatch(v or ""):
                    bad.append(f"Wi-Fi {i}: {name} ไม่ใช่ IPv4")
            if w.dns and not IPV4.fullmatch(w.dns):
                bad.append(f"Wi-Fi {i}: DNS ไม่ใช่ IPv4")
    m = site.mqtt
    if m:
        if not m.get("host") or len(m["host"]) > 63 or " " in m["host"]:
            bad.append("Broker: host 1–63 ตัว ไม่มีช่องว่าง")
        if not 1 <= int(m.get("port") or 0) <= 65535:
            bad.append("Broker: port 1–65535")
        if any(c.isspace() for c in (m.get("username", "") + m.get("password", ""))):
            bad.append("Broker: ชื่อผู้ใช้และรหัสผ่านต้องไม่มีช่องว่าง (console รับไม่ได้)")
        if m.get("password") and not m.get("username"):
            bad.append("Broker: มีรหัสผ่านแต่ไม่มีชื่อผู้ใช้")
    if site.ota_base and (not re.match(r"https?://", site.ota_base) or len(site.ota_base) >= 128):
        bad.append("OTA: ต้องขึ้นต้น http:// หรือ https:// และสั้นกว่า 128 ตัว")
    for k in site.config:
        if not CONFIG_KEY.fullmatch(k):
            bad.append(f"ค่าตั้ง: ชื่อ '{k}' ไม่ถูกต้อง")
    return bad


def save(site: Site, path: str | None = None) -> str:
    """Write the settings back to site.ini (the file the station reads at start)."""
    path = path or site.path or default_path()
    cp = configparser.ConfigParser(interpolation=None)
    cp.optionxform = str
    cp["station"] = {"name": site.station}
    for i, w in enumerate(site.wifi, 1):
        sec = {"ssid": w.ssid, "password": w.password, "dhcp": "yes" if w.dhcp else "no"}
        if not w.dhcp:
            sec.update(ip=w.ip, gateway=w.gateway, subnet=w.subnet, dns=w.dns)
        cp[f"wifi.{i}"] = sec
    if site.mqtt:
        cp["mqtt"] = {k: str(site.mqtt.get(k, "")) for k in ("host", "port", "username", "password")}
    cp["ota"] = {"base": site.ota_base}
    cp["config"] = {k: str(v) for k, v in site.config.items()}
    f = site.factory
    cp["factory"] = {"ssid": f.ssid, "password": f.password, "ip_base": f.ip_base,
                     "gateway": f.gateway, "subnet": f.subnet, "dns": f.dns}
    if site.station_mqtt:
        cp["station_mqtt"] = {k: str(v) for k, v in site.station_mqtt.items() if v not in ("", None)}
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("; Written by the station's settings page (mflash gui). Not in git: it holds logins.\n")
        cp.write(f)
    os.replace(tmp, path)
    site.path = path
    return path


def mask(text: str, secrets: list[str]) -> str:
    for sec in sorted(set(secrets), key=len, reverse=True):
        if sec:
            text = text.replace(sec, "****")
    return text
