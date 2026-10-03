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

## Versions and PDFs

Each source starts with a version block:

```
---
name: User-Manual_TH      # PDF file name part
lang: th                  # th or en: labels on the page
version: 0.1              # bump when the content changes
status: draft             # draft or release
date: 2026-10-03
firmware: 0.7.0-dev       # the firmware it describes
compact: yes              # optional: tighter type, for the one-page Quick Guide
---
```

Build (Python 3 + Microsoft Edge, nothing to install):

```
python docs/manuals/build.py                  # all four
python docs/manuals/build.py quick-guide.md   # one
```

Output: `pdf/mCOLD_<name>_v<version>.pdf`, with the version under the title
and in every page footer. A new version writes a new file; delete the old
PDF in the same commit when it is superseded.

## Status

| Document | Version | Firmware |
|---|---|---|
| Quick Guide | 0.1 draft | 0.7.0-dev |
| User Manual TH | 0.1 draft | 0.7.0-dev |
| User Manual EN | 0.1 draft | 0.7.0-dev |
| Service Manual | 0.2 draft | 0.7.0-dev |
