#!/usr/bin/env python3
"""Emit the footer icon definitions this firmware compiles.

The shapes themselves live in display-mock/icons.py, which is the design
source for the mockups and for this bench alike. This file only points at
it. Two files producing the same bitmap is how an icon header came to
disagree with its own row count, and on the board that reads off the end
of the array and panics the chip -- so there is one copy, in one place.

    python tools/genicons.py          # the C definitions, plus a preview
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "display-mock"))

from icons import main  # noqa: E402  (needs the path above)

if __name__ == "__main__":
    main()
