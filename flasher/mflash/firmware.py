"""The firmware library: the only place the station takes images from.

An image enters the library one way: built from this repository (a tag,
a branch, a commit) or imported from a PlatformIO build folder. Either
way it is copied in with a manifest -- version, the ELF's SHA-256, the
SHA-256 of every file, the git commit it came from -- and from then on
the files are checked against that manifest before every flash.

The operator's station flashes only images an engineer has approved
(`mflash fw approve`); anything else needs --allow-unapproved, which is
recorded with the run.
"""
from __future__ import annotations

import csv
import datetime as dt
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile

from . import imageinfo
from .profile import FLASHER, REPO, FirmwareKind, Profile


class FirmwareError(Exception):
    pass


def library_root() -> str:
    return os.environ.get("MFLASH_LIBRARY") or os.path.join(FLASHER, "library")


def _now() -> str:
    return dt.datetime.now().astimezone().isoformat(timespec="seconds")


def _git(*args: str, cwd: str = REPO) -> str:
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        raise FirmwareError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def _size(s: str) -> int:
    s = s.strip().upper()
    if not s:
        return 0
    mult = 1
    if s.endswith("K"):
        mult, s = 1024, s[:-1]
    elif s.endswith("M"):
        mult, s = 1024 * 1024, s[:-1]
    return int(s, 0) * mult


def read_partitions_csv(path: str) -> list[tuple[str, int, int]]:
    """(name, offset, size) of each row; offsets must be written out, as ours are."""
    rows = []
    with open(path, encoding="utf-8") as f:
        lines = [ln for ln in f if ln.strip() and not ln.lstrip().startswith("#")]
    for r in csv.reader(lines):
        r = [c.strip() for c in r]
        if len(r) < 5 or not r[3]:
            raise FirmwareError(f"{path}: row {r} has no explicit offset")
        rows.append((r[0], _size(r[3]), _size(r[4])))
    return rows


def check_partitions(bin_path: str, csv_path: str) -> list[str]:
    """Differences between a built partition table and the layout it should have."""
    parts = imageinfo.read_partition_table_file(bin_path)
    if not parts:
        return [f"{os.path.basename(bin_path)} is not a partition table"]
    want = read_partitions_csv(csv_path)
    got = [(p.name, p.offset, p.size) for p in parts]
    if got == want:
        return []
    diff = [f"expected {n} @0x{o:X} size 0x{s:X}" for n, o, s in want if (n, o, s) not in got]
    diff += [f"image has {n} @0x{o:X} size 0x{s:X}" for n, o, s in got if (n, o, s) not in want]
    return diff or ["same partitions in a different order"]


def is_signed_v2(path: str) -> bool:
    """A Secure Boot V2 signature block (magic 0xE7, version 2) in the last 4 KiB sector."""
    size = os.path.getsize(path)
    if size < 4096 or size % 4096:
        return False
    with open(path, "rb") as f:
        f.seek(size - 4096)
        head = f.read(2)
    return head == b"\xe7\x02"


def version_key(v: str) -> tuple:
    """0.7.0-dev.5 < 0.7.0-dev.61 < 0.7.0 < 0.7.1; anything unparsed sorts first."""
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)(?:-dev(?:\.(\d+))?)?", v)
    if not m:
        return (-1,)
    a, b, c, dev = m.groups()
    is_dev = "-dev" in v
    return (int(a), int(b), int(c), 0 if is_dev else 1, int(dev or 0))


def import_build(profile: Profile, kind: str, build_dir: str, source: dict | None = None) -> dict:
    """Copy one build's images into the library; returns its manifest."""
    fk: FirmwareKind = profile.firmware[kind]
    files = []
    app = None
    for img in fk.images:
        path = os.path.join(build_dir, img["file"])
        if not os.path.isfile(path):
            raise FirmwareError(f"missing {path} (build {fk.project} first: pio run -e {fk.env})")
        files.append((img, path))
        info = imageinfo.read_app_info_file(path)
        if info and app is None:
            app = (img, path, info)
    if not app:
        raise FirmwareError(f"no app image among {[i['file'] for i in fk.images]}")
    problems = []
    if fk.partitions_csv:
        table = [p for i, p in files if os.path.basename(p) == "partitions.bin"]
        if table:
            problems = check_partitions(table[0], os.path.join(REPO, fk.partitions_csv))
    if problems:
        raise FirmwareError("partition table does not match " + fk.partitions_csv + ":\n  "
                            + "\n  ".join(problems))
    img, app_path, info = app
    if fk.min_version and version_key(info.version) < version_key(fk.min_version):
        raise FirmwareError(f"{kind} {info.version} is older than {fk.min_version}, the first "
                            "with every console command this profile uses")
    app_sha = imageinfo.sha256_file(app_path)
    fw_id = f"{info.version}+{app_sha[:8]}"
    dest = os.path.join(library_root(), profile.product, kind, fw_id)
    if os.path.isdir(dest):
        with open(os.path.join(dest, "manifest.json"), encoding="utf-8") as f:
            return json.load(f)
    tmp = dest + ".part"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    images = []
    for img, path in files:
        name = os.path.basename(img["file"])
        shutil.copyfile(path, os.path.join(tmp, name))
        images.append({"offset": img["offset"], "file": name, "size": os.path.getsize(path),
                       "sha256": imageinfo.sha256_file(path)})
    manifest = {
        "product": profile.product, "kind": kind, "id": fw_id, "version": info.version,
        "project_name": info.project, "idf": info.idf, "built": f"{info.date} {info.time}",
        "elf_sha256": info.elf_sha256, "app_file": os.path.basename(img["file"]),
        "signed": is_signed_v2(app_path), "images": images,
        "source": source or {"build_dir": os.path.abspath(build_dir)},
        "imported": _now(), "approved": None,
    }
    with open(os.path.join(tmp, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    os.replace(tmp, dest)
    return manifest


def build_from_git(profile: Profile, kind: str, ref: str, log=print) -> dict:
    """Check out `ref` on its own, build the firmware there, import it.

    secrets.ini and keys/ are gitignored, so a fresh checkout has neither:
    they are copied over from this checkout's project folder when present
    (the default broker, the OTA signing key).
    """
    fk = profile.firmware[kind]
    commit = _git("rev-parse", "--verify", ref + "^{commit}")
    pio = shlex.split(os.environ.get("MFLASH_PIO", "pio"), posix=os.name != "nt")
    # ESP-IDF refuses a path with a space in it, and the Windows temp folder
    # often has one (C:\Users\First Last\...): build beside the repo instead.
    root = os.environ.get("MFLASH_BUILD_DIR") or os.path.join(FLASHER, "build")
    if " " in os.path.abspath(root):
        raise FirmwareError(f"ESP-IDF cannot build under {root}: it has a space; "
                            "set MFLASH_BUILD_DIR to a path without one")
    os.makedirs(root, exist_ok=True)
    work = tempfile.mkdtemp(prefix="b-", dir=root)
    tree = os.path.join(work, "src")
    log(f"  checking out {ref} ({commit[:10]}) in {tree}")
    _git("worktree", "add", "--detach", tree, commit)
    try:
        proj = os.path.join(tree, fk.project)
        here = os.path.join(REPO, fk.project)
        for extra in ("secrets.ini", "keys"):
            src = os.path.join(here, extra)
            if os.path.isfile(src):
                shutil.copyfile(src, os.path.join(proj, extra))
            elif os.path.isdir(src):
                shutil.copytree(src, os.path.join(proj, extra))
        log(f"  {' '.join(pio)} run -e {fk.env}  (in {fk.project}/)")
        r = subprocess.run([*pio, "run", "-e", fk.env], cwd=proj)
        if r.returncode:
            raise FirmwareError(f"build of {ref} failed (pio exit {r.returncode})")
        source = {"git_ref": ref, "git_commit": commit, "project": fk.project, "env": fk.env}
        return import_build(profile, kind, os.path.join(proj, ".pio", "build", fk.env), source)
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", tree], cwd=REPO,
                       capture_output=True)
        shutil.rmtree(work, ignore_errors=True)


def list_firmware(product: str, kind: str | None = None) -> list[dict]:
    root = os.path.join(library_root(), product)
    out = []
    if not os.path.isdir(root):
        return out
    for k in sorted(os.listdir(root)):
        if kind and k != kind:
            continue
        for fid in sorted(os.listdir(os.path.join(root, k))):
            mf = os.path.join(root, k, fid, "manifest.json")
            if os.path.isfile(mf):
                with open(mf, encoding="utf-8") as f:
                    out.append(json.load(f))
    # imported in the same second (a script, a demo): the higher version is the newer
    return sorted(out, key=lambda m: (m["imported"], version_key(m["version"])))


def folder(m: dict) -> str:
    return os.path.join(library_root(), m["product"], m["kind"], m["id"])


def save(m: dict) -> None:
    with open(os.path.join(folder(m), "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(m, f, indent=2)


def resolve(product: str, kind: str, which: str = "approved") -> dict:
    """An image by id or version, or the newest approved ('approved') / imported ('latest')."""
    all_ = list_firmware(product, kind)
    if not all_:
        raise FirmwareError(f"no {kind} firmware for {product} in the library "
                            f"(mflash fw build {product} {kind} <git ref>)")
    if which == "latest":
        return all_[-1]
    if which == "approved":
        ok = [m for m in all_ if m.get("approved")]
        if not ok:
            raise FirmwareError(f"no approved {kind} firmware for {product} "
                                f"(mflash fw approve {product} {kind} <id>)")
        return ok[-1]
    hits = [m for m in all_ if m["id"] == which or m["version"] == which]
    if not hits:
        raise FirmwareError(f"no {kind} firmware '{which}' for {product}")
    if len({m["id"] for m in hits}) > 1:
        raise FirmwareError(f"'{which}' names {len(hits)} builds; use the id: "
                            + ", ".join(m["id"] for m in hits))
    return hits[-1]


def approve(product: str, kind: str, which: str, who: str) -> dict:
    m = resolve(product, kind, which)
    m["approved"] = {"by": who, "at": _now()}
    save(m)
    return m


def verify_files(m: dict) -> list[tuple[int, str]]:
    """The (offset, path) list to flash, after checking every file against the manifest."""
    out = []
    for img in m["images"]:
        path = os.path.join(folder(m), img["file"])
        if not os.path.isfile(path):
            raise FirmwareError(f"{path} is gone")
        if imageinfo.sha256_file(path) != img["sha256"]:
            raise FirmwareError(f"{path} changed since it was imported")
        out.append((img["offset"], path))
    return out
