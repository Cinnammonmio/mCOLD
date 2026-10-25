"""The station's window: six slots in a page served from this machine.

    mflash gui                 the real bus; opens a window
    mflash gui --demo          six simulated boxes, for training and trying the profile

The page is plain HTML from this package, served by the standard library
on 127.0.0.1 -- nothing to install. On Windows it opens in Edge's app
mode, which looks and behaves like a window of its own; elsewhere in the
default browser. The page asks for the state twice a second and posts
the person's clicks back; everything else happens in mflash itself.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from . import profile as proflib
from . import site as sitelib
from .db import Db
from .station import Station

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(proflib.REPO, "mcold-spec", "fonts")
FONT_FILES = {"IBMPlexSansThai-Regular.ttf", "IBMPlexSansThai-Medium.ttf",
              "IBMPlexSansThai-SemiBold.ttf", "IBMPlexSansThai-Bold.ttf"}


def make_handler(station: Station):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):   # the console is for the station, not for polls
            pass

        def _send(self, code: int, body: bytes, ctype: str, cache: bool = False) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "max-age=86400" if cache else "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, obj, code: int = 200) -> None:
            self._send(code, json.dumps(obj, ensure_ascii=False).encode(), "application/json")

        def do_GET(self):
            u = urlparse(self.path)
            if u.path in ("/", "/index.html"):
                with open(os.path.join(HERE, "gui.html"), "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
            elif u.path == "/api/state":
                q = parse_qs(u.query)
                logs = {int(x) for x in q.get("log", [""])[0].split(",") if x.isdigit()}
                self._json(station.state(log_slots=logs))
            elif u.path == "/api/history":
                self._json(station.history())
            elif u.path in ("/api/summary", "/api/summary.csv"):
                lot = parse_qs(u.query).get("lot", [None])[0]
                try:
                    if u.path.endswith(".csv"):
                        body = station.summary_csv(lot).encode("utf-8-sig")
                        name = f"SN_{lot or station.lot or 'all'}.csv"
                        self.send_response(200)
                        self.send_header("Content-Type", "text/csv; charset=utf-8")
                        self.send_header("Content-Disposition", f'attachment; filename="{name}"')
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                    else:
                        self._json(station.summary(lot))
                except RuntimeError as e:
                    self._json({"error": str(e)}, 409)
            elif u.path.startswith("/fonts/") and os.path.basename(u.path) in FONT_FILES:
                path = os.path.join(FONTS, os.path.basename(u.path))
                if os.path.isfile(path):
                    with open(path, "rb") as f:
                        self._send(200, f.read(), "font/ttf", cache=True)
                else:
                    self._send(404, b"", "text/plain")
            else:
                self._send(404, b"not found", "text/plain")

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            try:
                a = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                return self._json({"error": "bad json"}, 400)
            path = urlparse(self.path).path
            try:
                if path == "/api/start":
                    station.start(int(a["slot"]), a.get("operator", ""))
                elif path == "/api/start_all":
                    return self._json({"started": station.start_all(a.get("operator", ""))})
                elif path == "/api/stop":
                    station.stop(int(a["slot"]))
                elif path == "/api/clear":
                    station.clear(int(a["slot"]))
                elif path == "/api/answer":
                    station.answer(int(a["slot"]), int(a["qid"]), a.get("value"))
                elif path == "/api/config":
                    if a.get("product"):
                        station.set_product(a["product"])
                    if a.get("stage"):
                        station.set_stage(a["stage"])
                elif path == "/api/lot":
                    station.set_lot(str(a.get("lot", "")))
                elif path == "/api/unit":
                    if "step" in a:
                        station.step_unit(int(a["slot"]), 1 if int(a["step"]) > 0 else -1)
                    else:
                        station.set_unit(int(a["slot"]), int(a["unit"]))
                elif path == "/api/layout/save":
                    return self._json({"saved": station.save_layout()})
                elif path == "/api/layout/forget":
                    station.forget_layout()
                else:
                    return self._json({"error": "unknown"}, 404)
            except (RuntimeError, KeyError, ValueError, proflib.ProfileError) as e:
                return self._json({"error": str(e)}, 409)
            self._json({"ok": True})

    return Handler


def open_window(url: str) -> None:
    """Edge's app mode on Windows (a window with no tabs or address bar), else a browser."""
    candidates = [shutil.which("msedge"),
                  os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe"),
                  os.path.expandvars(r"%ProgramFiles%\Microsoft\Edge\Application\msedge.exe")]
    if sys.platform == "win32":
        for exe in candidates:
            if exe and os.path.isfile(exe):
                subprocess.Popen([exe, f"--app={url}", "--window-size=1600,960"])
                return
    webbrowser.open(url)


def serve(product: str = "mcold", host: str = "127.0.0.1", port: int = 8765, demo: bool = False,
          browser: bool = True, db_path: str | None = None, site_path: str | None = None,
          speed: float = 0.4) -> None:
    if demo:
        from .sim import SimHardware, demo_setup
        from .site import Site, Wifi
        root = tempfile.mkdtemp(prefix="mflash-demo-")
        bus = demo_setup(root, speed=speed)
        site = Site(station="demo", wifi=[Wifi("Factory-AP", "demo-password")],
                    mqtt={"host": "broker.example", "port": 1883, "username": "demo",
                          "password": "demo-secret"})
        station = Station(product, Db(os.path.join(root, "demo.db")), site, hw=SimHardware(bus),
                          layout_path=os.path.join(root, "slots.json"), demo=True)
        for i, s in enumerate(station.slots):     # a dock whose bays have been saved
            s.location = f"1-1.{i + 1}"
        station.on_clear = bus.replace
        station.set_lot("mCDV1-L0169-1069")
    else:
        station = Station(product, Db(db_path), sitelib.load(site_path))
    station.watch()
    httpd = ThreadingHTTPServer((host, port), make_handler(station))
    url = f"http://{'127.0.0.1' if host in ('0.0.0.0', '') else host}:{httpd.server_port}/"
    print(f"mflash station{' (DEMO: simulated boxes)' if demo else ''}: {url}")
    if host not in ("127.0.0.1", "localhost"):
        print("  !! open to the network: anyone who can reach this port can run the station")
    print("  Ctrl+C to stop")
    if browser:
        threading.Timer(0.5, open_window, args=(url,)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("stopping")
        busy = [s.index + 1 for s in station.slots if s.busy]
        if busy:
            print(f"  slots {busy} were still running; a box stopped mid-flash stays in "
                  "download mode -- run it again")
