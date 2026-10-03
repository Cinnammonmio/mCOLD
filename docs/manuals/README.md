# mCOLD manuals — sources

Markdown sources for the product documents. Edit these; PDFs are built
from them only when asked, so the sources stay the single copy.

| File | Document | Language | Reader |
|---|---|---|---|
| `quick-guide.md` | Quick Guide (one page) | Thai + English | anyone handling the box |
| `user-manual-th.md` | คู่มือการใช้งาน | Thai | operators, shippers, receivers |
| `user-manual-en.md` | User Manual | English | operators, shippers, receivers |
| `service-manual.md` | Service Manual | English | technicians, firmware and QA |

## Rules for editing

- Write only what the current firmware does and has been checked on the
  board. Anything unknown or still being tested stays out of the text and
  goes in the "Not yet included" list at the end of each file.
- The two user manuals say the same thing. Change one, change the other.
- Facts come from the firmware and its docs, not from memory:
  lights `docs/led-design.md` + `firmware/src/indicate.cpp`, screen
  `docs/display-design.md`, settings `firmware/src/config.cpp`,
  commands `print_help()` in `firmware/src/main.cpp`.
- No passwords, Wi-Fi names, broker logins or locations, ever.

## Status

Draft 1, 2026-10-03, against firmware 0.7.0-dev (P7 in progress).
Not yet converted to PDF.
