import os
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

FONT_BASE = "https://raw.githubusercontent.com/google/fonts/main/ofl/ibmplexsansthai/"
FONTS = ["Light", "Regular", "Medium", "SemiBold", "Bold"]

ICON_BASE = "https://cdn.jsdelivr.net/npm/@tabler/icons@3/icons/outline/"
ICONS = [
    "temperature", "door", "activity", "map-pin", "bell-ringing", "device-tablet",
    "nfc", "wifi", "cloud-upload", "database", "usb", "battery-charging",
    "battery-3", "bluetooth", "device-mobile", "clock", "truck-delivery",
    "circle-check", "flag", "building-warehouse", "file-spreadsheet",
    "alert-triangle", "info-circle", "device-sd-card", "power", "plug",
    "package", "snowflake", "server", "device-laptop", "hand-finger",
    "shield-check", "x", "check", "arrow-right", "player-play", "player-stop",
    "chart-line", "users", "user", "wifi-off", "refresh", "eye",
]


def fetch(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return "cached"
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=30) as r, open(dest, "wb") as f:
        f.write(r.read())
    return "ok"


for w in FONTS:
    name = f"IBMPlexSansThai-{w}.ttf"
    try:
        print(name, fetch(FONT_BASE + name, os.path.join(HERE, "fonts", name)))
    except Exception as e:
        print(name, "FAIL", e)

for n in ICONS:
    try:
        fetch(ICON_BASE + n + ".svg", os.path.join(HERE, "icons", n + ".svg"))
    except Exception as e:
        print("icon", n, "FAIL", e)
print("icons:", len(os.listdir(os.path.join(HERE, "icons"))))
