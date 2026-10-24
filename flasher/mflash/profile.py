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
    chip_expect: dict = field(default_factory=dict)
    incomplete: list[str] = field(default_factory=list)
    path: str = ""

    def stage(self, name: str) -> list[dict]:
        if name not in self.stages:
            raise ProfileError(f"{self.product}: no stage '{name}' (has {', '.join(self.stages)})")
        return self.stages[name]

    def sn_ok(self, sn: str) -> bool:
        pat = self.sn.get("pattern")
        return bool(re.fullmatch(pat, sn)) if pat else bool(sn)


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
            chip_expect=d.get("chip_expect", {}), incomplete=d.get("incomplete", []) or [],
            path=path)
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
