"""What a firmware file says about itself, read without flashing it.

An app image carries an esp_app_desc_t at the start of its first segment
(ESP-IDF and the Arduino core both put it there): version, project name,
build time, IDF version and the ELF's SHA-256. That is what the box
reports on its console (`version`), so a flash can be checked against
the file that was meant to go on, not just against "esptool said OK".

A partition table is 32-byte entries; the one that goes on must match
the product's layout, or the trip log lands somewhere the firmware does
not look.
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass

APP_DESC_MAGIC = 0xABCD5432
IMAGE_MAGIC = 0xE9
# image header (24 bytes) + first segment header (8 bytes)
APP_DESC_OFFSET = 24 + 8

PART_MAGIC = 0x50AA
PART_MD5_MAGIC = 0xEBEB

PART_TYPES = {0x00: "app", 0x01: "data"}
APP_SUBTYPES = {0x00: "factory", 0x20: "test", **{0x10 + i: f"ota_{i}" for i in range(16)}}
DATA_SUBTYPES = {0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump", 0x04: "nvs_keys",
                 0x05: "efuse", 0x06: "undefined", 0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs",
                 0x83: "littlefs"}


@dataclass
class AppInfo:
    version: str
    project: str
    time: str
    date: str
    idf: str
    elf_sha256: str      # hex, 64 chars
    secure_version: int


@dataclass
class Partition:
    name: str
    type: str
    subtype: str
    offset: int
    size: int


def _cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def read_app_info(data: bytes) -> AppInfo | None:
    """The app descriptor of an app image, or None if this is not one."""
    if len(data) < APP_DESC_OFFSET + 256 or data[0] != IMAGE_MAGIC:
        return None
    d = data[APP_DESC_OFFSET:APP_DESC_OFFSET + 256]
    magic, secure_version = struct.unpack_from("<II", d, 0)
    if magic != APP_DESC_MAGIC:
        return None
    # magic, secure_version, reserv1[2], version[32], project_name[32],
    # time[16], date[16], idf_ver[32], app_elf_sha256[32]
    o = 16
    version = _cstr(d[o:o + 32]); o += 32
    project = _cstr(d[o:o + 32]); o += 32
    btime = _cstr(d[o:o + 16]); o += 16
    bdate = _cstr(d[o:o + 16]); o += 16
    idf = _cstr(d[o:o + 32]); o += 32
    sha = d[o:o + 32].hex()
    return AppInfo(version, project, btime, bdate, idf, sha, secure_version)


def read_app_info_file(path: str) -> AppInfo | None:
    with open(path, "rb") as f:
        return read_app_info(f.read(APP_DESC_OFFSET + 256))


def _subtype_name(ptype: int, sub: int) -> str:
    if ptype == 0x00:
        return APP_SUBTYPES.get(sub, f"0x{sub:02x}")
    if ptype == 0x01:
        return DATA_SUBTYPES.get(sub, f"0x{sub:02x}")
    return f"0x{sub:02x}"


def read_partition_table(data: bytes) -> list[Partition] | None:
    """The entries of a binary partition table, or None if it is not one."""
    parts = []
    for i in range(0, len(data) - 31, 32):
        e = data[i:i + 32]
        magic = struct.unpack_from("<H", e, 0)[0]
        if magic == PART_MD5_MAGIC or e == b"\xff" * 32:
            break
        if magic != PART_MAGIC:
            return None
        ptype, sub, off, size = struct.unpack_from("<BBII", e, 2)
        name = _cstr(e[12:28])
        parts.append(Partition(name, PART_TYPES.get(ptype, f"0x{ptype:02x}"),
                               _subtype_name(ptype, sub), off, size))
    return parts or None


def read_partition_table_file(path: str) -> list[Partition] | None:
    with open(path, "rb") as f:
        return read_partition_table(f.read(0xC00))


def short_elf_sha(info: AppInfo) -> str:
    """The 16 hex characters the firmware prints after `elf sha256`."""
    return info.elf_sha256[:16]
