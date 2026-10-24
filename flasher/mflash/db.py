"""The station's record: every box, every run, every step, kept in SQLite.

A box is known by its MAC, which nothing can change; its SN is what the
factory gives it. A run is one stage on one box; its steps keep the
console text they were judged on, so a FAIL can be read again later
without the box.
"""
from __future__ import annotations

import csv
import datetime as dt
import json
import os
import sqlite3
import threading

from .profile import FLASHER

SCHEMA = """
CREATE TABLE IF NOT EXISTS device (
  mac TEXT PRIMARY KEY, product TEXT NOT NULL, sn TEXT, chip TEXT,
  first_seen TEXT NOT NULL, last_seen TEXT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS device_sn ON device(product, sn) WHERE sn IS NOT NULL;
CREATE TABLE IF NOT EXISTS run (
  id INTEGER PRIMARY KEY AUTOINCREMENT, mac TEXT, sn TEXT, product TEXT NOT NULL,
  stage TEXT NOT NULL, station TEXT, operator TEXT, started TEXT NOT NULL, finished TEXT,
  result TEXT, firmware TEXT, unapproved INTEGER DEFAULT 0
);
CREATE TABLE IF NOT EXISTS step (
  run_id INTEGER NOT NULL REFERENCES run(id), idx INTEGER NOT NULL, name TEXT NOT NULL,
  type TEXT NOT NULL, result TEXT NOT NULL, detail TEXT, output TEXT, at TEXT NOT NULL,
  PRIMARY KEY (run_id, idx)
);
CREATE TABLE IF NOT EXISTS value (
  mac TEXT NOT NULL, key TEXT NOT NULL, val TEXT, run_id INTEGER, at TEXT NOT NULL,
  PRIMARY KEY (mac, key)
);
"""


def _now() -> str:
    return dt.datetime.now().astimezone().isoformat(timespec="seconds")


def default_path() -> str:
    return os.environ.get("MFLASH_DB") or os.path.join(FLASHER, "records", "mflash.db")


class Db:
    def __init__(self, path: str | None = None):
        self.path = path or default_path()
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        self._lock = threading.Lock()
        self._c = sqlite3.connect(self.path, check_same_thread=False)
        self._c.row_factory = sqlite3.Row
        self._c.executescript(SCHEMA)

    def _x(self, sql: str, args=()) -> sqlite3.Cursor:
        with self._lock:
            cur = self._c.execute(sql, args)
            self._c.commit()
            return cur

    def q(self, sql: str, args=()) -> list[sqlite3.Row]:
        with self._lock:
            return list(self._c.execute(sql, args))

    def seen(self, mac: str, product: str, chip: str = "") -> None:
        now = _now()
        self._x("INSERT INTO device(mac, product, chip, first_seen, last_seen) VALUES (?,?,?,?,?) "
                "ON CONFLICT(mac) DO UPDATE SET last_seen=excluded.last_seen, "
                "chip=COALESCE(NULLIF(excluded.chip,''), device.chip)",
                (mac, product, chip, now, now))

    def sn_owner(self, product: str, sn: str) -> str | None:
        r = self.q("SELECT mac FROM device WHERE product=? AND sn=?", (product, sn))
        return r[0]["mac"] if r else None

    def device(self, mac: str) -> sqlite3.Row | None:
        r = self.q("SELECT * FROM device WHERE mac=?", (mac,))
        return r[0] if r else None

    def set_sn(self, mac: str, sn: str) -> None:
        self._x("UPDATE device SET sn=?, last_seen=? WHERE mac=?", (sn, _now(), mac))

    def start_run(self, product: str, stage: str, mac: str, sn: str, station: str, operator: str,
                  firmware: str = "", unapproved: bool = False) -> int:
        return self._x("INSERT INTO run(mac, sn, product, stage, station, operator, started, "
                       "firmware, unapproved) VALUES (?,?,?,?,?,?,?,?,?)",
                       (mac, sn, product, stage, station, operator, _now(), firmware,
                        int(unapproved))).lastrowid

    def update_run(self, run_id: int, **kw) -> None:
        cols = ", ".join(f"{k}=?" for k in kw)
        self._x(f"UPDATE run SET {cols} WHERE id=?", (*kw.values(), run_id))

    def finish_run(self, run_id: int, result: str, mac: str = "", sn: str = "") -> None:
        self._x("UPDATE run SET finished=?, result=?, mac=COALESCE(NULLIF(?,''), mac), "
                "sn=COALESCE(NULLIF(?,''), sn) WHERE id=?", (_now(), result, mac, sn, run_id))

    def add_step(self, run_id: int, idx: int, name: str, type_: str, result: str, detail: str,
                 output: str) -> None:
        self._x("INSERT OR REPLACE INTO step VALUES (?,?,?,?,?,?,?,?)",
                (run_id, idx, name, type_, result, detail, output, _now()))

    def set_value(self, mac: str, key: str, val, run_id: int) -> None:
        v = val if isinstance(val, str) else json.dumps(val)
        self._x("INSERT OR REPLACE INTO value VALUES (?,?,?,?,?)", (mac, key, v, run_id, _now()))

    def history(self, mac_or_sn: str = "", limit: int = 50) -> list[sqlite3.Row]:
        if mac_or_sn:
            return self.q("SELECT * FROM run WHERE mac=? OR sn=? ORDER BY id DESC LIMIT ?",
                          (mac_or_sn.upper(), mac_or_sn, limit))
        return self.q("SELECT * FROM run ORDER BY id DESC LIMIT ?", (limit,))

    def steps(self, run_id: int) -> list[sqlite3.Row]:
        return self.q("SELECT * FROM step WHERE run_id=? ORDER BY idx", (run_id,))

    def export_csv(self, path: str, product: str = "") -> int:
        """One row per box: SN, MAC, the last result of each stage, its values."""
        devs = self.q("SELECT * FROM device" + (" WHERE product=?" if product else "")
                      + " ORDER BY sn, mac", (product,) if product else ())
        stages = [r["stage"] for r in self.q("SELECT DISTINCT stage FROM run ORDER BY stage")]
        keys = [r["key"] for r in self.q("SELECT DISTINCT key FROM value ORDER BY key")]
        with open(path, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.writer(f)
            w.writerow(["product", "sn", "mac", "chip", "first_seen", "last_seen"]
                       + [f"{s}_result" for s in stages] + [f"{s}_firmware" for s in stages] + keys)
            for d in devs:
                last = {}
                for s in stages:
                    r = self.q("SELECT result, firmware FROM run WHERE mac=? AND stage=? "
                               "ORDER BY id DESC LIMIT 1", (d["mac"], s))
                    last[s] = r[0] if r else None
                vals = {r["key"]: r["val"] for r in self.q("SELECT * FROM value WHERE mac=?",
                                                           (d["mac"],))}
                w.writerow([d["product"], d["sn"] or "", d["mac"], d["chip"] or "", d["first_seen"],
                            d["last_seen"]]
                           + [last[s]["result"] if last[s] else "" for s in stages]
                           + [last[s]["firmware"] if last[s] else "" for s in stages]
                           + [vals.get(k, "") for k in keys])
        return len(devs)
