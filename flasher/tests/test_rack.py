"""The quick route: a short USB step at the dock, the rest on the rack over MQTT."""
import json
import os
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mflash.db import Db  # noqa: E402
from mflash.mqttlink import Fleet, topic_match  # noqa: E402
from mflash.remote import Rack, check_fields  # noqa: E402
from mflash.sim import SimBox, SimBroker, SimBus, SimHardware, SimNet, demo_setup  # noqa: E402
from mflash.site import Factory, Site, Wifi, validate  # noqa: E402
from mflash.station import Station  # noqa: E402

LOT = "mCDV1-L0169-1069"
LINE = "mCOLD-Line"


def make_site(tmp_path, customer=True):
    return Site(station="t", wifi=[Wifi("Customer-WiFi", "customer-pw")] if customer else [],
                mqtt={"host": "broker.example", "port": 1883, "username": "u", "password": "broker-pw"},
                factory=Factory(LINE, "line-password", "10.20.0.100", "10.20.0.1", "255.255.255.0"),
                path=str(tmp_path / "site.ini"))


@pytest.fixture
def line(tmp_path, monkeypatch):
    """A dock of simulated boxes on a broker, a station in the quick mode and its rack."""
    monkeypatch.setenv("MFLASH_LIBRARY", str(tmp_path / "library"))
    broker = SimBroker(in_range={LINE})
    bus = demo_setup(str(tmp_path), speed=0, broker=broker)
    st = Station("mcold", Db(str(tmp_path / "r.db")), make_site(tmp_path), hw=SimHardware(bus),
                 layout_path=str(tmp_path / "slots.json"))
    for i, s in enumerate(st.slots):
        s.location = f"1-1.{i + 1}"
    st.on_clear = bus.replace
    st.refresh()
    st.set_lot(LOT)
    st.set_stage("usb_quick")
    rack = Rack(st, Fleet(broker.connect("station")), time_scale=0.02,
                debug_dir=str(tmp_path / "debug"))
    return st, bus, broker, rack


def wait_idle(st, timeout=20):
    end = time.monotonic() + timeout
    while any(s.busy for s in st.slots):
        assert time.monotonic() < end, "dock still running"
        assert not any(s.question for s in st.slots), "the quick route asked a question"
        time.sleep(0.01)


def run_rack(rack, answer=lambda q: True, again_once=(), timeout=30):
    """Play the person at the rack's question queue until every box is done."""
    asked_again = set()
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        rack.refresh()
        for q in list(rack.questions):
            if q.sn in again_once and q.sn not in asked_again:
                asked_again.add(q.sn)
                rack.answer(q.id, "again")
            else:
                try:
                    rack.answer(q.id, answer(q))
                except RuntimeError:
                    pass
        bs = list(rack.boxes.values())
        if bs and all(b.state in ("pass", "fail", "stopped") for b in bs) and not any(b.busy for b in bs):
            return
        time.sleep(0.01)
    raise AssertionError({b.sn: (b.state, b.step_name) for b in rack.boxes.values()})


def unplug_all(st):
    for s in st.slots:
        if s.state in ("pass", "fail"):
            st.clear(s.index)


def test_topic_match():
    assert topic_match("mcold/+/online", "mcold/X-001/online")
    assert not topic_match("mcold/+/online", "mcold/X-001/ota/state")
    assert topic_match("mcold/v1/X/#", "mcold/v1/X/rpc")


def test_ip_pool_settings():
    f = Factory(LINE, "line-password", "10.20.0.100", "10.20.0.1", "255.255.255.0")
    assert f.ip_for(7) == "10.20.0.107" and f.capacity() == 154
    assert validate(Site(factory=f)) == []
    bad = validate(Site(factory=Factory(LINE, "pw", "10.20.0.0", "10.20.0.5", "255.255.255.0")))
    assert any("8–63" in b for b in bad) and any("gateway" in b for b in bad)
    assert any("ซ้ำ" in b for b in validate(Site(wifi=[Wifi(LINE, "x" * 8)], factory=f)))


def test_quick_route_end_to_end(line):
    st, bus, broker, rack = line
    assert st.state()["slots"][0]["ip"] == "10.20.0.101"
    assert st.start_all("somchai") == 5
    wait_idle(st)
    assert [s.state for s in st.slots] == ["pass", "pass", "pass", "empty", "pass", "pass"]
    # the USB step: no bringup, the line's Wi-Fi at the unit's address, the customer's not yet
    b0 = bus.boxes[0]
    assert "wifi ip mCOLD-Line 10.20.0.101 10.20.0.1 255.255.255.0" in b0.sent
    assert b0.wifi == [LINE] and b0.sn == f"{LOT}-001"
    assert not any(c.startswith(("scan", "walk", "logtest")) for c in b0.sent)
    assert st.db.values(b0.mac)["factory_ip"] == "10.20.0.101"
    # off the dock, onto the rack: each box runs by itself as it comes online
    on_dock = list(bus.boxes)
    unplug_all(st)
    run_rack(rack)
    res = {b.unit: b.state for b in rack.boxes.values()}
    assert res == {1: "pass", 2: "pass", 3: "pass", 4: "pass", 5: "fail"}
    bad = rack.boxes[f"{LOT}-005"]
    assert bad.fail["step"].startswith("ทดสอบโมดูล") and "gnss.ok" in bad.fail["detail"]
    # hand-over: the customer's network in, the line's out, the box off the line's Wi-Fi
    assert on_dock[0].wifi == ["Customer-WiFi"] and not on_dock[0].factory_mode
    nb = rack.fleet.boxes[f"{LOT}-001"]
    assert nb.online is False
    assert f"mcold/v1/{LOT}-001/config" not in broker.retained       # cleared after
    assert "factory_ip" not in st.db.values(on_dock[0].mac)         # its address is free again
    # the failed box keeps the line's Wi-Fi and its address, to be looked at
    assert rack.fleet.boxes[f"{LOT}-005"].online is True
    # debug was pulled to a file
    d = json.load(open(st.db.values(on_dock[0].mac)["debug_file"]))
    assert d["debug"]["coredump"] is False and d["status"]["net"]["ip"] == "10.20.0.101"
    # the SN summary sees both halves
    sm = st.summary()
    assert sm["counts"] == {"PASS": 4, "FAIL": 1, "INCOMPLETE": 0}
    r1 = [r for r in sm["rows"] if r["sn"] == f"{LOT}-001"][0]
    assert r1["stages"]["quick"] == "PASS" and r1["stages"]["rack"] == "PASS" and r1["result"] == "PASS"
    # passwords never in the record
    out = " ".join(r["output"] or "" for r in st.db.q("SELECT output FROM step"))
    assert "customer-pw" not in out and "line-password" not in out and "broker-pw" not in out


def test_identify_again_resends(line):
    st, bus, broker, rack = line
    st.start_all()
    wait_idle(st)
    unplug_all(st)
    run_rack(rack, again_once={f"{LOT}-002"})
    rows = st.db.q("SELECT s.detail FROM step s JOIN run r ON r.id=s.run_id "
                   "WHERE r.sn=? AND r.stage='rack' AND s.name LIKE 'ไฟและเสียง%'", (f"{LOT}-002",))
    assert rows[-1]["detail"] == "after 1 repeats"


def test_operator_says_no(line):
    st, bus, broker, rack = line
    st.start_all()
    wait_idle(st)
    unplug_all(st)
    run_rack(rack, answer=lambda q: q.sn != f"{LOT}-003")
    rb = rack.boxes[f"{LOT}-003"]
    assert rb.state == "fail" and rb.fail["detail"] == "operator: no"
    # not handed over: it stays on the line's Wi-Fi for the repair bench
    assert rack.fleet.boxes[f"{LOT}-003"].online is True


def test_firmware_without_mqtt_rpc_stops_at_the_dock(tmp_path, monkeypatch):
    """Not passed untested: the quick route needs the rack's commands, so a firmware
    without them is refused before the box leaves the dock."""
    monkeypatch.setenv("MFLASH_LIBRARY", str(tmp_path / "library"))
    broker = SimBroker(in_range={LINE})
    bus = demo_setup(str(tmp_path), speed=0, broker=broker)
    for b in bus.boxes:
        b.net.rpc = False
    st = Station("mcold", Db(str(tmp_path / "r.db")), make_site(tmp_path), hw=SimHardware(bus),
                 layout_path=str(tmp_path / "slots.json"))
    for i, s in enumerate(st.slots):
        s.location = f"1-1.{i + 1}"
    st.refresh()
    st.set_lot(LOT)
    st.set_stage("usb_quick")
    st.start(0)
    wait_idle(st)
    s = st.slots[0]
    assert s.state == "fail" and s.fail["step"].startswith("เปิดโหมดโรงงาน")
    assert "ทำใหม่ทั้งหมด" in s.fail["help"]


def test_rack_says_when_the_box_never_answers(line):
    st, bus, broker, rack = line
    st.start(0)
    wait_idle(st)
    bus.boxes[0].net.rpc = False         # e.g. an older firmware put back by hand
    unplug_all(st)
    run_rack(rack)
    rb = rack.boxes[f"{LOT}-001"]
    assert rb.state == "fail" and "ไม่เคยตอบคำสั่งผ่าน MQTT" in rb.fail["detail"]


def test_ip_taken_by_a_box_not_handed_over(line):
    st, bus, broker, rack = line
    st.db.seen("28:84:85:00:00:01", "mcold")
    st.db.set_value("28:84:85:00:00:01", "factory_ip", "10.20.0.101", 0)
    st.start(0)
    wait_idle(st)
    s = st.slots[0]
    assert s.state == "fail" and "10.20.0.101" in s.fail["detail"]


def test_auto_start_on_plug(line):
    st, bus, broker, rack = line
    st.set_auto_start(True)
    st.refresh()
    assert sum(1 for s in st.slots if s.busy or s.state == "pass") == 5
    wait_idle(st)
    st.refresh()                         # finished boxes still in the slot: not started again
    assert not any(s.busy for s in st.slots)
    st.clear(0)                          # a new box in slot 1
    st.refresh()
    assert st.slots[0].busy or st.slots[0].state == "pass"
    wait_idle(st)
    assert st.slots[0].box.sn == f"{LOT}-006"
    # remembered
    st2 = Station("mcold", st.db, st.site, hw=st.hw, layout_path=st.layout_path)
    assert st2.auto_start is True


def test_hundred_boxes_on_the_rack(tmp_path, monkeypatch):
    """A lot of 100 after the dock: every box at once, one question each in the queue."""
    monkeypatch.setenv("MFLASH_LIBRARY", str(tmp_path / "library"))
    broker = SimBroker(in_range={LINE})
    bus = demo_setup(str(tmp_path), speed=0, broker=broker)
    st = Station("mcold", Db(str(tmp_path / "r.db")), make_site(tmp_path), hw=SimHardware(bus),
                 layout_path=str(tmp_path / "slots.json"))
    st.set_lot(LOT)
    boxes = []
    for u in range(1, 101):
        mac = "28:84:85:AA:%02X:%02X" % (u // 256, u % 256)
        b = SimBox(mac=mac, slot=50 + u, version="0.7.0-demo", gnss_dead=u in (13, 77))
        b.state, b.sn, b.mqtt, b.factory_mode = "app", f"{LOT}-{u:03d}", True, True
        b.wifi, b.wifi_ip = [LINE], {LINE: f"10.20.0.{100 + u}"}
        SimNet(b, broker).check()
        boxes.append(b)
        st.db.seen(mac, "mcold")
        st.db.set_sn(mac, b.sn)
        rid = st.db.start_run("mcold", "quick", mac, b.sn, "t", "t")
        st.db.finish_run(rid, "PASS")
    rack = Rack(st, Fleet(broker.connect("station")), time_scale=0.05, debug_dir=str(tmp_path / "d"))
    run_rack(rack, timeout=60)
    states = [rack.boxes[f"{LOT}-{u:03d}"].state for u in range(1, 101)]
    assert states.count("pass") == 98 and states.count("fail") == 2
    assert all(b.wifi == ["Customer-WiFi"] for i, b in enumerate(boxes) if i + 1 not in (13, 77))
    assert st.summary()["counts"]["PASS"] == 98


def test_box_offline_midway(line):
    st, bus, broker, rack = line
    st.start(0)
    wait_idle(st)
    box = bus.boxes[0]
    unplug_all(st)
    rack.auto = False
    rack.refresh()
    deadline = time.monotonic() + 5
    while rack.boxes[f"{LOT}-001"].state != "ready":
        assert time.monotonic() < deadline
        rack.refresh()
        time.sleep(0.01)
    box.net.drop()                       # its charger pulled
    rack.start(f"{LOT}-001")
    run_rack(rack)
    rb = rack.boxes[f"{LOT}-001"]
    assert rb.state == "fail" and rb.fail["step"].startswith("กล่องต่อ broker")


def test_check_fields():
    d = {"ok": True, "temp": {"c": 23.5, "ok": True}}
    assert check_fields(d, {"ok": True, "temp.ok": True}, {"temp.c": [-40, 85]}) == []
    assert check_fields(d, {"gnss.ok": True}) == ['gnss.ok = null (ต้องเป็น true)']


def test_settings_page_line_wifi_and_station_login(line):
    from mflash import site as sitelib
    st = line[0]
    got = st.settings()
    assert got["factory"]["ssid"] == LINE and got["factory"]["first"] == "10.20.0.101"
    assert got["factory"]["capacity"] == 154 and "line-password" not in json.dumps(got)
    form = {"station": "line-2", "wifi": [{"ssid": "Customer-WiFi", "password": None, "dhcp": True}],
            "mqtt": {"host": "broker.example", "port": 1883, "username": "u", "password": None},
            "factory": {"ssid": LINE, "password": None, "ip_base": "10.20.0.50", "gateway": "10.20.0.1",
                        "subnet": "255.255.255.0", "dns": ""},
            "station_mqtt": {"username": "station", "password": "station-pw"}}
    back = sitelib.load(st.save_settings(form))
    assert back.factory.password == "line-password" and back.factory.ip_base == "10.20.0.50"
    assert back.station_mqtt == {"username": "station", "password": "station-pw"}
    assert back.rack_broker()["username"] == "station" and back.rack_broker()["host"] == "broker.example"
    assert "station-pw" in back.secrets() and "line-password" in back.secrets()
    with pytest.raises(RuntimeError, match="gateway"):
        st.save_settings({**form, "factory": {**form["factory"], "ip_base": "10.20.0.0"}})
    form["station_mqtt"] = {"username": "", "password": None}    # back to the boxes' login
    assert sitelib.load(st.save_settings(form)).station_mqtt == {}


def test_rack_http_api(line):
    import threading
    import urllib.error
    import urllib.request
    from http.server import ThreadingHTTPServer
    from mflash.gui import make_handler
    st, bus, broker, rack = line
    rack.auto = False
    srv = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(st, rack))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_port}"

    def post(path, body):
        req = urllib.request.Request(base + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
        try:
            return json.load(urllib.request.urlopen(req))
        except urllib.error.HTTPError as e:
            return {"status": e.code, **json.load(e)}

    try:
        assert post("/api/auto_start", {"on": True}) == {"ok": True} and st.auto_start
        post("/api/auto_start", {"on": False})
        st.start(0)
        wait_idle(st)
        unplug_all(st)
        end = time.monotonic() + 5
        while True:
            rack.refresh()
            r = json.load(urllib.request.urlopen(base + "/api/rack"))
            if r["boxes"] and r["boxes"][0]["state"] == "ready":
                break
            assert time.monotonic() < end, r
            time.sleep(0.02)
        assert r["enabled"] and r["connected"] and r["boxes"][0]["ip"] == "10.20.0.101"
        assert post("/api/rack/start", {"sn": "nope"})["status"] == 409
        assert post("/api/rack/start", {"sn": f"{LOT}-001"}) == {"ok": True}
        end = time.monotonic() + 10
        while not rack.questions:
            assert time.monotonic() < end
            time.sleep(0.02)
        r = json.load(urllib.request.urlopen(base + f"/api/rack?log={LOT}-001"))
        q = r["questions"][0]
        assert q["unit"] == 1 and "001" in q["text"] and r["boxes"][0]["log"]
        assert post("/api/rack/answer", {"qid": q["id"], "value": True}) == {"ok": True}
        assert post("/api/rack/answer", {"qid": q["id"], "value": True})["status"] == 409
        rack.boxes[f"{LOT}-001"].thread.join(10)
        assert rack.boxes[f"{LOT}-001"].state == "pass"
    finally:
        srv.shutdown()
    # no broker set up: the page says why
    srv = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(st, None, "ยังไม่ได้ตั้ง MQTT broker"))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        r = json.load(urllib.request.urlopen(f"http://127.0.0.1:{srv.server_port}/api/rack"))
        assert r == {"enabled": False, "error": "ยังไม่ได้ตั้ง MQTT broker"}
    finally:
        srv.shutdown()


def test_factory_documents_leave_the_server_rev_alone(line):
    """The customer's server starts its revs at 1 after the hand-over and is heard."""
    st, bus, broker, rack = line
    st.start(0)
    wait_idle(st)
    box = bus.boxes[0]
    unplug_all(st)
    run_rack(rack)
    assert box.server_rev == 0 and box.factory_rev >= 2
    box.wifi.append(LINE)                 # back on the line's Wi-Fi to hear the server
    box.net.check()
    deadline = time.monotonic() + 5
    while not (box.net.link and box.net.link.connected):
        assert time.monotonic() < deadline
        time.sleep(0.01)
    server = broker.connect("server")
    server.publish(f"mcold/v1/{box.sn}/config", json.dumps({"rev": 1, "config": {"sample_period_s": 600}}),
                   retain=True)
    deadline = time.monotonic() + 5
    while box.config.get("sample_period_s") != 600:
        assert time.monotonic() < deadline
        time.sleep(0.01)
    assert box.server_rev == 1
