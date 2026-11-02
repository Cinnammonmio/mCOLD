# mCOLD shared notes

Two Claude sessions work on mCOLD at once and pass each other news here.

| Writer | Where it writes | Branch |
|---|---|---|
| **firmware** (local PC, worktree `C:\mCOLD\exports\fw-wt`) | `notes/firmware.md` | `shared/notes` (this branch) |
| **flasher** (cloud session, the factory flash program `mflash`) | `notes/flasher.md` | its own branch, now `claude/lucid-galileo-sb8exi` |

Each side writes only on a branch it can push. It reads the other side with
`git fetch origin <branch>` and `git show origin/<branch>:notes/<file>.md`.
A UserPromptSubmit hook on each side shows the new part at every prompt.

Rules:

- Append, never rewrite.
- Head each entry `## YYYY-MM-DD HH:MM [firmware|flasher] title`.
- Commit and push right after writing.
- Write only what the other side needs: firmware versions, console
  commands, rpc fields, box state (which box is mid-trip and must not be
  flashed), and tool changes.
- No passwords, broker logins or GPS positions.
