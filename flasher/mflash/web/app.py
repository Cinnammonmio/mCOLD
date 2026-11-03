"""The line's web app over HTTP (FastAPI). API docs at /docs.

    mflash web                     site.ini and records/ of this machine, the real broker
    mflash web --demo              simulated broker and boxes, a simulated USB port too

The page itself is static (static/): HTML and JavaScript, no build step.
USB is the browser's (Web Serial + esptool-js, Chrome or Edge), so the page
must be served over https -- or from localhost -- to reach a port.
There is no login here: the server team puts the main website's in front.
"""
from __future__ import annotations

import asyncio
import os

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, PlainTextResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from .. import firmware as fwlib
from .. import profile as proflib
from .line import Line

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(HERE, "static")
FONTS = os.path.join(proflib.REPO, "mcold-spec", "fonts")


class Prepare(BaseModel):
    mac: str
    lot: str = ""
    unit: int | None = None
    wifi: list[str] = []
    erase_all: bool | None = None
    operator: str = ""
    chip: str = ""


class Done(BaseModel):
    token: str
    ok: bool
    log: str = ""
    error: str = ""


class Start(BaseModel):
    sns: list[str] = []
    operator: str = ""


class Answer(BaseModel):
    qid: int
    value: bool | str


class Toggle(BaseModel):
    on: bool
    operator: str = ""
    minutes: int = 60


class Lot(BaseModel):
    lot: str


class FirmwareChoice(BaseModel):
    usb: str | None = None
    final: str | None = None


def create_app(line: Line, demo_usb=None) -> FastAPI:
    app = FastAPI(title="mCOLD Line", version="1.0",
                  description="Add mCOLD boxes over USB (in the browser), test them over MQTT.")

    def call(fn, *a, **kw):
        try:
            return fn(*a, **kw)
        except (RuntimeError, fwlib.FirmwareError, proflib.ProfileError, ValueError) as e:
            raise HTTPException(409, str(e)) from e
        except KeyError as e:
            raise HTTPException(404, str(e).strip("'\"")) from e

    @app.exception_handler(HTTPException)
    async def _err(request, exc: HTTPException):
        return JSONResponse({"error": exc.detail}, status_code=exc.status_code)

    # ---- the page -------------------------------------------------------------------
    @app.get("/", include_in_schema=False)
    def index():
        return FileResponse(os.path.join(STATIC, "index.html"), headers={"Cache-Control": "no-store"})

    app.mount("/static", StaticFiles(directory=STATIC), name="static")
    if os.path.isdir(FONTS):
        app.mount("/fonts", StaticFiles(directory=FONTS), name="fonts")

    # ---- configuration --------------------------------------------------------------
    @app.get("/api/config")
    def config():
        return line.config()

    @app.post("/api/lot")
    def set_lot(b: Lot):
        call(line.set_lot, b.lot)
        return {"ok": True, "next_unit": line.next_unit()}

    @app.post("/api/firmware")
    def set_firmware(b: FirmwareChoice):
        call(line.set_firmware, b.usb, b.final)
        return {"ok": True}

    @app.get("/api/firmware/library")
    def library():
        return line.library()

    @app.post("/api/firmware/import")
    async def import_fw(request: Request, approve_by: str = ""):
        """Body: a zip of the build folder (bootloader.bin, partitions.bin, ota_data_initial.bin,
        firmware.bin), Content-Type application/zip."""
        data = await request.body()
        return call(line.import_zip, data, approve_by)

    @app.post("/api/firmware/approve")
    def approve(b: dict):
        return call(line.approve, str(b.get("id", "")), str(b.get("by", "")))

    @app.get("/api/settings")
    def settings():
        return line.settings()

    @app.post("/api/settings")
    def save_settings(d: dict):
        return {"saved": call(line.save_settings, d)}

    # ---- adding a device over USB -----------------------------------------------------
    @app.get("/api/next_unit")
    def next_unit(lot: str = "", after: int = 0):
        return {"unit": line.next_unit(lot or None, after)}

    @app.post("/api/usb/prepare")
    def prepare(b: Prepare):
        return call(line.prepare, b.mac, b.lot, b.unit, b.wifi, b.erase_all, b.operator, b.chip)

    @app.get("/api/usb/image/{token}/{name}")
    def image(token: str, name: str):
        data = call(line.image, token, name)
        return Response(data, media_type="application/octet-stream", headers={"Cache-Control": "no-store"})

    @app.post("/api/usb/done")
    def done(b: Done):
        return call(line.done, b.token, b.ok, b.log, b.error)

    # ---- devices and tests --------------------------------------------------------------
    @app.get("/api/devices")
    def devices(lot: str | None = None, log: str = ""):
        return line.devices(lot, log)

    @app.get("/api/devices/{mac}/log")
    def device_log(mac: str):
        return line.device_log(mac)

    @app.delete("/api/devices/{mac}")
    def delete(mac: str, by: str = ""):
        return {"deleted": call(line.delete, mac, by)}

    @app.post("/api/line/start")
    def start(b: Start):
        return {"started": call(line.start, b.sns or None, b.operator)}

    @app.post("/api/ship/start")
    def ship(b: Start):
        """The customer's Wi-Fi in, the line's out, factory mode off: the box leaves the company."""
        return {"started": call(line.ship, b.sns or None, b.operator)}

    @app.post("/api/line/stop/{sn}")
    def stop(sn: str):
        call(line.stop, sn)
        return {"ok": True}

    @app.post("/api/line/answer")
    def answer(b: Answer):
        call(line.answer, b.qid, b.value)
        return {"ok": True}

    @app.post("/api/line/auto")
    def auto(b: Toggle):
        line.set_auto(b.on, b.operator)
        return {"ok": True}

    # ---- the debug stream ---------------------------------------------------------------
    @app.post("/api/devices/{sn}/debug")
    def debug(sn: str, b: Toggle):
        return call(line.set_debug, sn, b.on, b.minutes)

    @app.get("/api/devices/{sn}/debug")
    def debug_poll(sn: str, since: int = 0):
        return line.debug_since(sn, since)

    @app.post("/api/devices/{sn}/get_debug")
    def get_debug(sn: str):
        return call(line.get_debug, sn)

    @app.websocket("/api/devices/{sn}/debug/ws")
    async def debug_ws(ws: WebSocket, sn: str):
        """The stream as it comes, and the box's status whenever a new one arrives."""
        await ws.accept()
        seq, status_at, first = 0, -1.0, True
        try:
            while True:
                d = line.debug_since(sn, seq)
                if d["messages"] or d["status_at"] != status_at or first:
                    seq = d["messages"][-1]["seq"] if d["messages"] else seq
                    status_at, first = d["status_at"], False
                    await ws.send_json(d)
                await asyncio.sleep(0.4)
        except (WebSocketDisconnect, RuntimeError):
            return

    # ---- records --------------------------------------------------------------------
    @app.get("/api/summary")
    def summary(lot: str | None = None):
        return call(line.summary, lot)

    @app.get("/api/summary.csv")
    def summary_csv(lot: str | None = None):
        body = call(line.summary_csv, lot)
        name = f"SN_{lot or line.lot or 'all'}.csv"
        return PlainTextResponse("﻿" + body, media_type="text/csv; charset=utf-8",
                                 headers={"Content-Disposition": f'attachment; filename="{name}"'})

    @app.get("/api/history")
    def history():
        return line.history()

    # ---- the demo's USB port ----------------------------------------------------------
    if demo_usb:
        @app.post("/api/demo/usb/connect")
        def demo_connect():
            return demo_usb.connect()

        @app.post("/api/demo/usb/write")
        def demo_write(b: Done):
            return call(demo_usb.write, b.token)

    return app


def build(demo: bool = False, product: str = "mcold", db_path: str | None = None,
          site_path: str | None = None, speed: float = 0.3):
    """The app, with its line, from this machine's settings or the demo's."""
    if demo:
        from .demo import setup
        line, usb = setup(product, speed)
        line.rack.watch(1.0)
        return create_app(line, usb)
    from .. import site as sitelib
    from ..db import Db
    from ..mqttlink import Fleet, LinkError, PahoLink, connect
    from .line import DebugHub
    site = sitelib.load(site_path)
    fleet = debug = None
    err = ""
    try:
        fleet = Fleet(connect(site, "mflash-web-" + (site.station or "line")))
    except LinkError as e:
        err = str(e)
    if site.debug_mqtt.get("host"):
        d = site.debug_mqtt
        try:
            debug = DebugHub(PahoLink(d["host"], int(d.get("port") or 1883), d.get("username", ""),
                                      d.get("password", ""), "mflash-web-debug"))
        except LinkError:
            debug = None
    line = Line(product, Db(db_path), site, fleet=fleet, debug=debug, link_error=err)
    if line.rack:
        line.rack.watch(2.0)
    return create_app(line)


def main(argv=None) -> None:
    import argparse

    import uvicorn
    ap = argparse.ArgumentParser(prog="mflash web", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--demo", action="store_true", help="simulated broker, boxes and USB port")
    ap.add_argument("--host", default="127.0.0.1", help="0.0.0.0 behind the https proxy")
    ap.add_argument("--port", type=int, default=8800)
    ap.add_argument("--product", default="mcold")
    ap.add_argument("--db")
    ap.add_argument("--site")
    ap.add_argument("--speed", type=float, default=0.3, help="demo: delays x this")
    a = ap.parse_args(argv)
    app = build(a.demo, a.product, a.db, a.site, a.speed)
    print(f"mCOLD Line web{' (DEMO)' if a.demo else ''}: http://{a.host}:{a.port}/  (API: /docs)")
    uvicorn.run(app, host=a.host, port=a.port, log_level="warning")


if __name__ == "__main__":
    main()
