"""The station's line to the boxes over Wi-Fi: the broker they already use.

A box publishes under `mcold/<sn>/` and listens under `mcold/v1/<sn>/`
(firmware/PROTOCOL.md section 6, on firmware/p7-power). The station
listens to every box of the product and keeps the last word of each:

    mcold/<sn>/online        "1" / "0" (retained; "0" is also the broker's last will)
    mcold/<sn>/status        GET_STATUS (retained)
    mcold/<sn>/settings      every setting, after each change (retained)
    mcold/<sn>/config/state  what came of a settings document (retained)
    mcold/<sn>/ota/state     what came of a firmware name (retained)
    mcold/<sn>/rpc/reply     the answer to a command sent on mcold/v1/<sn>/rpc
                             (firmware with the MQTT rpc: docs/firmware-mqtt-rpc.md)

`Link` is the broker connection, `PahoLink` the real one; sim.SimBroker
gives one in memory for the tests and the demo. `Fleet` is what the
boxes said, by SN, with a condition to wait on.
"""
from __future__ import annotations

import itertools
import json
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

Handler = Callable[[str, bytes, bool], None]     # topic, payload, retained


def topic_match(pattern: str, topic: str) -> bool:
    p, t = pattern.split("/"), topic.split("/")
    for i, part in enumerate(p):
        if part == "#":
            return True
        if i >= len(t) or (part != "+" and part != t[i]):
            return False
    return len(p) == len(t)


class LinkError(Exception):
    pass


class Link:
    """A connection to a broker: publish, and subscribe with a handler."""

    connected = False

    def publish(self, topic: str, payload: str | bytes, retain: bool = False, qos: int = 1) -> None:
        raise NotImplementedError

    def subscribe(self, pattern: str, handler: Handler) -> None:
        raise NotImplementedError

    def close(self) -> None:
        pass


class PahoLink(Link):
    def __init__(self, host: str, port: int = 1883, username: str = "", password: str = "",
                 client_id: str = "mflash", tls: bool = False):
        try:
            import paho.mqtt.client as mqtt
        except ImportError as e:      # the dock works without it; only the rack needs it
            raise LinkError("ต้องติดตั้ง paho-mqtt ก่อน (pip install paho-mqtt)") from e
        self._subs: list[tuple[str, Handler]] = []
        self._lock = threading.Lock()
        self.error = ""
        self.host, self.port = host, int(port)
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id, clean_session=True)
        if username:
            c.username_pw_set(username, password)
        if tls or self.port == 8883:
            c.tls_set()
        c.on_connect = self._on_connect
        c.on_disconnect = self._on_disconnect
        c.on_message = self._on_message
        c.reconnect_delay_set(1, 30)
        self._c = c
        c.connect_async(host, self.port, keepalive=30)
        c.loop_start()

    def _on_connect(self, c, userdata, flags, reason, props=None):
        if reason.is_failure:
            self.error = f"broker {self.host}:{self.port} ไม่รับ: {reason}"
            return
        self.connected, self.error = True, ""
        with self._lock:
            for pattern, _ in self._subs:
                c.subscribe(pattern, qos=1)

    def _on_disconnect(self, c, userdata, flags, reason, props=None):
        self.connected = False
        if reason.is_failure:
            self.error = f"หลุดจาก broker {self.host}:{self.port}: {reason}"

    def _on_message(self, c, userdata, msg):
        with self._lock:
            subs = list(self._subs)
        for pattern, h in subs:
            if topic_match(pattern, msg.topic):
                try:
                    h(msg.topic, msg.payload, bool(msg.retain))
                except Exception as e:      # one bad message must not stop the others
                    print(f"mqtt {msg.topic}: {e!r}")

    def publish(self, topic, payload, retain=False, qos=1):
        import paho.mqtt.client as mqtt
        r = self._c.publish(topic, payload, qos=qos, retain=retain)
        # not connected: paho keeps a QoS 1 message and sends it once it is back
        if r.rc != mqtt.MQTT_ERR_SUCCESS and not (r.rc == mqtt.MQTT_ERR_NO_CONN and qos > 0):
            raise LinkError(f"ส่ง {topic} ไม่ได้: {mqtt.error_string(r.rc)}")

    def subscribe(self, pattern, handler):
        with self._lock:
            self._subs.append((pattern, handler))
        if self.connected:
            self._c.subscribe(pattern, qos=1)

    def close(self):
        self._c.loop_stop()
        self._c.disconnect()


@dataclass
class NetBox:
    """What one box has said on the broker."""
    sn: str
    online: bool | None = None
    online_at: float = 0.0
    status: dict = field(default_factory=dict)
    status_at: float = 0.0
    settings: dict = field(default_factory=dict)
    config_state: dict = field(default_factory=dict)
    ota_state: dict = field(default_factory=dict)
    replies: dict = field(default_factory=dict)     # rpc id -> reply
    seen_at: float = 0.0                            # last message of any kind, live (not retained)

    def ip(self) -> str:
        return str((self.status.get("net") or {}).get("ip", ""))

    def fw(self) -> str:
        return str((self.status.get("fw") or {}).get("ver", ""))


class Fleet:
    """Every box of the product on the broker, by SN."""

    def __init__(self, link: Link, product: str = "mcold", version: str = "v1"):
        self.link = link
        self.up = product                    # what the boxes publish under
        self.down = f"{product}/{version}"   # what they listen under
        self.boxes: dict[str, NetBox] = {}
        self.cond = threading.Condition()
        self._ids = itertools.count(int(time.time()) % 100000 * 10)
        self.on_change: Callable[[str], None] | None = None
        for leaf in ("online", "status", "settings", "config/state", "ota/state", "rpc/reply"):
            link.subscribe(f"{self.up}/+/{leaf}", self._got)

    def _got(self, topic: str, payload: bytes, retained: bool) -> None:
        parts = topic.split("/")
        sn, leaf = parts[1], "/".join(parts[2:])
        text = payload.decode("utf-8", "replace").strip()
        if leaf != "online" and not text:
            return                       # a retained message cleared
        with self.cond:
            b = self.boxes.setdefault(sn, NetBox(sn))
            now = time.time()
            if not retained:
                b.seen_at = now
            if leaf == "online":
                b.online, b.online_at = text == "1", now
            else:
                try:
                    d = json.loads(text)
                except ValueError:
                    return
                if not isinstance(d, dict):
                    return
                if leaf == "status":
                    b.status, b.status_at = d, now
                elif leaf == "settings":
                    b.settings = d
                elif leaf == "config/state":
                    b.config_state = d
                elif leaf == "ota/state":
                    b.ota_state = d
                elif leaf == "rpc/reply" and "id" in d:
                    b.replies[d["id"]] = d
            self.cond.notify_all()
        if self.on_change:
            self.on_change(sn)

    def box(self, sn: str) -> NetBox:
        with self.cond:
            return self.boxes.setdefault(sn, NetBox(sn))

    def wait(self, pred: Callable[[], object], timeout: float,
             should_stop: Callable[[], bool] = lambda: False) -> object:
        """pred() under the lock until it is truthy, the time is up or the person stops."""
        end = time.monotonic() + timeout
        with self.cond:
            while True:
                v = pred()
                if v or should_stop():
                    return v
                left = end - time.monotonic()
                if left <= 0:
                    return v
                self.cond.wait(min(left, 0.5))

    # ---- to a box ----------------------------------------------------------

    def rpc(self, sn: str, cmd: str, args: dict | None = None, timeout: float = 10,
            should_stop: Callable[[], bool] = lambda: False) -> dict | None:
        rid = next(self._ids)
        msg = {"id": rid, "cmd": cmd, **(args or {})}
        self.link.publish(f"{self.down}/{sn}/rpc", json.dumps(msg), retain=False, qos=1)
        b = self.box(sn)
        r = self.wait(lambda: b.replies.get(rid), timeout, should_stop)
        with self.cond:
            b.replies.pop(rid, None)
        return r or None

    def send_config(self, sn: str, doc: dict) -> None:
        self.link.publish(f"{self.down}/{sn}/config", json.dumps(doc), retain=True)

    def send_firmware(self, sn: str, name: str) -> None:
        self.link.publish(f"{self.down}/{sn}/firmware", name, retain=True)

    def clear(self, sn: str, leaf: str) -> None:
        """Take a retained message to the box off the broker (config, firmware)."""
        self.link.publish(f"{self.down}/{sn}/{leaf}", b"", retain=True)


def connect(site, client_id: str = "") -> Link:
    """The rack's link with the station's own broker login (site.rack_broker())."""
    b = site.rack_broker()
    if not b.get("host"):
        raise LinkError("ยังไม่ได้ตั้ง MQTT broker ในหน้าตั้งค่า")
    return PahoLink(b["host"], int(b.get("port") or 1883), b.get("username", ""),
                    b.get("password", ""), client_id or f"mflash-{site.station or 'station'}")
