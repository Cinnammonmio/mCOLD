"""Talking to the person at the station.

Several boxes run at once, one thread each, but there is one person:
questions are asked one at a time, each prefixed with the box it is
about, and a box waits its turn rather than talking over another.
"""
from __future__ import annotations

import sys
import threading
import time


class Ui:
    def info(self, who: str, msg: str) -> None: ...
    def yes_no(self, who: str, question: str) -> bool: ...
    def text(self, who: str, question: str, default: str = "") -> str: ...

    # progress, for a window that shows it; the terminal reads info() alone
    def stage_started(self, who: str, stage: str, steps: int) -> None: ...
    def step_started(self, who: str, idx: int, name: str) -> None: ...
    def step_done(self, who: str, idx: int, name: str, result: str, detail: str,
                  help: str) -> None: ...
    def stage_done(self, who: str, stage: str, result: str) -> None: ...


class TerminalUi(Ui):
    def __init__(self, assume_yes: bool = False, stream=sys.stdout):
        self._lock = threading.RLock()
        self.assume_yes = assume_yes
        self.out = stream

    def info(self, who: str, msg: str) -> None:
        with self._lock:
            for line in msg.splitlines() or [""]:
                print(f"{time.strftime('%H:%M:%S')} [{who}] {line}", file=self.out, flush=True)

    def yes_no(self, who: str, question: str) -> bool:
        with self._lock:
            if self.assume_yes:
                self.info(who, f"{question} -> y (--yes)")
                return True
            while True:
                a = input(f"\a[{who}] {question} [y/n] ").strip().lower()
                if a in ("y", "yes", "ใช่", "ผ่าน"):
                    return True
                if a in ("n", "no", "ไม่", "ไม่ผ่าน"):
                    return False

    def text(self, who: str, question: str, default: str = "") -> str:
        with self._lock:
            hint = f" [{default}]" if default else ""
            a = input(f"\a[{who}] {question}{hint}: ").strip()
            return a or default


class ScriptedUi(Ui):
    """Answers from a list, for tests and for unattended runs."""

    def __init__(self, answers: list | None = None):
        self.answers = list(answers or [])
        self.log: list[str] = []

    def info(self, who: str, msg: str) -> None:
        self.log.append(f"[{who}] {msg}")

    def yes_no(self, who: str, question: str) -> bool:
        self.log.append(f"[{who}] ? {question}")
        return bool(self.answers.pop(0)) if self.answers else True

    def text(self, who: str, question: str, default: str = "") -> str:
        self.log.append(f"[{who}] ? {question}")
        return str(self.answers.pop(0)) if self.answers else default
