"""The six-slot station and its HTTP API, on simulated boxes."""
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mflash.db import Db  # noqa: E402
from mflash.gui import make_handler  # noqa: E402
from mflash.sim import SimHardware, demo_setup  # noqa: E402
from mflash.site import Site  # noqa: E402
from mflash.station import Station  # noqa: E402


@pytest.fixture
def station(tmp_path, monkeypatch):
    monkeypatch.setenv("MFLASH_LIBRARY", str(tmp_path / "library"))
    bus = demo_setup(str(tmp_path), speed=0)
    st = Station("mcold", Db(str(tmp_path / "r.db")), Site(), hw=SimHardware(bus),
                 layout_path=str(tmp_path / "slots.json"))
    for i, s in enumerate(st.slots):
        s.location = f"1-1.{i + 1}"
    st.on_clear = bus.replace
    st.bus = bus
    st.refresh()
    st.set_lot(LOT)
    return st


LOT = "mCDV1-L0169-1069"


def answer_until_idle(st, no_for=(), timeout=30):
    """Play the person: yes to every check, an SN for every box."""
    sn = iter(range(1, 100))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for s in st.slots:
            q = s.question
            if q:
                if q.kind == "text":
                    val = f"mCDV1-L0169-1069-{next(sn):03d}"
                else:
                    val = not any(x in q.text for x in no_for)
                try:
                    st.answer(s.index, q.id, val)
                except RuntimeError:
                    pass
        if not any(s.busy for s in st.slots):
            return
        time.sleep(0.01)
    raise AssertionError("still running")


def test_slots_follow_the_bus(station):
    states = [s.state for s in station.slots]
    assert states == ["ready", "ready", "ready", "empty", "ready", "ready"]
    assert station.slots[0].port.device == "COM3"


def test_all_slots_run_at_once(station):
    assert station.start_all("tester") == 5
    answer_until_idle(station)
    states = [s.state for s in station.slots]
    assert states == ["pass", "pass", "pass", "empty", "pass", "fail"]
    assert station.counts == {"pass": 4, "fail": 1}
    assert station.slots[5].fail["step"].startswith("GNSS")
    assert "GPIO18" in station.slots[5].fail["help"]
    # the lot's prefix and each slot's own unit number, never asked for
    assert [s.box.sn for s in (station.slots[0], station.slots[1], station.slots[2], station.slots[4])] == \
        [f"{LOT}-001", f"{LOT}-002", f"{LOT}-003", f"{LOT}-004"]
    st = station.state()
    assert st["slots"][0]["stages"][-1] == {"name": "verify", "label": "ตรวจก่อนส่ง", "result": "PASS"}
    assert st["slots"][0]["firmware"] == "0.7.0-demo"          # what it runs now


def test_a_no_fails_the_box(station):
    station.start(0)
    answer_until_idle(station, no_for=["ได้ยินเสียง beep"])
    s = station.slots[0]
    assert s.state == "fail" and s.fail["step"] == "Buzzer ดัง"
    assert s.results == {"test": "FAIL", "production": "", "setup": "", "verify": ""}


def test_stop_while_waiting_for_an_answer(station):
    station.start(1)
    end = time.monotonic() + 5
    while not station.slots[1].question and time.monotonic() < end:
        time.sleep(0.01)
    assert station.slots[1].state == "ask"
    station.stop(1)
    station.slots[1].thread.join(5)
    assert station.slots[1].state == "stopped"
    run = station.db.history()[0]
    assert run["result"] == "FAIL"
    assert station.db.steps(run["id"])[-1]["detail"] == "stopped by the operator"
    with pytest.raises(RuntimeError, match="ตอบไปแล้ว"):
        station.answer(1, 999, True)


def test_a_new_box_in_a_finished_slot_starts_fresh(station):
    station.start(0)
    answer_until_idle(station)
    assert station.slots[0].state == "pass"
    box = station.bus.boxes[0]
    station.bus.boxes.remove(box)                         # taken out
    station.refresh()
    assert station.slots[0].state == "pass" and station.slots[0].left
    station.bus.boxes.insert(0, type(box)(mac="28:84:85:00:00:01", slot=0))   # a new one in
    station.refresh()
    assert station.slots[0].state == "ready" and station.slots[0].box is None


def test_clear_puts_a_blank_demo_board_in(station):
    station.start(4)
    answer_until_idle(station)
    old = station.bus.boxes[3].mac
    station.clear(4)
    assert station.slots[4].state == "ready"
    assert station.bus.boxes[3].mac != old and station.bus.boxes[3].state == "rom"


def test_layout_is_saved_and_loaded(station, tmp_path):
    path = station.save_layout()
    st2 = Station("mcold", station.db, Site(), hw=station.hw, layout_path=path)
    assert [s.location for s in st2.slots] == [f"1-1.{i}" for i in range(1, 7)]


def test_http_api(station):
    srv = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(station))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_port}"

    def post(path, body):
        req = urllib.request.Request(base + path, json.dumps(body).encode(),
                                     {"Content-Type": "application/json"})
        try:
            return json.load(urllib.request.urlopen(req))
        except urllib.error.HTTPError as e:
            return {"status": e.code, **json.load(e)}

    try:
        page = urllib.request.urlopen(base + "/").read().decode()
        assert "mCOLD Station" in page
        st = json.load(urllib.request.urlopen(base + "/api/state?log=0"))
        assert st["product"] == "mcold" and len(st["slots"]) == 6
        assert all(f["ok"] for f in st["firmware"])
        assert st["slots"][0]["log"] == [] and st["slots"][1]["log"] is None
        assert post("/api/start", {"slot": 3})["status"] == 409          # empty slot
        assert post("/api/answer", {"slot": 0, "qid": 5, "value": True})["status"] == 409
        assert post("/api/config", {"stage": "nope"})["status"] == 409
        assert post("/api/config", {"stage": "verify"}) == {"ok": True}
        assert station.stages() == ["verify"]
        assert post("/api/start", {"slot": 0, "operator": "x"}) == {"ok": True}
        assert post("/api/config", {"product": "mcold"})["status"] == 409  # while running
        station.stop(0)
        station.slots[0].thread.join(5)
        assert post("/api/lot", {"lot": "bad"})["status"] == 409
        assert post("/api/unit", {"slot": 1, "unit": 77}) == {"ok": True}
        assert post("/api/unit", {"slot": 2, "unit": 77})["status"] == 409
        assert post("/api/unit", {"slot": 2, "step": 1}) == {"ok": True}
        sm = json.load(urllib.request.urlopen(base + "/api/summary?lot=" + LOT))
        assert sm["lot"] == LOT and "rows" in sm
        resp = urllib.request.urlopen(base + "/api/summary.csv?lot=" + LOT)
        assert resp.headers["Content-Disposition"] == f'attachment; filename="SN_{LOT}.csv"'
        assert resp.read().decode("utf-8-sig").startswith("sn,unit,result")
        assert urllib.request.urlopen(base + "/fonts/IBMPlexSansThai-Regular.ttf").status == 200
        with pytest.raises(urllib.error.HTTPError):
            urllib.request.urlopen(base + "/fonts/../../site.ini")
    finally:
        srv.shutdown()


def test_lot_and_unit_numbers(station):
    with pytest.raises(RuntimeError, match="ไม่ตรงรูปแบบ"):
        station.set_lot("mCDV1-L0169-1369")             # month 13
    assert [s.unit for s in station.slots] == [1, 2, 3, None, 4, 5]
    st = station.state()
    assert st["lot"] == LOT and st["slots"][0]["sn"] == f"{LOT}-001" and st["slots"][0]["sn_planned"]
    assert st["slots"][0]["mac"]                         # from the chip's USB serial, before any run
    with pytest.raises(RuntimeError, match="ใช้แล้ว: ช่อง 2"):
        station.set_unit(0, 2)
    station.set_unit(0, 42)
    assert station.slots[0].unit == 42 and not station.slots[0].unit_auto
    station.step_unit(1, 1)                              # 3 is slot 3's, 4 slot 5's, 5 slot 6's
    assert station.slots[1].unit == 6
    station.step_unit(1, -1)
    assert station.slots[1].unit == 2


def test_a_used_number_is_refused_and_the_next_box_gets_the_next_free_one(station):
    station.start(0)
    answer_until_idle(station)
    assert station.slots[0].box.sn == f"{LOT}-001"
    station.clear(0)                                     # a new blank box in slot 1
    station.refresh()
    assert station.slots[0].unit == 6                    # 1 is in the record, 2-5 in other slots
    with pytest.raises(RuntimeError, match="ใช้แล้ว"):
        station.set_unit(0, 1)


def test_sn_summary(station):
    assert station.start_all("somchai") == 5
    answer_until_idle(station)
    sm = station.summary()
    assert sm["lot"] == LOT and sm["counts"] == {"PASS": 4, "FAIL": 1, "INCOMPLETE": 0}
    with_sn = [r for r in sm["rows"] if r["sn"]]
    assert [r["unit"] for r in with_sn] == [1, 2, 3, 4]
    r = with_sn[0]
    assert r["result"] == "PASS" and r["firmware"] == "0.7.0-demo" and r["operator"] == "somchai"
    assert r["stages"] == {"test": "PASS", "production": "PASS", "setup": "PASS", "verify": "PASS"}
    bad = [r for r in sm["rows"] if r["result"] == "FAIL"][0]
    assert bad["sn"] == "" and bad["fail"]["stage"] == "test" and bad["fail"]["step"].startswith("GNSS")
    assert sm["missing"] == []
    csv_text = station.summary_csv()
    assert csv_text.splitlines()[0].startswith("sn,unit,result,mac,firmware,test,production")
    assert f"{LOT}-002,2,PASS" in csv_text
    # another lot: nothing of this one
    assert [r for r in station.summary("mCDV1-L0270-1170")["rows"] if r["sn"]] == []


def test_cli_summary(station, capsys, tmp_path):
    from mflash.cli import main
    station.start(0, "somchai")
    answer_until_idle(station)
    main(["--db", station.db.path, "summary", "mcold", LOT])
    out = capsys.readouterr().out
    assert f"lot {LOT}: 1 SNs, 1 passed" in out and f"{LOT}-001" in out and "somchai" in out
    main(["--db", station.db.path, "summary", "mcold", LOT, "--csv", str(tmp_path / "s.csv")])
    assert (tmp_path / "s.csv").read_text(encoding="utf-8-sig").count(f"{LOT}-001") == 1


# ---- a box made already: new settings, new firmware ------------------------

def _make_one(station, slot=0):
    station.start(slot, "t")
    answer_until_idle(station)
    assert station.slots[slot].state == "pass"
    return station.bus.boxes[0]


def test_reflash_keeps_the_sn_and_rewrites_only_the_app(station):
    from mflash.site import Site, Wifi
    box = _make_one(station)
    sn = box.sn
    station.site = Site(wifi=[Wifi("Old-AP", "password1")], path=station.site.path)
    station.start(0, "t", mode="resetup")
    answer_until_idle(station)
    assert box.wifi == ["Old-AP"]
    # the settings change: Old-AP goes, New-AP comes; the firmware is written again
    station.site = Site(wifi=[Wifi("New-AP", "password2")], path=station.site.path)
    esp = station.hw.esp("", {})
    n = len(esp.writes)
    station.start(0, "t", mode="reflash_setup")
    answer_until_idle(station)
    s = station.slots[0]
    assert s.state == "pass", s.fail
    assert s.stages == ["update", "setup", "verify"]
    assert esp.writes[n:] == [(["ota_data_initial.bin", "firmware.bin"], False)]   # no erase
    assert box.sn == sn and s.box.sn == sn                      # its SN, not a new number
    assert box.wifi == ["New-AP"]
    assert "wifi del Old-AP" in box.sent


def test_resetup_flashes_nothing(station):
    _make_one(station)
    esp = station.hw.esp("", {})
    n = len(esp.writes)
    station.start(0, "t", mode="resetup")
    answer_until_idle(station)
    assert station.slots[0].state == "pass" and len(esp.writes) == n


def test_reflash_needs_a_box_that_was_made(station):
    station.start(1, "t", mode="reflash")              # a blank box: nothing to keep
    answer_until_idle(station)
    s = station.slots[1]
    assert s.state == "fail"


def test_firmware_dropdown(station, tmp_path):
    from mflash import firmware as fwlib
    from mflash import profile as proflib
    from mflash.sim import build_dir
    p = proflib.load("mcold")
    m = fwlib.import_build(p, "production", build_dir(str(tmp_path / "b2"), "0.7.1-demo", "production",
                                                      proflib.REPO))
    opts = station.firmware_options()["production"]
    assert [o["version"] for o in opts] == ["0.7.0-demo", "0.7.0-dev.61"]   # 0.7.1: not approved
    with pytest.raises(RuntimeError, match="ยังไม่ได้อนุมัติ"):
        station.set_firmware("production", m["id"])
    fwlib.approve("mcold", "production", m["id"], "boss")
    station._fwo_cache = (0.0, None)
    assert [o["version"] for o in station.firmware_options()["production"]] == \
        ["0.7.1-demo", "0.7.0-demo", "0.7.0-dev.61"]
    old = [o for o in station.firmware_options()["production"] if o["version"] == "0.7.0-demo"][0]
    station.set_firmware("production", old["id"])          # the older one, on purpose
    assert [f["version"] for f in station.firmware_status() if f["kind"] == "production"] == ["0.7.0-demo"]
    box = _make_one(station)
    assert box.version == "0.7.0-demo"
    station.set_firmware("production", "approved")         # back to the newest approved
    station.start(0, "t", mode="reflash")
    answer_until_idle(station)
    assert station.slots[0].state == "pass" and box.version == "0.7.1-demo"
    # the choice outlives a restart
    station.set_firmware("production", old["id"])
    st2 = Station("mcold", station.db, Site(), hw=station.hw, layout_path=station.layout_path)
    assert st2.fw_choice == {"production": old["id"]}


def test_settings_page(station, tmp_path):
    from mflash import site as sitelib
    from mflash.site import Wifi
    station.site = sitelib.Site(wifi=[Wifi("Line-AP", "secret-123")],
                                mqtt={"host": "b.example", "port": 1883, "username": "u",
                                      "password": "mqtt-secret"},
                                path=str(tmp_path / "site.ini"))
    got = station.settings()
    assert "secret-123" not in json.dumps(got) and "mqtt-secret" not in json.dumps(got)
    assert got["wifi"][0]["has_password"] and got["mqtt"]["has_password"]
    assert got["config_keys"]["sample_period_s"]["max"] == 3600
    form = {"station": "line-2",
            "wifi": [{"ssid": "Line-AP-5G", "old_ssid": "Line-AP", "password": None, "dhcp": True},
                     {"ssid": "Office", "password": "office-pass", "dhcp": False, "ip": "10.0.0.50",
                      "gateway": "10.0.0.1", "subnet": "255.255.255.0", "dns": ""}],
            "mqtt": {"host": "b.example", "port": 8883, "username": "u", "password": None},
            "ota_base": "https://files.example/mcold/", "config": {"sample_period_s": "600", "tz_offset_min": ""}}
    path = station.save_settings(form)
    back = sitelib.load(path)
    assert back.station == "line-2"
    assert [(w.ssid, w.password, w.dhcp) for w in back.wifi] == [("Line-AP-5G", "secret-123", True),
                                                                 ("Office", "office-pass", False)]
    assert back.wifi[1].ip == "10.0.0.50" and back.mqtt["password"] == "mqtt-secret"
    assert back.mqtt["port"] == 8883 and back.config == {"sample_period_s": 600}
    assert station.site.station == "line-2"                     # used from the next run on
    for bad, msg in [({"wifi": [{"ssid": "x", "password": "short"}]}, "8–63"),
                     ({"config": {"sample_period_s": "5"}}, "นอกช่วง"),
                     ({"mqtt": {"host": "b", "username": "a b"}}, "ช่องว่าง"),
                     ({"ota_base": "ftp://x"}, "OTA"),
                     ({"wifi": [{"ssid": "o", "dhcp": False, "ip": "10.0.0.999"}]}, "IPv4")]:
        with pytest.raises(RuntimeError, match=msg):
            station.save_settings(bad)
    assert sitelib.load(path).station == "line-2"               # a refused form changes nothing
