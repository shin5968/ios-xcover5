# XCover5 (SM-G525N) — read this first

This branch (`xcover5`, remote `xc5` = shin5968/ios-xcover5) is the Galaxy XCover5 port. **Start with
`docs/xcover5-handoff.md`** (state, build flow, boot-args, next action, tools) and update its "지금 상태" and
"다음 행동" sections at every milestone, then commit. Reply to the user in Korean. Keep device dumps, serials and
Apple binaries out of git. Scripts for building and analysing are in `tools/xcover5/dev/`.

# Working on the A5XELTE fork

- For Samsung SM-A510F work, read `A5XELTE_PROGRESS.md` and `docs/a5xelte-migration-map.md` first. Record new device evidence in the former and commit completed changes. Keep private VM addresses, passwords, device dumps and downloaded Apple binaries out of git.
- Preserve recovery p11, EFS p3 and the existing Linux BOOT checkpoint. Current XNU trials may write only the explicitly authorized BOOT p10; the donor's Xiaomi `fastboot` and USERDATA instructions are not A5XELTE instructions.
- Keep build success, recovered kernel markers, physical screen reports and iOS runtime behavior as separate claims. `NOTES.md` below is SWEET donor history; do not edit it to imply Samsung behavior.

## Original SWEET contributor instructions (donor reference)

- Read `NOTES.md` before starting; it contains the project goal, hardware facts,
  current blocker, local artifact locations and the next concrete action.
- The user switches agents frequently. Update the active handoff in `NOTES.md`
  after each milestone and before stopping. Record commits, CI run IDs, exact
  build/boot results and unfinished work. Distinguish observations from guesses.
- Commit completed changes. Use `gh` for GitHub Actions kernel builds and logs.
  Local single-file cross-compiles are useful before spending a CI run.
- Device bring-up uses temporary `fastboot boot` images. Preserve the Android
  installation and partition backups. The user approved (2026-10-15) using
  internal UFS for iOS and said Android may be deleted: the iOS root goes
  into the existing `userdata` partition (no GPT changes). Keep bootloaders,
  recovery, modem, persist and the backups intact, and confirm each
  destructive step with the user before running it.
- Keep device dumps, serial numbers and downloaded Apple artifacts out of git.
