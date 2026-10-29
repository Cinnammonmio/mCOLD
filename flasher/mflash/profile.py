"""A product profile: everything the station needs to know about one product.

One YAML file per product in flasher/profiles/. Adding a product (eTEMP)
is adding a file, not changing code. Paths in a profile are relative to
the repository root.
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass, field

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
FLASHER = os.path.dirname(HERE)
REPO = os.path.dirname(FLASHER)
PROFILES = os.path.join(FLASHER, "profiles")

STEP_TYPES = {
    "flash", "console", "health", "confirm", "input_sn", "set_sn", "reboot", "provision",
    "nmea", "version", "calibrate", "note", "chip",
}

# what the rack (remote.py) runs over Wi-Fi -> MQTT, on boxes off the dock
RACK_STEP_TYPES = {
    "online", "status", "rpc", "identify_confirm", "debug_pull", "config_doc", "ota",
    "wifi_handover",
}


class ProfileError(Exception):
    pass


@dataclass
class FirmwareKind:
    name: str
    project: str                 # PlatformIO project, relative to the repo
    env: str
    images: list[dict]           # {offset, file}
    console: str                 # "usj" or "app": which port its console is on
    partitions_csv: str = ""     # the layout its partitions.bin must match
    min_version: str = ""        # older builds lack commands the stages use
    description: str = ""


@dataclass
class Profile:
    product: str
    name: str
    chip: str
    flash: dict
    usb: dict
    console: dict
    sn: dict
    firmware: dict[str, FirmwareKind]
    stages: dict[str, list[dict]]
    sequence: list[str]
    modes: list[dict] = field(default_factory=list)
    rack: list[dict] = field(default_factory=list)    # the rack's steps, by MQTT
    # the ways a new box goes through the line; a box that has passed all the
    # stages of one of them is done (the SN summary's PASS)
    routes: list[list[str]] = field(default_factory=list)
    config_keys: dict = field(default_factory=dict)
    chip_expect: dict = field(default_factory=dict)
    incomplete: list[str] = field(default_factory=list)
    path: str = ""

    def mode(self, name: str) -> list[str] | None:
        """The stages of a named mode, or None if `name` is not one."""
        if name == "all":
            return list(self.sequence)
        for m in self.modes:
            if m["name"] == name:
                return list(m["stages"])
        return None

    def stage(self, name: str) -> list[dict]:
        if name not in self.stages:
            raise ProfileError(f"{self.product}: no stage '{name}' (has {', '.join(self.stages)})")
        return self.stages[name]

    def sn_ok(self, sn: str) -> bool:
        pat = self.sn.get("pattern")
        return bool(re.fullmatch(pat, sn)) if pat else bool(sn)

    # A lot is the SN without its unit number; a slot adds the unit.
    def lot_ok(self, lot: str) -> bool:
        pat = self.sn.get("lot_pattern")
        return bool(lot) and (bool(re.fullmatch(pat, lot)) if pat else True)

    @property
    def unit_max(self) -> int:
        return 10 ** int(self.sn.get("unit_digits", 3)) - 1

    def unit_from_sn(self, sn: str) -> int | None:
        """The unit number at the end of an SN, whatever its lot."""
        digits = int(self.sn.get("unit_digits", 3))
        tail = sn.rsplit(self.sn.get("unit_sep", "-"), 1)[-1] if sn else ""
        return int(tail) if tail.isdigit() and len(tail) == digits else None

    def stage_names(self) -> list[str]:
        """Every stage a box can have a result for, in the order of the line."""
        out = []
        for r in [self.sequence, *self.routes]:
            for s in r:
                if s not in out:
                    out.append(s)
        return out

    def make_sn(self, lot: str, unit: int) -> str:
        return f"{lot}{self.sn.get('unit_sep', '-')}{unit:0{int(self.sn.get('unit_digits', 3))}d}"

    def unit_of(self, sn: str, lot: str) -> int | None:
        """The unit number of `sn` if it belongs to `lot`, else None."""
        head = lot + self.sn.get("unit_sep", "-")
        tail = sn[len(head):] if sn and sn.startswith(head) else ""
        return int(tail) if tail.isdigit() and len(tail) == int(self.sn.get("unit_digits", 3)) else None


def list_profiles() -> list[str]:
    if not os.path.isdir(PROFILES):
        return []
    return sorted(f[:-5] for f in os.listdir(PROFILES) if f.endswith(".yaml"))


def load(product: str) -> Profile:
    path = product if product.endswith(".yaml") else os.path.join(PROFILES, product + ".yaml")
    if not os.path.isfile(path):
        raise ProfileError(f"no profile {path} (have: {', '.join(list_profiles()) or 'none'})")
    with open(path, encoding="utf-8") as f:
        d = yaml.safe_load(f)
    try:
        fw = {}
        for k, v in (d.get("firmware") or {}).items():
            fw[k] = FirmwareKind(
                name=k, project=v["project"], env=v.get("env", ""),
                images=[{"offset": int(i["offset"]), "file": i["file"]} for i in v["images"]],
                console=v.get("console", "app"), partitions_csv=v.get("partitions_csv", ""),
                min_version=str(v.get("min_version", "")),
                description=v.get("description", ""))
        p = Profile(
            product=d["product"], name=d.get("name", d["product"]), chip=d["chip"],
            flash=d["flash"], usb=d["usb"], console=d.get("console", {}),
            sn=d.get("sn", {}), firmware=fw, stages=d.get("stages", {}),
            sequence=d.get("sequence", list(d.get("stages", {}))),
            config_keys=d.get("config_keys") or {},
            modes=[{"name": m["name"], "label": m.get("label", m["name"]), "stages": list(m["stages"])}
                   for m in (d.get("modes") or [])],
            chip_expect=d.get("chip_expect", {}), incomplete=d.get("incomplete", []) or [],
            rack=list(d.get("rack") or []),
            routes=[list(r) for r in (d.get("routes") or [])],
            path=path)
        if not p.routes:
            p.routes = [list(p.sequence)]
    except (KeyError, TypeError, ValueError) as e:
        raise ProfileError(f"{path}: {e!r}") from e
    problems = validate(p)
    if problems:
        raise ProfileError(f"{path}:\n  " + "\n  ".join(problems))
    return p


def validate(p: Profile) -> list[str]:
    out = []
    for s in p.sequence:
        if s not in p.stages:
            out.append(f"sequence names stage '{s}', which is not defined")
    for m in p.modes:
        for s in m["stages"]:
            if s not in p.stages:
                out.append(f"mode {m['name']} names stage '{s}', which is not defined")
    for r in p.routes:
        for s in r:
            if s not in p.stages and not (s == "rack" and p.rack):
                out.append(f"route {r} names stage '{s}', which is not defined")
    for i, st in enumerate(p.rack):
        if st.get("type") not in RACK_STEP_TYPES:
            out.append(f"rack step {i + 1}: unknown type {st.get('type')!r}")
        elif "name" not in st:
            out.append(f"rack step {i + 1}: no name")
        elif st["type"] == "ota" and st.get("firmware", "production") not in p.firmware:
            out.append(f"rack step {i + 1}: firmware {st.get('firmware')!r} not in firmware:")
    for sname, steps in p.stages.items():
        for i, st in enumerate(steps or []):
            where = f"stage {sname} step {i + 1}"
            t = st.get("type")
            if t not in STEP_TYPES:
                out.append(f"{where}: unknown type {t!r}")
                continue
            if "name" not in st:
                out.append(f"{where}: no name")
            if t == "flash" and st.get("firmware") not in p.firmware:
                out.append(f"{where}: firmware {st.get('firmware')!r} not in firmware:")
            if t in ("console", "nmea") and not st.get("send"):
                out.append(f"{where}: {t} needs 'send'")
            for key in ("expect", "forbid"):
                for rx in st.get(key, []) or []:
                    try:
                        re.compile(rx)
                    except re.error as e:
                        out.append(f"{where}: bad {key} regex {rx!r}: {e}")
    return out
