"""The site's settings for provisioning: Wi-Fi, broker, OTA server, defaults.

They live in flasher/site.ini, which is gitignored (shape in
site.example.ini): a login in the repository is a login in every clone.
The station masks every password in what it prints and records.
"""
from __future__ import annotations

import configparser
import os
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
class Site:
    station: str = ""
    wifi: list[Wifi] = field(default_factory=list)
    mqtt: dict = field(default_factory=dict)
    ota_base: str = ""
    config: dict[str, int] = field(default_factory=dict)
    path: str = ""

    def secrets(self) -> list[str]:
        out = [w.password for w in self.wifi if w.password]
        if self.mqtt.get("password"):
            out.append(self.mqtt["password"])
        return out


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
    return s


def mask(text: str, secrets: list[str]) -> str:
    for sec in sorted(set(secrets), key=len, reverse=True):
        if sec:
            text = text.replace(sec, "****")
    return text
