"""`mflash web --demo`: the whole line without a board or a broker.

The browser's USB port is simulated too: "connect" makes a blank box with a
MAC of its own, "write" gives it the firmware version and the NVS image the
page downloaded through the real image endpoints, and the box boots, joins
the simulated broker and is tested there like a real one. One box in seven
has a GNSS that sends nothing, so the page shows a failure as well.
"""
from __future__ import annotations

import os
import tempfile
import threading

from .. import firmware as fwlib
from .. import profile as proflib
from ..db import Db
from ..mqttlink import Fleet
from ..sim import SimBox, SimBroker, SimNet, build_dir
from ..site import Factory, Site, Wifi
from .line import DebugHub, Line


class DemoUsb:
    def __init__(self, line: Line, broker: SimBroker, debug_broker: SimBroker):
        self.line, self.broker, self.debug_broker = line, broker, debug_broker
        self.boxes: dict[str, SimBox] = {}
        self.made = 0
        self.lock = threading.Lock()

    def connect(self) -> dict:
        with self.lock:
            self.made += 1
            n = self.made
            mac = "28:84:85:%02X:%02X:%02X" % (0x40 + n // 256, n % 256, 0x5A)
            box = SimBox(mac=mac, slot=100 + n, gnss_dead=n % 7 == 0)
            SimNet(box, self.broker, rpc=True, debug_broker=self.debug_broker)
            self.boxes[mac] = box
        return {"mac": mac, "chip": "ESP32-S3 (QFN56) (revision v0.2)", "flash_size": "16MB"}

    def factory_on(self, sn: str) -> dict:
        """`factory on` at the console of a box on the cable (the demo: any box it made)."""
        box = next((b for b in self.boxes.values() if b.sn == sn), None)
        if not box:
            raise RuntimeError(f"ไม่มีกล่อง {sn} เสียบอยู่")
        box.factory_mode = True
        return {"ok": True, "reply": "  factory mode on"}

    def write(self, token: str) -> dict:
        with self.line.lock:
            pr = self.line.prepared.get(token)
        if not pr:
            raise RuntimeError("ไม่รู้จักงาน flash นี้")
        box = self.boxes.get(pr.mac)
        if not box:
            raise RuntimeError("กล่องจำลองนี้ไม่ได้เสียบอยู่")
        if pr.erase_all:
            box.erase()
        box.flash = {0x20000: "firmware.bin"}
        box.version = pr.firmware["version"]
        box.apply_nvs(self.line.image(token, "nvs.bin"))
        box.state = "app"
        box.boot()                     # joins the broker in a moment, like a real one
        return {"ok": True}


def setup(product: str = "mcold", speed: float = 0.3) -> tuple[Line, DemoUsb]:
    root = tempfile.mkdtemp(prefix="mflash-web-demo-")
    os.environ["MFLASH_LIBRARY"] = os.path.join(root, "library")
    p = proflib.load(product)
    for kind, ver in (("production", "0.7.0-dev.61"), ("production", "0.7.0-demo")):
        d = build_dir(os.path.join(root, f"build-{ver}"), ver, kind, proflib.REPO)
        m = fwlib.import_build(p, kind, d, {"build_dir": "demo"})
        if not m.get("approved"):
            fwlib.approve(product, kind, m["id"], "demo")
    site = Site(station="demo-web", wifi=[Wifi("Customer-WiFi", "demo-password")],
                mqtt={"host": "broker.example", "port": 1883, "username": "demo", "password": "demo-secret"},
                ota_base="https://files.example/mcold",
                factory=Factory("etemp", "line-password", "10.20.0.100", "10.20.0.1", "255.255.255.0"),
                debug_mqtt={"host": "debug.example", "port": 1883, "username": "demo", "password": "debug-secret"},
                path=os.path.join(root, "site.ini"))
    broker = SimBroker(speed=speed, in_range={"etemp"})
    debug_broker = SimBroker(speed=speed)
    line = Line(product, Db(os.path.join(root, "demo.db")), site,
                fleet=Fleet(broker.connect("mflash-web-demo")),
                debug=DebugHub(debug_broker.connect("mflash-web-debug")),
                state_path=os.path.join(root, "web.json"), time_scale=max(speed, 0.05),
                debug_dir=os.path.join(root, "debug"), demo=True,
                soak_interval=3.0)                       # the demo reads every 3 s, not every minute
    line.set_lot("mCDV1-L0169-1069")
    # the USB flash writes the older build; the line ends on the newer one by OTA
    older = [m for m in fwlib.list_firmware(product, "production") if m["version"] == "0.7.0-dev.61"][0]
    line.set_firmware(usb=older["id"], final="approved")
    return line, DemoUsb(line, broker, debug_broker)
