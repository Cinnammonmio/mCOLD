"""The box's text console over USB: send a line, read what comes back.

Two rules from the bench (firmware/HANDOFF.md, section 4):

- DTR and RTS are held low *before* the port opens. On the chip's
  USB-Serial-JTAG they are the reset and BOOT lines, so a plain open
  resets the box and anything set up a moment earlier is gone.
- A reply has no end marker. It is read until the line has been quiet
  for a while, or until a pattern that ends it has arrived (`until`), for
  commands such as `logtest` that pause between lines.
"""
from __future__ import annotations

import re
import time

import serial


class ConsoleError(Exception):
    pass


class Console:
    def __init__(self, port: str, baud: int = 115200, eol: str = "\r\n"):
        self.port, self.baud, self.eol = port, baud, eol
        self._s: serial.Serial | None = None

    def open(self) -> "Console":
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = self.port, self.baud, 0.1
        s.dtr = s.rts = False
        try:
            s.open()
        except (serial.SerialException, OSError) as e:
            raise ConsoleError(f"{self.port}: {e}") from e
        self._s = s
        return self

    def close(self) -> None:
        if self._s:
            try:
                self._s.close()
            except (serial.SerialException, OSError):
                pass
            self._s = None

    def __enter__(self) -> "Console":
        return self.open() if not self._s else self

    def __exit__(self, *exc) -> None:
        self.close()

    def drain(self, quiet: float = 0.3, limit: float = 3.0) -> str:
        """Whatever the box is saying on its own (a boot banner, a log line)."""
        return self._read(quiet, limit, None)

    def command(self, line: str, quiet: float = 1.5, timeout: float = 10.0,
                until: str | None = None, may_vanish: bool = False) -> str:
        """Send one command and return its reply.

        `may_vanish`: the command restarts the box (`reboot`, `flash`), so
        the port going away while reading is the expected outcome.
        """
        if not self._s:
            self.open()
        try:
            self._s.reset_input_buffer()
            self._s.write((line + self.eol).encode())
            self._s.flush()
            return self._read(quiet, timeout, until)
        except (serial.SerialException, OSError) as e:
            if may_vanish:
                self.close()
                return ""
            raise ConsoleError(f"{self.port}: {e}") from e

    def _read(self, quiet: float, timeout: float, until: str | None) -> str:
        pat = re.compile(until) if until else None
        buf = b""
        start = last = time.monotonic()
        while True:
            now = time.monotonic()
            if now - start > timeout:
                break
            if not pat and buf and now - last > quiet:
                break
            if not pat and not buf and now - start > max(quiet, 2.0):
                break
            b = self._s.read(4096)
            if b:
                buf += b
                last = time.monotonic()
                if pat and pat.search(buf.decode("utf-8", "replace")):
                    # the rest of the line that ended it
                    time.sleep(0.2)
                    buf += self._s.read(4096)
                    break
        return strip_echo(buf.decode("utf-8", "replace"))


def strip_echo(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")
