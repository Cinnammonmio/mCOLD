"""The box's settings as an NVS partition image, written at flash time.

The browser flashes a new box in one esptool session: bootloader,
partition table, otadata, the app -- and this image at the `nvs`
partition. The box then boots with its SN, its Wi-Fi, its broker and its
OTA base already in place: no console, no second USB connection.

Every namespace, key and type below is the one the firmware reads
(firmware/p7-power, 4185a46). If the firmware renames one, the box
boots without that setting -- so they are listed in notes/flasher.md for
the firmware side, and `read()` here lets a test check what was written.

    sys/sn            str     main.cpp device_sn() / sn_store()
    mqtt/host,user,pass str   uplink.cpp load()
    mqtt/port         u16
    net/s0..s4        str     net.cpp load(): SSID of slot i
    net/p0..p4        str     its password ("" = open)
    net/i0..i4        blob16  fixed address: ip, gw, mask, dns as esp_ip4_addr_t (network order)
    ota/base          str     ota.cpp get_str("base")
    cfg/<key>         i32     config.cpp load(); cfg/schema u32 = 3
    factory/on        u8      factory.cpp (docs/firmware-mqtt-rpc.patch)

Built with Espressif's own generator (esp-idf-nvs-partition-gen), NVS
format version 2, the one ESP-IDF 5 reads.
"""
from __future__ import annotations

import io
import ipaddress
import struct
import zlib
from dataclasses import dataclass, field

NVS_OFFSET = 0x9000            # firmware/partitions.csv
NVS_SIZE = 0x4000
CONFIG_SCHEMA = 3              # config.cpp SCHEMA
NET_MAX = 5                    # net.cpp NET_MAX
KEY_MAX = 15                   # NVS key length


class NvsError(Exception):
    pass


@dataclass
class Net:
    ssid: str
    password: str = ""
    ip: str = ""               # empty = DHCP
    gateway: str = ""
    subnet: str = ""
    dns: str = ""


@dataclass
class BoxSettings:
    sn: str
    wifi: list[Net] = field(default_factory=list)
    mqtt: dict = field(default_factory=dict)       # host, port, username, password
    ota_base: str = ""
    config: dict[str, int] = field(default_factory=dict)
    factory: bool = False


def _ip_blob(n: Net) -> bytes:
    """Four esp_ip4_addr_t, each its address bytes in network order."""
    def b(s: str) -> bytes:
        return ipaddress.IPv4Address(s).packed if s else b"\0\0\0\0"
    return b(n.ip) + b(n.gateway) + b(n.subnet) + b(n.dns)


def build(s: BoxSettings, size: int = NVS_SIZE) -> bytes:
    from esp_idf_nvs_partition_gen import nvs_partition_gen as g

    if not s.sn:
        raise NvsError("no SN")
    if len(s.wifi) > NET_MAX:
        raise NvsError(f"at most {NET_MAX} Wi-Fi networks")
    for k in s.config:
        if len(k) > KEY_MAX:
            raise NvsError(f"config key {k!r} is longer than {KEY_MAX}")

    out = io.BytesIO()
    # as the generator's own CLI does (check_size): one page stays reserved for the
    # NVS library's garbage collection, and the generator appends it itself
    nvs = g.nvs_open(out, size - 4096, version=g.Page.VERSION2)

    def ns(name):
        g.write_entry(nvs, name, "namespace", "", "")

    def put(key, enc, value):
        g.write_entry(nvs, key, "data", enc, value)

    ns("sys")
    put("sn", "string", s.sn)
    if s.mqtt.get("host"):
        ns("mqtt")
        put("host", "string", s.mqtt["host"])
        put("port", "u16", str(int(s.mqtt.get("port") or 1883)))
        put("user", "string", s.mqtt.get("username", ""))
        put("pass", "string", s.mqtt.get("password", ""))
    if s.wifi:
        ns("net")
        for i, n in enumerate(s.wifi):
            put(f"s{i}", "string", n.ssid)
            put(f"p{i}", "string", n.password)
            if n.ip:
                put(f"i{i}", "hex2bin", _ip_blob(n).hex())
    if s.ota_base:
        ns("ota")
        put("base", "string", s.ota_base)
    if s.config:
        ns("cfg")
        put("schema", "u32", str(CONFIG_SCHEMA))
        for k, v in s.config.items():
            put(k, "i32", str(int(v)))
    if s.factory:
        ns("factory")
        put("on", "u8", "1")
    g.nvs_close(nvs)
    img = out.getvalue()
    if len(img) != size:
        raise NvsError(f"image is {len(img)} bytes, the partition {size}")
    return img


# ---- reading one back (tests, and checking what a box was given) ---------

_TYPES = {0x01: "u8", 0x11: "i8", 0x02: "u16", 0x12: "i16", 0x04: "u32", 0x14: "i32",
          0x08: "u64", 0x18: "i64", 0x21: "str", 0x41: "blob", 0x42: "blob_data", 0x48: "blob_idx"}
_FMT = {"u8": "<B", "i8": "<b", "u16": "<H", "i16": "<h", "u32": "<I", "i32": "<i",
        "u64": "<Q", "i64": "<q"}


def read(img: bytes) -> dict[str, dict[str, object]]:
    """namespace -> key -> value, from an NVS image (format version 2)."""
    names: dict[int, str] = {}
    raw: list[tuple[int, str, str, int, bytes, bytes]] = []    # ns, type, key, chunk, data, payload
    for p in range(0, len(img), 4096):
        page = img[p:p + 4096]
        if struct.unpack_from("<I", page, 0)[0] == 0xFFFFFFFF:
            continue                                            # never used
        bitmap = page[32:64]
        i = 0
        while i < 126:
            state = (bitmap[i // 4] >> ((i % 4) * 2)) & 3
            if state != 2:                                      # 2 = written
                i += 1
                continue
            e = page[64 + 32 * i: 96 + 32 * i]
            nsi, typ, span, chunk = e[0], e[1], e[2], e[3]
            crc = struct.unpack_from("<I", e, 4)[0]
            if crc != zlib.crc32(e[0:4] + e[8:32], 0xFFFFFFFF) & 0xFFFFFFFF:   # as ESP-IDF seeds it
                raise NvsError(f"entry {i} on page {p // 4096}: bad CRC")
            key = e[8:24].split(b"\0", 1)[0].decode()
            payload = b"".join(page[64 + 32 * j: 96 + 32 * j] for j in range(i + 1, i + span))
            raw.append((nsi, _TYPES.get(typ, hex(typ)), key, chunk, e[24:32], payload))
            i += max(span, 1)
    out: dict[str, dict[str, object]] = {}
    for nsi, typ, key, _, data, _ in raw:
        if nsi == 0:
            names[data[0]] = key
    chunks: dict[tuple[int, str], dict[int, bytes]] = {}
    for nsi, typ, key, chunk, data, payload in raw:
        if nsi == 0:
            continue
        if typ == "blob_data":
            size = struct.unpack_from("<H", data, 0)[0]
            chunks.setdefault((nsi, key), {})[chunk] = payload[:size]
    for nsi, typ, key, chunk, data, payload in raw:
        if nsi == 0 or typ == "blob_data":
            continue
        d = out.setdefault(names.get(nsi, f"#{nsi}"), {})
        if typ in _FMT:
            d[key] = struct.unpack_from(_FMT[typ], data, 0)[0]
        elif typ == "str":
            size = struct.unpack_from("<H", data, 0)[0]
            d[key] = payload[:size].rstrip(b"\0").decode()
        elif typ == "blob_idx":
            size, count, start = struct.unpack_from("<IBB", data, 0)
            parts = chunks.get((nsi, key), {})
            d[key] = b"".join(parts[c] for c in range(start, start + count) if c in parts)[:size]
    return out


def from_site(site, sn: str, wifi: list[str] | None = None, factory: bool = False,
              unit: int | None = None) -> BoxSettings:
    """A box's settings from the site's (site.ini): the networks named in `wifi`
    (all of them when None); the line's own network with the unit's fixed address."""
    nets: list[Net] = []
    f = site.factory
    want = None if wifi is None else set(wifi)
    if f.ssid and (want is None or f.ssid in want):
        ip = f.ip_for(unit) if (f.ip_base and unit) else ""
        nets.append(Net(f.ssid, f.password, ip, f.gateway if ip else "", f.subnet if ip else "",
                        f.dns if ip else ""))
    for w in site.wifi:
        if want is None or w.ssid in want:
            nets.append(Net(w.ssid, w.password, "" if w.dhcp else w.ip, "" if w.dhcp else w.gateway,
                            "" if w.dhcp else w.subnet, "" if w.dhcp else w.dns))
    return BoxSettings(sn=sn, wifi=nets[:NET_MAX], mqtt=dict(site.mqtt), ota_base=site.ota_base,
                       config=dict(site.config), factory=factory)
