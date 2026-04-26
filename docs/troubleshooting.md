# nocturne — troubleshooting

Symptom-keyed. Ctrl-F the symptom you're hitting; each entry has
**Cause**, **Fix**, and a Pitfall reference.

Pitfall numbers reference [`.planning/research/PITFALLS.md`][pf] —
read the long form there for context.

[pf]: ../.planning/research/PITFALLS.md

---

## Symptom: phone Syncthing-Fork shows phone storage filling up despite the manifest cap

**Cause:** the sync-files folder has versioning enabled (Trash Can,
Staggered, Simple, External). Each "deletion" via rotate's
unlink+link is being preserved in `.stversions/` on the phone,
doubling effective storage. (see PITFALLS.md Pitfall 24)

**Fix:**

1. Phone, Syncthing-Fork → sync-files folder → Edit → Versioning.
2. Set type to **None**. Save.
3. Delete `/storage/emulated/0/Music/nocturne/.stversions/`
   (file manager — it's a hidden directory).
4. On desktop, also verify: Syncthing GUI → sync-files folder →
   versioning = None.

The XML `nocturned sync-config --print` emits already specifies
`<versioning type="none"/>` — but if you imported the folder manually
on the phone (Option B in phone-setup.md), Syncthing-Fork might have
defaulted to a non-none versioning. Re-edit the folder.

---

## Symptom: `nocturned rotate` succeeds but tracks don't appear on the phone

**Cause:** Syncthing on phone hasn't rescanned the desktop folder
since the last rotation, OR the phone is offline. (see PITFALLS.md
Pitfall 1 sub-case)

**Fix:**

1. On desktop, check Syncthing GUI sees the folder is up-to-date with
   `nocturne-phone`.
2. On phone, Syncthing-Fork → sync-files folder → tap "Rescan".
3. Confirm the phone is on WiFi (per-folder Power Conditions are
   per-folder; if WiFi-only is set, mobile data won't trigger sync).
4. If `rotate` printed "warn: syncthing rescan POST failed", it means
   the desktop never told Syncthing to rescan. See "Daemon says rescan
   POST failed" below.

---

## Symptom: sync-conflict files appear (`*.sync-conflict-*.json`) in `sync-meta/`

**Cause:** both the desktop and the phone wrote the same file path
between syncs. For sync-files this should NEVER happen (it's
Send-Only on desktop / Receive-Only on phone). For sync-meta it CAN
happen if the phone's Phase 7 ingester (future) writes the same JSONL
filename a desktop process is also writing. (see PITFALLS.md Pitfall 8)

**Fix:**

1. Identify the offending paths:
   ```sh
   find ~/sync/nocturne/meta -name '*.sync-conflict-*' -print
   ```
2. Inspect each. Decide which copy is canonical.
3. `rm` the conflict copies after merging by hand.

Phase 7 (ingester) will be stricter about per-device filename
namespacing so this can't recur — track in
`.planning/STATE.md` if observed.

---

## Symptom: `nocturned migrate --apply` reports `errors=N` for some tracks

**Cause:** per-track filesystem errors during migrate — usually
EACCES (wrong user owns some files), EXDEV (cross-fs library — handled
gracefully via copy-fallback, but logged), or unicode-path edge cases.
(see PITFALLS.md Pitfall 22)

**Fix:**

1. Re-read the stderr from the migrate run; each error line shows
   the offending sha256 and what failed.
2. EACCES: ensure you ran migrate as the user owning the music
   library (`stat ~/music/library/some.flac` — does it match $USER?).
3. EXDEV: NOT an error in the strict sense — migrate falls back to
   copy+unlink. See `fallback=N` in the summary; if this is non-zero,
   that's the count.
4. After fixing the root cause, rerun `nocturned migrate ~/music/library --apply`.
   Migrate is idempotent; previously-moved tracks are skipped.

---

## Symptom: phone Syncthing-Fork shows "Out of Sync" permanently

**Cause:** Syncthing's Minimum Free Disk Space check is preventing
sync — phone is below the threshold. (see PITFALLS.md Pitfall 10)

**Fix:**

1. Check phone storage:
   ```
   Settings → Storage on the phone, OR plug phone in and run
   `df -h /storage/emulated/0` from a desktop adb shell.
   ```
2. Either free space, or shrink the manifest cap in your desktop
   `~/.config/nocturne/nocturne.toml`:
   ```toml
   [cap]
   bytes = 8589934592    # 8 GiB instead of 12 GiB
   ```
3. Rerun `nocturned resolve && nocturned rotate` — the smaller
   manifest will evict the bottom of the rotation set, giving phone
   storage breathing room.

---

## Symptom: `nocturned rotate` says "syncthing rescan POST failed"

**Cause:** Syncthing not running, or the daemon can't read its config.
(see PITFALLS.md Pitfall 26 — generally the libcurl wrapper is
working; the issue is at the Syncthing/config layer)

**Fix:**

1. Confirm Syncthing is up:
   ```sh
   systemctl --user status syncthing
   ```
   If inactive: `systemctl --user start syncthing`.
2. Confirm `~/.config/syncthing/config.xml` exists:
   ```sh
   test -f ~/.config/syncthing/config.xml && echo OK
   ```
3. Confirm it has an `<apikey>`:
   ```sh
   grep -oP '<apikey>\K[^<]+' ~/.config/syncthing/config.xml
   ```
4. If your Syncthing config lives in a non-default place, set the env:
   ```sh
   export NOCTURNE_SYNCTHING_CONFIG="$HOME/somewhere-else/syncthing"
   nocturned rotate
   ```

The rotate's file motion succeeded regardless — Syncthing's own
watcher will pick up the changes within its scan interval (default 1
hour). The warning means rescan was a noop, not that rotate failed.

---

## Symptom: phone Syncthing-Fork shows the phone's IP/hostname instead of `nocturne-phone`

**Cause:** device name was never overridden on the phone — Syncthing's
default is the make+model. (see PITFALLS.md Pitfall 20)

**Fix:**

1. Phone, Syncthing-Fork → Settings → General → Device name →
   `nocturne-phone`.
2. (Optional) restart Syncthing-Fork.

The desktop side has a similar trap; if the desktop GUI shows
`<my-hostname>` rather than `nocturne-desktop`, fix in
Settings → General on the desktop GUI. The XML `nocturned sync-config
--print` emits sets the right name; this only matters if you let
Syncthing run before applying the config.

---

## Symptom: modifying an audio file on the phone DOESN'T revert (SYNC-07 fails)

**Cause:** sync-files folder type is **Send & Receive** (or just
**Send Only**) on the phone, not **Receive Only**. (see PITFALLS.md
Pitfall 9)

**Fix:**

1. Phone, Syncthing-Fork → sync-files folder → Edit → Folder type →
   **Receive Only**. Save.
2. Confirm desktop side: Syncthing GUI → sync-files → Edit → Folder
   type → **Send Only**.
3. Re-run SYNC-07 (modify a file on phone, observe it revert).

If both sides are correctly Send-Only / Receive-Only and the file
STILL doesn't revert: ensure the rescan interval has elapsed (default
3600s) or tap "Rescan" manually on the phone.

---

## Symptom: `tests/test_no_network.sh` (CROSS-03 audit) fails with "libcurl pulled in"

**Cause:** plan 03-03 introduced libcurl; plan 03-07 refreshes the
audit to allow loopback-scoped libcurl. If you're between those two
states (e.g. checked out a snapshot of the codebase mid-phase), the
audit will reject libcurl. (see PITFALLS.md none — this is a Phase 3
mid-state)

**Fix:**

1. `git log --oneline | grep 03-07` — if the plan-03-07 commit is in
   your tree, the audit should pass.
2. If not, you're on a phase-3 mid-state. Check out the phase-3 close
   tag (or HEAD of main if Phase 3 has shipped) and rerun.

If plan 03-07 IS in place but the audit still fails: examine the
strace layer output (layer 4). Any non-127.0.0.1 connect() is a real
regression — file an issue.

---

## Symptom: `.stignore` evaluation says > 10s on the desktop sync-files folder

**Cause:** someone manually added pattern lines to
`~/music/library/.stignore`. The path-layout selective-sync mechanism
(per CONTEXT.md locked decision) means `.stignore` should be EMPTY —
the path layout itself selects what syncs. (see PITFALLS.md Pitfall 2)

**Fix:**

1. ```sh
   wc -l ~/music/library/.stignore
   ```
   Expected: 0. If non-zero, you (or a previous tooling iteration)
   manually added patterns.
2. ```sh
   : > ~/music/library/.stignore
   ```
3. Restart Syncthing or wait for next rescan; cost should drop to <1s.

---

## Symptom: WiFi-only sync isn't being respected on phone

**Cause:** WiFi-only is a Syncthing-Fork **per-folder** setting, NOT
global. If you set the global "Run on WiFi only" toggle but didn't
configure the per-folder Power Conditions, mobile-data syncs slip
through. (see PITFALLS.md Pitfall 25)

**Fix:**

1. Phone, Syncthing-Fork → Settings → Per-folder Power Conditions.
2. For sync-files: enable "Run on WiFi only".
3. Verify by toggling phone WiFi off; Syncthing-Fork should pause
   sync-files within ~30 seconds.

---

## Symptom: daemon refuses to start with "lock busy" error

**Cause:** another `nocturned` is already running — likely a
`nocturned watch` under systemd, holding the single-writer lock.
(see PITFALLS.md none — DAEMON-04 single-writer invariant by design)

**Fix:**

1. Check who's holding it:
   ```sh
   cat ~/.cache/nocturne/nocturned.pid
   ps -p $(cat ~/.cache/nocturne/nocturned.pid)
   ```
2. If it's your `watch` service: stop it first, run your one-shot
   command, restart watch.
   ```sh
   systemctl --user stop nocturned-watch
   nocturned migrate ~/music/library --apply
   systemctl --user start nocturned-watch
   ```
3. Note: `nocturned doctor` is **lock-free** — it works while watch
   is up. Use it for diagnostics without disturbing the watcher.

---

## Symptom: `nocturned doctor` reports `schema_version: 2` (or 1)

**Cause:** the migration didn't run — usually because the daemon
crashed mid-migration on a previous boot. (see PITFALLS.md none —
this is a state-drift case)

**Fix:**

1. ```sh
   sqlite3 ~/.local/state/nocturne/nocturne.db 'PRAGMA user_version'
   ```
2. If output < 3, the on-disk DB hasn't run migration 0003. The next
   `nocturned <anything>` invocation should advance it via
   `migrations_apply`. If it doesn't, look for prior crash messages
   in the daemon's stderr.
3. Worst case: back up the DB, delete it, rerun `nocturned scan` to
   regenerate from scratch.
   ```sh
   cp ~/.local/state/nocturne/nocturne.db ~/.local/state/nocturne/nocturne.db.backup
   rm ~/.local/state/nocturne/nocturne.db
   nocturned scan ~/music/library
   ```
   You'll lose play history (Phase 7 hasn't shipped yet so this is
   recoverable from JSONL deltas if any exist).

---

## Symptom: integration test (`make test-integration-rotate`) skips with "syncthing not installed"

**Cause:** the test is hermetic — it spawns its own Syncthing under a
tmpdir. If `syncthing` isn't in PATH, the test exits 77 (the
conventional skip code).

**Fix:**

```sh
sudo pacman -S syncthing
```

Then `make test-integration-rotate` again.

---

## Where to look when nothing here matches

1. `nocturned doctor --json` — full health report.
2. `~/.local/state/nocturne/nocturne.db` — open with `sqlite3` and
   inspect `tracks`, `residency_state`, `manifest_current`,
   `manifest_meta`.
3. Syncthing GUI logs (`https://127.0.0.1:8384` → "Logs" tab) —
   cross-reference with `~/music/library/.stversions` (should not
   exist if config is correct).
4. `dmesg` if filesystem-level errors are suspected (filesystem
   corruption, ENOSPC, EXDEV).
