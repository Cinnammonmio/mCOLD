import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fakes  # noqa: E402
from mflash import firmware as fwlib  # noqa: E402
from mflash import imageinfo, parsers  # noqa: E402
from mflash import profile as proflib  # noqa: E402
from mflash.db import Db  # noqa: E402
from mflash.runner import Box, Runner  # noqa: E402
from mflash.site import Site, Wifi  # noqa: E402
from mflash.ui import ScriptedUi  # noqa: E402

REPO = proflib.REPO
SN = "mCDV1-L0169-1069-001"


@pytest.fixture
def lib(tmp_path, monkeypatch):
    monkeypatch.setenv("MFLASH_LIBRARY", str(tmp_path / "library"))
    return tmp_path


def _import(tmp_path, kind, version, approve=True):
    p = proflib.load("mcold")
    d = fakes.build_dir(str(tmp_path / f"build-{kind}-{version}"), version, kind, REPO)
    m = fwlib.import_build(p, kind, d)
    if approve:
        m = fwlib.approve("mcold", kind, m["id"], "tester")
    return m


# ---- images and the library --------------------------------------------

def test_app_descriptor_roundtrip():
    data, sha = fakes.app_image("0.7.0-dev.61")
    info = imageinfo.read_app_info(data)
    assert info.version == "0.7.0-dev.61"
    assert info.project == "mCOLD"
    assert info.idf == "v5.3.2"
    assert info.elf_sha256 == sha
    assert imageinfo.read_app_info(b"\xff" * 4096) is None


def test_partition_table_matches_the_repo_csv(tmp_path):
    path = tmp_path / "partitions.bin"
    path.write_bytes(fakes.partition_table(os.path.join(REPO, "firmware", "partitions.csv")))
    parts = imageinfo.read_partition_table_file(str(path))
    names = [p.name for p in parts]
    assert names == ["nvs", "otadata", "phy_init", "nvs_keys", "ota_0", "ota_1", "log_meta",
                     "trip_log", "coredump"]
    trip = parts[names.index("trip_log")]
    assert (trip.offset, trip.size) == (0x630000, 0x9C0000)
    assert fwlib.check_partitions(str(path), os.path.join(REPO, "firmware", "partitions.csv")) == []


def test_import_refuses_a_different_partition_layout(lib, tmp_path):
    p = proflib.load("mcold")
    d = fakes.build_dir(str(tmp_path / "b"), "0.7.0", "production", REPO)
    other = tmp_path / "other.csv"
    other.write_text("nvs, data, nvs, 0x9000, 0x6000,\nota_0, app, ota_0, 0x20000, 0x300000,\n")
    with open(os.path.join(d, "partitions.bin"), "wb") as f:
        f.write(fakes.partition_table(str(other)))
    with pytest.raises(fwlib.FirmwareError, match="partition table does not match"):
        fwlib.import_build(p, "production", d)


def test_library_import_approve_resolve_and_tamper(lib, tmp_path):
    m = _import(tmp_path, "production", "0.7.0", approve=False)
    assert m["version"] == "0.7.0" and m["id"].startswith("0.7.0+")
    assert [i["offset"] for i in m["images"]] == [0x0, 0x8000, 0xD000, 0x20000]
    with pytest.raises(fwlib.FirmwareError, match="no approved"):
        fwlib.resolve("mcold", "production", "approved")
    fwlib.approve("mcold", "production", "0.7.0", "somchai")
    assert fwlib.resolve("mcold", "production")["approved"]["by"] == "somchai"
    files = fwlib.verify_files(m)
    assert len(files) == 4
    with open(files[-1][1], "r+b") as f:
        f.seek(100)
        f.write(b"X")
    with pytest.raises(fwlib.FirmwareError, match="changed since"):
        fwlib.verify_files(m)


def test_import_refuses_firmware_older_than_the_profile_needs(lib, tmp_path):
    with pytest.raises(fwlib.FirmwareError, match="older than 0.7.0-dev.5"):
        _import(tmp_path, "production", "0.6.0")
    assert _import(tmp_path, "production", "0.7.0-dev.61")["version"] == "0.7.0-dev.61"


def test_build_from_a_git_tag(lib, tmp_path, monkeypatch):
    """The checkout, the build and the import, with pio replaced by a script."""
    fake_pio = tmp_path / "pio.py"
    here = os.path.dirname(os.path.abspath(__file__))
    fake_pio.write_text(
        "import os, sys\n"
        f"sys.path[:0] = [{here!r}, {os.path.dirname(here)!r}]\n"
        "import fakes\n"
        "assert sys.argv[1:] == ['run', '-e', 'mcold'], sys.argv\n"
        "assert os.path.isfile('partitions.csv'), os.getcwd()\n"
        f"fakes.build_dir('.pio/build/mcold', '0.7.0-dev.61', 'production', {REPO!r})\n")
    monkeypatch.setenv("MFLASH_PIO", f"{sys.executable} {fake_pio}")
    monkeypatch.setenv("MFLASH_BUILD_DIR", str(tmp_path / "builds"))
    m = fwlib.build_from_git(proflib.load("mcold"), "production", "HEAD", log=lambda s: None)
    assert m["source"]["git_ref"] == "HEAD" and len(m["source"]["git_commit"]) == 40
    assert m["version"] == "0.7.0-dev.61"
    assert os.listdir(tmp_path / "builds") == []          # the checkout is cleaned up


# ---- parsers ---------------------------------------------------------

def test_health_parser():
    out = fakes.HEALTH.format(temp="-- (open; MAX6675)")
    states = parsers.health_states(out)
    assert states["accel"] == "ok" and states["pd"] == "unknown" and len(states) == 13
    assert parsers.health_temperature(out) is None
    assert parsers.health_temperature(fakes.HEALTH.format(temp="4.25 C  (MAX6675)")) == 4.25
    assert parsers.health_clock(out) == ("2026-10-24 10:00:00", "ntp", True)


def test_nmea_needs_good_checksums_not_bytes():
    good, bad, _ = parsers.nmea_sentences(fakes.GNSS)
    assert (good, bad) == (3, 0)
    garbage = "$GNRMC,,V,,,,,,,,,,N*00\r\n" + "\x13\x8a" * 200
    assert parsers.nmea_sentences(garbage)[:2] == (0, 1)


def test_profiles_load():
    p = proflib.load("mcold")
    assert p.sequence == ["test", "production", "setup", "verify"]
    assert p.sn_ok(SN) and not p.sn_ok("mCDV1-L0169-1369-001") and not p.sn_ok("box-1")
    e = proflib.load("etemp")
    assert e.incomplete


def test_profile_rejects_bad_steps(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text("product: x\nchip: esp32\nflash: {}\nusb: {vid: 1}\n"
                   "stages:\n  s:\n    - {name: a, type: console}\n"
                   "    - {name: b, type: flash, firmware: nope}\n"
                   "    - {name: c, type: console, send: x, expect: ['(']}\n")
    with pytest.raises(proflib.ProfileError) as e:
        proflib.load(str(bad))
    msg = str(e.value)
    assert "needs 'send'" in msg and "firmware 'nope'" in msg and "bad expect regex" in msg


# ---- the whole line on a simulated box ---------------------------------

def _runner(tmp_path, box, answers, site=None, sn=""):
    p = proflib.load("mcold")
    db = Db(str(tmp_path / "rec.db"))
    ui = ScriptedUi(answers)
    r = Runner(p, db, ui, site or Site(), hw=fakes.FakeHardware(box), operator="t", sn=sn)
    return r, db, ui


def test_full_sequence_on_a_blank_board(lib, tmp_path):
    _import(tmp_path, "test", "1.0-bringup")
    prod = _import(tmp_path, "production", "0.7.0")
    box = fakes.FakeBox(elf16=prod["elf_sha256"][:16])
    site = Site(wifi=[Wifi("Factory AP", "secret-pass")],
                mqtt={"host": "siamatic.co.th", "port": 1883, "username": "u", "password": "pw1"},
                ota_base="https://files.example/mcold/", config={"tz_offset_min": 420})
    r, db, ui = _runner(tmp_path, box, [True] * 30, site=site, sn=SN)
    b = Box(label="1", port=box.port())
    assert r.run(b, ["test", "production", "setup", "verify"]), "\n".join(ui.log)
    assert b.mac == fakes.MAC and b.sn == SN and box.sn == SN
    assert box.state == "app" and 0x10000 not in box.flash      # bringup erased away
    writes = r.esp.writes
    assert writes[0] == (["bootloader.bin", "partitions.bin", "firmware.bin"], True)
    assert writes[1][1] is True and "ota_data_initial.bin" in writes[1][0]
    assert 'wifi add "Factory AP" secret-pass' in box.sent
    assert "mqtt set siamatic.co.th 1883 u pw1" in box.sent
    assert box.config == {"tz_offset_min": 420}
    runs = db.history()
    assert [x["result"] for x in runs] == ["PASS"] * 4
    assert db.device(fakes.MAC)["sn"] == SN
    # passwords never reach the record
    for run in runs:
        for s in db.steps(run["id"]):
            assert "secret-pass" not in (s["output"] or "") and "pw1" not in (s["output"] or "")
    out = tmp_path / "export.csv"
    assert db.export_csv(str(out)) == 1
    assert SN in out.read_text(encoding="utf-8-sig")


def test_open_probe_that_reads_a_temperature_fails_the_board(lib, tmp_path):
    _import(tmp_path, "test", "1.0-bringup")
    box = fakes.FakeBox(probe_reads_open=False)
    r, db, ui = _runner(tmp_path, box, [True] * 30)
    assert not r.run(Box(label="1", port=box.port()), ["test"])
    steps = db.steps(db.history()[0]["id"])
    assert steps[-1]["result"] == "FAIL" and "OPEN" in steps[-1]["name"]
    assert any("T−" in x for x in ui.log)


def test_unapproved_firmware_is_refused(lib, tmp_path):
    m = _import(tmp_path, "production", "0.7.0", approve=False)
    box = fakes.FakeBox(elf16=m["elf_sha256"][:16])
    r, db, ui = _runner(tmp_path, box, [])
    assert not r.run(Box(label="1", port=box.port()), ["production"])
    assert "no approved" in db.steps(db.history()[0]["id"])[0]["detail"]
    # named on purpose, still refused without --allow-unapproved
    r.choice = {"production": "0.7.0"}
    assert not r.run(Box(label="1", port=box.port()), ["production"])
    assert "is not approved" in db.steps(db.history()[0]["id"])[0]["detail"]
    assert box.flash == {}
    # an engineer can, and the record says so
    r.allow_unapproved = True
    assert r.run(Box(label="1", port=box.port()), ["production"])
    assert db.history()[0]["unapproved"] == 1


def test_sn_already_used_by_another_box(lib, tmp_path):
    prod = _import(tmp_path, "production", "0.7.0")
    box = fakes.FakeBox(elf16=prod["elf_sha256"][:16])
    box.flash = {0x20000: "firmware.bin"}
    box.state = "app"
    r, db, ui = _runner(tmp_path, box, [], sn=SN)
    db.seen("AA:BB:CC:DD:EE:FF", "mcold")
    db.set_sn("AA:BB:CC:DD:EE:FF", SN)
    assert not r.run(Box(label="1", port=box.port()), ["setup"])
    assert "already belongs" in db.steps(db.history()[0]["id"])[0]["detail"]


def test_reflash_from_the_product_firmware_goes_through_its_flash_command(lib, tmp_path):
    prod = _import(tmp_path, "production", "0.7.0")
    box = fakes.FakeBox(elf16=prod["elf_sha256"][:16])
    box.flash = {0x20000: "firmware.bin"}
    box.state, box.sn = "app", SN
    r, db, ui = _runner(tmp_path, box, [])
    b = Box(label="1", port=box.port())
    assert r.run(b, ["production"]), "\n".join(ui.log)
    assert "flash" in box.sent and b.mac == fakes.MAC
