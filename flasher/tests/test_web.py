"""The line's web app: add boxes over a (simulated) USB port, test them over MQTT."""
import os
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

pytest.importorskip("fastapi")
pytest.importorskip("httpx")
from fastapi.testclient import TestClient  # noqa: E402

from mflash import nvsimage  # noqa: E402
from mflash.web.app import create_app  # noqa: E402
from mflash.web.demo import setup  # noqa: E402

LOT = "mCDV1-L0169-1069"


@pytest.fixture
def web(monkeypatch):
    line, usb = setup(speed=0)
    line.rack.time_scale = 0.05
    c = TestClient(create_app(line, usb))
    yield c, line, usb
    os.environ.pop("MFLASH_LIBRARY", None)


def add(c, unit=None, wifi=("etemp",)):
    """What the page does: connect, prepare, download every image, write, report."""
    port = c.post("/api/demo/usb/connect").json()
    r = c.post("/api/usb/prepare", json={"mac": port["mac"], "lot": LOT, "unit": unit,
                                         "wifi": list(wifi), "operator": "somchai"})
    assert r.status_code == 200, r.json()
    prep = r.json()
    files = {i["name"]: c.get(i["url"]).content for i in prep["images"]}
    assert [i["offset"] for i in prep["images"]] == sorted(i["offset"] for i in prep["images"])
    assert c.post("/api/demo/usb/write", json={"token": prep["token"], "ok": True}).status_code == 200
    d = c.post("/api/usb/done", json={"token": prep["token"], "ok": True, "log": "wrote 5 files"}).json()
    return prep, files, d


def wait(fn, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError("timed out")


def test_add_device_writes_firmware_and_settings(web):
    c, line, usb = web
    prep, files, done = add(c)
    assert prep["sn"] == f"{LOT}-001" and prep["factory_ip"] == "10.20.0.101"
    assert set(files) == {"bootloader.bin", "partitions.bin", "ota_data_initial.bin", "firmware.bin", "nvs.bin"}
    nvs = nvsimage.read(files["nvs.bin"])
    assert nvs["sys"]["sn"] == f"{LOT}-001" and nvs["net"]["s0"] == "etemp"
    assert nvs["mqtt"]["host"] == "broker.example" and nvs["factory"]["on"] == 1
    assert done["next_unit"] == 2
    # the image is gone once it is used: a token is for one flash
    assert c.get(prep["images"][0]["url"]).status_code == 404
    # the next box gets the next number; a taken number is refused
    assert add(c)[0]["sn"] == f"{LOT}-002"
    port = c.post("/api/demo/usb/connect").json()
    r = c.post("/api/usb/prepare", json={"mac": port["mac"], "lot": LOT, "unit": 1, "wifi": ["etemp"]})
    assert r.status_code == 409 and "ใช้แล้ว" in r.json()["error"]
    # the box comes up on the broker with its SN and the unit's address
    rows = wait(lambda: [x for x in c.get("/api/devices").json()["rows"] if x["online"]] or None)
    assert rows[0]["ip"] == "10.20.0.101" and rows[0]["state"] == "online"


def test_reflash_of_a_known_box_keeps_its_sn(web):
    c, line, usb = web
    prep, _, _ = add(c)
    r = c.post("/api/usb/prepare", json={"mac": prep["mac"], "lot": LOT, "unit": 9, "wifi": ["etemp"]})
    assert r.json()["sn"] == prep["sn"] and r.json()["existing"]


def test_line_tests_confirm_ota_and_handover(web):
    c, line, usb = web
    for _ in range(2):
        add(c)
    wait(lambda: len([x for x in c.get("/api/devices").json()["rows"] if x["online"]]) == 2)
    assert c.post("/api/line/start", json={"operator": "somchai"}).json()["started"] == 2
    asked = set()

    def step():
        d = c.get("/api/devices").json()
        for q in d["questions"]:
            asked.add(q["text"].split(":")[0] if ":" in q["text"] else q["text"][:20])
            c.post("/api/line/answer", json={"qid": q["id"], "value": True})
        return all(r["state"] in ("pass", "fail") for r in d["rows"]) and d["rows"]
    rows = wait(step, 60)
    assert [r["state"] for r in rows] == ["pass", "pass"], rows
    assert len(asked) >= 2                                   # visual checks were asked
    for r in rows:
        b = usb.boxes[r["mac"]]
        assert b.version == "0.7.0-demo"                     # OTA'd to the final firmware
        # tested, still at the company: on the line's Wi-Fi, online, debug possible
        assert b.wifi == ["etemp"] and b.factory_mode
    assert all(r["online"] for r in rows)
    sn = rows[0]["sn"]
    assert c.post(f"/api/devices/{sn}/debug", json={"on": True, "minutes": 1}).json()["on"] is True
    c.post(f"/api/devices/{sn}/debug", json={"on": False})
    sm = c.get("/api/summary", params={"lot": LOT}).json()
    assert sm["counts"]["PASS"] == 2
    row = sm["rows"][0]
    assert row["stages"]["usb"] == "PASS" and row["stages"]["line"] == "PASS" and row["stages"]["ship"] == ""
    # shipping: the customer's Wi-Fi in, the line's out, factory mode off
    assert c.post("/api/ship/start", json={"operator": "somchai"}).json()["started"] == 2
    rows = wait(lambda: (lambda d: d["rows"] if all(r["state"] == "shipped" for r in d["rows"]) else None)(
        c.get("/api/devices").json()), 30)
    for r in rows:
        b = usb.boxes[r["mac"]]
        # five slots: the line's network stays (on DHCP now), the customer's is added
        assert b.wifi == ["etemp", "Customer-WiFi"] and b.wifi_ip["etemp"] == ""
        assert not b.factory_mode and r["online"]          # still reachable at the company
    assert "factory_ip" not in line.db.values(rows[0]["mac"])   # its fixed address is free again
    log = c.get(f"/api/devices/{rows[0]['mac']}/log").json()
    assert [x["stage"] for x in log] == ["usb", "line", "ship"]



def test_a_no_fails_the_box(web):
    c, line, usb = web
    add(c)
    wait(lambda: [x for x in c.get("/api/devices").json()["rows"] if x["online"]])
    c.post("/api/line/start", json={})

    def step():
        d = c.get("/api/devices").json()
        for q in d["questions"]:
            c.post("/api/line/answer", json={"qid": q["id"], "value": "ไฟ" not in q["text"]})
        return d["rows"][0]["state"] in ("pass", "fail") and d["rows"][0]
    r = wait(step, 60)
    assert r["state"] == "fail" and r["fail"]["detail"] == "operator: no"


def test_debug_stream(web):
    c, line, usb = web
    prep, _, _ = add(c)
    wait(lambda: [x for x in c.get("/api/devices").json()["rows"] if x["online"]])
    assert c.post(f"/api/devices/{prep['sn']}/debug", json={"on": True, "minutes": 1}).json()["on"] is True
    d = wait(lambda: (lambda d: d if d["on"] and d["messages"] else None)(
        c.get(f"/api/devices/{prep['sn']}/debug").json()), 10)
    assert {m["ch"] for m in d["messages"]} >= {"power", "sys", "net"}   # dev.66 channels
    power = [m for m in d["messages"] if m["ch"] == "power"][0]["data"]
    assert len(power["rows"]) == 10 and "ma" in power["rows"][0]           # ten points a message (100 ms)
    assert {"mv", "soc", "charge", "mono", "period_ms"} <= set(power)
    with c.websocket_connect(f"/api/devices/{prep['sn']}/debug/ws") as ws:
        assert ws.receive_json()["messages"]
    c.post(f"/api/devices/{prep['sn']}/debug", json={"on": False})
    wait(lambda: not c.get(f"/api/devices/{prep['sn']}/debug").json()["on"])


def test_settings_and_firmware_choice(web):
    c, line, usb = web
    s = c.get("/api/settings").json()
    assert s["debug_mqtt"]["host"] == "debug.example" and s["debug_mqtt"]["has_password"]
    assert "debug-secret" not in str(s) and "line-password" not in str(s)
    assert c.post("/api/lot", json={"lot": "bad"}).status_code == 409
    opts = c.get("/api/config").json()["firmware_options"]
    assert c.post("/api/firmware", json={"final": opts[-1]["id"]}).json() == {"ok": True}
    assert c.get("/api/config").json()["firmware"]["final"]["id"] == opts[-1]["id"]
    assert c.get("/").status_code == 200 and "mCOLD" in c.get("/").text


def test_firmware_upload_from_the_page(web, tmp_path):
    import io
    import zipfile
    from mflash import profile as proflib
    from mflash.sim import build_dir
    c, line, usb = web
    d = build_dir(str(tmp_path / "b"), "0.7.0-dev.99", "production", proflib.REPO)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for n in ("bootloader.bin", "partitions.bin", "ota_data_initial.bin", "firmware.bin"):
            z.write(os.path.join(d, n), f".pio/build/mcold/{n}")
    r = c.post("/api/firmware/import", content=buf.getvalue(), headers={"Content-Type": "application/zip"})
    assert r.status_code == 200 and r.json()["version"] == "0.7.0-dev.99" and not r.json()["approved"]
    assert c.post("/api/firmware/approve", json={"id": r.json()["id"], "by": ""}).status_code == 409
    assert c.post("/api/firmware/approve", json={"id": r.json()["id"], "by": "somchai"}).json()["approved"]
    assert "0.7.0-dev.99" in [o["version"] for o in c.get("/api/config").json()["firmware_options"]]
    bad = c.post("/api/firmware/import", content=b"not a zip", headers={"Content-Type": "application/zip"})
    assert bad.status_code == 409 and "zip" in bad.json()["error"]


def test_firmware_without_diag_run_uses_the_fallbacks(web):
    """dev.65 has FACTORY_TEST and IDENTIFY but no DIAG_RUN: tested with those, not passed blind."""
    c, line, usb = web
    prep, _, _ = add(c)
    usb.boxes[prep["mac"]].diag = False
    wait(lambda: [x for x in c.get("/api/devices").json()["rows"] if x["online"]])
    c.post("/api/line/start", json={})
    asked = []

    def step():
        d = c.get("/api/devices").json()
        for q in d["questions"]:
            asked.append(q["text"])
            c.post("/api/line/answer", json={"qid": q["id"], "value": True})
        return d["rows"][0]["state"] in ("pass", "fail") and d["rows"][0]
    r = wait(step, 60)
    assert r["state"] == "pass", r
    assert any("กะพริบขาวครบ" in t for t in asked) and any("หน้าปกติ" in t for t in asked)
    steps = c.get(f"/api/devices/{prep['mac']}/log").json()[-1]["steps"]
    auto = [s for s in steps if s["name"].startswith("ทดสอบโมดูลอัตโนมัติ")][0]
    assert auto["result"] == "PASS"



def test_nvs_carries_the_debug_broker(web):
    c, line, usb = web
    prep, files, _ = add(c)
    dbg = nvsimage.read(files["nvs.bin"])["dbg"]
    assert dbg == {"host": "debug.example", "port": 1883, "user": "demo", "pass": "debug-secret"}
    assert "tls" not in dbg                                   # dev.66 does not read it yet


def test_debug_without_a_debug_broker_says_why(web):
    c, line, usb = web
    prep, _, _ = add(c)
    wait(lambda: [x for x in c.get("/api/devices").json()["rows"] if x["online"]])
    usb.boxes[prep["mac"]].dbg = {}                            # flashed before the debug broker was set
    r = c.post(f"/api/devices/{prep['sn']}/debug", json={"on": True, "minutes": 1})
    assert r.status_code == 409 and "ไม่มี debug broker" in r.json()["error"]



def test_line_and_customer_wifi_from_the_start(web):
    """Both networks written at the USB flash: shipping only turns factory mode off."""
    c, line, usb = web
    prep, files, _ = add(c, wifi=("etemp", "Customer-WiFi"))
    net = nvsimage.read(files["nvs.bin"])["net"]
    assert (net["s0"], net["s1"]) == ("etemp", "Customer-WiFi")
    port = c.post("/api/demo/usb/connect").json()
    r = c.post("/api/usb/prepare", json={"mac": port["mac"], "lot": LOT,
                                         "wifi": ["etemp", "Customer-WiFi"] * 3})
    assert r.status_code == 409 and "5" in r.json()["error"]


def test_debug_view_has_the_status_and_get_debug(web):
    c, line, usb = web
    prep, _, _ = add(c)
    wait(lambda: [x for x in c.get("/api/devices").json()["rows"] if x["online"]])
    d = c.get(f"/api/devices/{prep['sn']}/debug").json()
    assert d["on"] is False and d["status_at"] and d["status"]["net"]["ip"] == "10.20.0.101"
    r = c.post(f"/api/devices/{prep['sn']}/get_debug").json()
    assert {"internal", "coredump", "uptime_s", "reset"} <= set(r)
