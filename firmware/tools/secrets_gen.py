"""PlatformIO pre script: the broker every box starts with, from secrets.ini.

secrets.ini (gitignored, shape in secrets.example.ini) becomes
src/secrets_gen.h (gitignored too), which uplink.cpp includes. A box with no broker of its own in NVS (`mqtt set`) uses it. With
no secrets.ini the header is empty and boxes start with no broker, as
before -- the repository never holds the login.
"""
import configparser
import os

Import("env")  # noqa: F821  (PlatformIO)

ROOT = env.subst("$PROJECT_DIR")  # noqa: F821
INI = os.path.join(ROOT, "secrets.ini")
OUT = os.path.join(ROOT, "src", "secrets_gen.h")


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_if_changed(path, text):
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    if old != text:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


# Always written, empty without a login: a header that comes and goes is
# not a dependency the build tracks, and uplink.cpp kept its old object
# when secrets.ini first appeared (2026-10-05).
lines = ["// Generated from secrets.ini by tools/secrets_gen.py. Not in git.", "#pragma once"]
if os.path.exists(INI):
    cp = configparser.ConfigParser()
    cp.read(INI, encoding="utf-8")
    m = cp["mqtt"] if cp.has_section("mqtt") else {}
    host = m.get("host", "").strip()
    if host:
        lines += [
            "#define MQTT_DEFAULT_HOST " + c_str(host),
            "#define MQTT_DEFAULT_PORT " + str(int(m.get("port", "1883") or 1883)),
            "#define MQTT_DEFAULT_USER " + c_str(m.get("username", "").strip()),
            "#define MQTT_DEFAULT_PASS " + c_str(m.get("password", "").strip()),
        ]
        print("Default broker from secrets.ini: " + host)
write_if_changed(OUT, "\n".join(lines) + "\n")
