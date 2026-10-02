#!/usr/bin/env python3
"""Reference client for PROTOCOL.md: does over BLE what the iOS app does.

    pip install bleak pyserial
    python tools/ble_client.py COM7           # key from the USB console
    python tools/ble_client.py --key <hex32>  # key read from the tag by hand

The key normally comes from tapping the NFC tag. A PC has no NFC, so this
reads the same record through the USB console's `nfc` command -- opening
the port resets the board, which also gives it a new key, so the script
waits for the board to come back before asking. With --key, nothing
touches the USB port.

It then runs the authorization handshake and a handful of commands, and
checks the cases that must fail: a command before AUTH, a wrong proof.
Everything a phone app has to get right is in this one file: the
fragment framing, the nonce from INFO, HMAC-SHA256(key, nonce), request
ids, and reassembling the notified response.
"""
import argparse
import asyncio
import hashlib
import hmac
import json
import re
import sys
import time

from bleak import BleakClient, BleakScanner

BASE = "-3818-4667-a205-b3ed9c37b8a5"
INFO, STATUS, CMD, RSP, EVT = (f"d2a5000{i}{BASE}" for i in (1, 2, 3, 4, 5))
SERVICE = f"d2a50000{BASE}"
FIRST, LAST, INDEX = 0x80, 0x40, 0x3F


def key_over_usb(port_name):
    import serial
    port = serial.Serial(port_name, 115200, timeout=0.2)   # resets the board
    time.sleep(9)
    port.read(100000)
    port.write(b"nfc\r\n")
    end, out = time.time() + 3, b""
    while time.time() < end:
        out += port.read(4096)
    m = re.search(rb'"sn":"([^"]+)".*"key":"([0-9a-f]{32})"', out)
    if not m:
        sys.exit("no key in the tag record: is the board up?")
    return m.group(1).decode(), bytes.fromhex(m.group(2).decode()), port


class Link:
    """Requests out through COMMAND, answers back through RESPONSE."""

    def __init__(self, client):
        self.c = client
        self.buf = b""
        self.answers = asyncio.Queue()
        self.next_id = int(time.time()) % 1000000 * 10

    def on_response(self, _, data: bytearray):
        if data[0] & FIRST:
            self.buf = b""
        self.buf += bytes(data[1:])
        if data[0] & LAST:
            self.answers.put_nowait(self.buf.decode())

    async def call(self, req, req_id=None):
        self.next_id += 1
        req = dict(req, id=req_id or self.next_id)
        raw = json.dumps(req, separators=(",", ":")).encode()
        room = self.c.mtu_size - 3 - 1          # ATT header, then our flags byte
        parts = [raw[i:i + room] for i in range(0, len(raw), room)]
        for i, part in enumerate(parts):
            flags = (FIRST if i == 0 else 0) | (LAST if i == len(parts) - 1 else 0) | (i & INDEX)
            await self.c.write_gatt_char(CMD, bytes([flags]) + part, response=True)
        return json.loads(await asyncio.wait_for(self.answers.get(), 10))


def show(label, answer):
    text = json.dumps(answer)
    print(f"  {label:22} {text[:140]}{'...' if len(text) > 140 else ''}")


async def run(sn, key):
    print(f"  looking for {sn} ...")
    dev = await BleakScanner.find_device_by_name(sn, timeout=20)
    if not dev:
        sys.exit(f"{sn} not advertising (tap the tag, or power it from USB)")
    async with BleakClient(dev) as c:
        link = Link(c)
        await c.start_notify(RSP, link.on_response)
        info = json.loads((await c.read_gatt_char(INFO)).decode())
        print(f"  connected: fw {info['fw']}, protocol {info['proto']}, MTU {c.mtu_size}")
        nonce = bytes.fromhex(info["nonce"])

        show("before AUTH", await link.call({"cmd": "STOP_TRIP"}))
        show("AUTH, wrong proof", await link.call({"cmd": "AUTH", "proof": "00" * 32}))
        proof = hmac.new(key, nonce, hashlib.sha256).hexdigest()
        show("AUTH", await link.call({"cmd": "AUTH", "proof": proof}))
        show("GET_STATUS", await link.call({"cmd": "GET_STATUS"}))
        show("LIST_TRIPS", await link.call({"cmd": "LIST_TRIPS"}))
        show("GET_STORAGE_STATUS", await link.call({"cmd": "GET_STORAGE_STATUS"}))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?", help="USB console port, to read the key")
    ap.add_argument("--key", help="the key from the tag, 32 hex digits")
    ap.add_argument("--sn", default=None, help="device SN (with --key)")
    a = ap.parse_args()
    if a.key:
        if not a.sn:
            sys.exit("--key needs --sn too")
        asyncio.run(run(a.sn, bytes.fromhex(a.key)))
    elif a.port:
        sn, key, port = key_over_usb(a.port)
        print(f"  {sn}, key {key.hex()} (as a tap would read it)")
        try:
            asyncio.run(run(sn, key))
        finally:
            port.close()
    else:
        ap.error("give the USB port, or --key and --sn")


if __name__ == "__main__":
    main()
