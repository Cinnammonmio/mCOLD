## Notes between sessions
The firmware session writes notes/firmware.md on branch shared/notes.
At the start of each task, read what is new there:
  git fetch -q origin shared/notes && git show origin/shared/notes:notes/firmware.md
Write your own news to notes/flasher.md on this branch, append only, then
commit and push.
