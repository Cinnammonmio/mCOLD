"""Reading the console's replies. Every pattern here is taken from the
firmware source that prints it, named beside each function."""
from __future__ import annotations

import re

# firmware/src/main.cpp print_health(): "  %-9s %-9s %7lu %7lu %10s"
_HEALTH_ROW = re.compile(r"^\s+(\w+)\s+(ok|degraded|failed|unknown|absent)\s+(\d+)\s+(\d+)\s+(\S+)\s*$",
                         re.M)


def health_states(text: str) -> dict[str, str]:
    """device -> state from `health`."""
    return {m.group(1): m.group(2) for m in _HEALTH_ROW.finditer(text)}


def health_temperature(text: str) -> float | None:
    """`temperature  23.50 C  (...)`, or None for `temperature  -- (...)`."""
    m = re.search(r"^\s+temperature\s+(-?\d+(?:\.\d+)?) C", text, re.M)
    return float(m.group(1)) if m else None


def health_clock(text: str) -> tuple[str, str, bool] | None:
    """(time, source, usable) from the `clock` line of `health`."""
    m = re.search(r"^\s+clock\s+(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\s+source (\w+)(.*)$", text, re.M)
    if not m:
        return None
    return m.group(1), m.group(2), "NOT USABLE" not in m.group(3)


def version(text: str) -> tuple[str, str] | None:
    """(version, first 16 hex of the ELF SHA-256) from `version` (print_version)."""
    v = re.search(r"^\s+firmware\s+(\S+)", text, re.M)
    s = re.search(r"^\s+elf sha256\s+([0-9a-f]+)", text, re.M)
    if not v:
        return None
    return v.group(1), s.group(1) if s else ""


def nmea_sentences(text: str) -> tuple[int, int, list[str]]:
    """(good, bad, talker+type of the good ones): NMEA lines by checksum.

    At the wrong baud rate the GNSS still sends bytes -- 488 of nonsense
    in twelve seconds -- so bytes arriving proves nothing; a sentence
    whose checksum adds up does.
    """
    good, bad, kinds = 0, 0, []
    for m in re.finditer(r"\$([A-Z]{2}[A-Z]{3})([^$*\r\n]*)\*([0-9A-Fa-f]{2})", text):
        body = m.group(1) + m.group(2)
        ck = 0
        for c in body.encode("ascii", "replace"):
            ck ^= c
        if ck == int(m.group(3), 16):
            good += 1
            kinds.append(m.group(1))
        else:
            bad += 1
    return good, bad, kinds


def rpc_mac(text: str) -> str:
    """The MAC from `rpc {"cmd":"GET_STATUS"}` (rpc.cpp c_status: net.mac)."""
    m = re.search(r'"mac"\s*:\s*"([0-9A-Fa-f:]{17})"', text)
    return m.group(1).upper() if m else ""


def wifi_known(text: str) -> list[str]:
    """The known networks from `wifi` / `sync` (print_sync: "  known      a, b")."""
    m = re.search(r"^\s+known\s+(.*)$", text, re.M)
    if not m or m.group(1).strip() in ("", "none"):
        return []
    return [x.strip() for x in m.group(1).split(", ") if x.strip()]
