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
    return st


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
    assert station.slots[5].fail["step"].startswith("ถอดโพรบแล้ว")
    assert "T−" in station.slots[5].fail["help"]
    sns = {s.box.sn for s in station.slots[:3]}
    assert len(sns) == 3                                  # each its own SN
    st = station.state()
    assert st["slots"][0]["stages"][-1] == {"name": "verify", "label": "ตรวจก่อนส่ง", "result": "PASS"}
    assert st["slots"][0]["firmware"].endswith("production 0.7.0-demo")


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
    assert station.db.steps(run["id"])[0]["detail"] == "stopped by the operator"
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
        assert urllib.request.urlopen(base + "/fonts/IBMPlexSansThai-Regular.ttf").status == 200
        with pytest.raises(urllib.error.HTTPError):
            urllib.request.urlopen(base + "/fonts/../../site.ini")
    finally:
        srv.shutdown()
