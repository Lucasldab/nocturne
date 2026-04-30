# RACES.md — Syncthing Mid-Sync Race Conditions in the Pin-Cycle

Identified by static analysis of the pin-cycle pipeline:
`nocturne-pin-cycle.path` → `nocturne-pin-cycle.service` → `nocturne-pin-cycle-runner`
→ `nocturned cycle` (scan → ingest → resolve → rotate → publish).

All races assume Syncthing is actively syncing `sync-meta` (JSONL files) or
`sync-files` (audio FLACs to phone) while the cycle runs.

---

## RACE-01 — `PathChanged` fires on `.syncthing.*.tmp` creation; mtime tie suppresses retry cycle

**Severity:** HIGH — confirmed source of "pin not reflected until next 15-min timer" symptom

**Files/Lines:**
- `config/systemd/user/nocturne-pin-cycle.path:12-13` — `PathChanged` watches whole meta dir
- `src/nocturned/ingest.c:533-535` — offset lookup + `jsonl_open` on current JSONL
- `src/nocturned/ingest.c:619-628` — offset committed after reading to EOF
- `src/nocturned/atomic_io.c:112` — `rename(tmp, path)` publishes manifest.json

**Sequence:**
```
T=0.00  Phone pins track. Syncthing starts download.
T=0.40  Syncthing creates .syncthing.pins-phone-X.jsonl.tmp in meta/.
T=0.40  PathChanged fires → nocturne-pin-cycle.service starts (after 5s debounce).
T=5.40  nocturned cycle: ingest opens pins-phone-X.jsonl (OLD version, rename not done).
        Reads 0 new events. Offset unchanged at old_EOF_N.
        Manifest published → manifest.json mtime = T5 (wall second, 1-sec resolution).
T=5.42  Syncthing renames .tmp → pins-phone-X.jsonl (new version, new events).
        PathChanged fires again on meta/ directory.
        pins-phone-X.jsonl mtime = T5 (same wall second as manifest.json).
T=5.42  nocturne-pin-cycle-runner gate check: JSONL.mtime > manifest.mtime?
        T5 > T5 → FALSE → second cycle suppressed.
        New pin silently dropped until next 15-min timer cycle.
```

**Root cause:** The runner's gate check compares `stat.st_mtime` values at 1-second resolution. When the Syncthing rename and the manifest atomic-write land in the same wall-clock second, `JSONL.mtime == manifest.mtime` instead of `JSONL.mtime > manifest.mtime`, so the retry is skipped.

**Repro steps:**
1. Put the phone on slow Wi-Fi to stretch the Syncthing transfer to 3–8 seconds.
2. Pin a track on the phone.
3. Watch `journalctl --user -u nocturne-pin-cycle.service --follow`.
4. Observe a single cycle run that emits `ingest: offsets_advanced=0` followed by no
   further run, even though the JSONL was replaced ~1s after manifest was published.
5. Pin remains missing from the phone's manifest until the 15-min timer fires.

**Suggested guards:**

*Option A — nanosecond mtime in gate check (cheapest fix):*
Replace `JSONL.mtime > manifest.mtime` with a comparison using `st_mtim.tv_sec` +
`st_mtim.tv_nsec` in `nocturne-pin-cycle-runner`. Two files written within the same
second are almost certain to differ at nanosecond granularity.

*Option B — watch specific files, not the directory:*
In `nocturne-pin-cycle.path`, replace:
```
PathChanged=%h/sync/nocturne/meta
PathChanged=%h/sync/nocturne/meta/stats
```
with `PathChanged` entries on the actual JSONL files. systemd `PathChanged` fires on
`rename()`-over (unlike `PathModified`, which does not), so this catches Syncthing's
atomic write while ignoring `.tmp` creation. The existing comment claiming
`PathModified` "would miss events" is correct for `PathModified` but not for
`PathChanged` on specific files.

*Option C — `.syncthing.*.tmp` sentinel check (belt-and-suspenders):*
In `nocturne-pin-cycle-runner`, before starting the cycle, `stat` the meta dir for
`.syncthing.pins-phone-*.jsonl.tmp`. If any exist, sleep 2 s and recheck. Only run
the cycle once no `.tmp` files are present. Syncthing always removes the `.tmp` before
exposing the new JSONL to readers.

---

## RACE-02 — Ingest opens JSONL and gets stale inode during Syncthing atomic rename

**Severity:** MEDIUM — transient; self-corrects on next cycle (unless RACE-01 suppresses it)

**Files/Lines:**
- `src/nocturned/jsonl.c:130` — `open(path, O_RDONLY)` inside `jsonl_open`
- `src/nocturned/ingest.c:535` — `jsonl_open(abs_path, old_offset)`
- POSIX: `rename()` is atomic from kernel VFS, but `open()` before vs. after is not

**Sequence:**
```
T=0    Syncthing has .syncthing.pins-phone-X.jsonl.tmp ready to rename.
T=0    cycle's ingest calls open("pins-phone-X.jsonl") → gets fd to OLD inode A.
T=1ms  Syncthing renames .tmp → pins-phone-X.jsonl → directory entry now points to
       NEW inode B (with the extra pin events). Inode A still held by ingest's fd.
T=?    ingest reads inode A to EOF (old content), finds no events past old_offset.
       Commits offset=old_EOF_N. Cycle completes; manifest published without new pin.
T=?    PathChanged fires (from the rename) → second cycle starts.
       Gate check must pass (see RACE-01) for the retry to succeed.
```

**Why not a hard bug in isolation:** `jsonl_open` uses `O_RDONLY` + `lseek` to
`old_offset`. If ingest happens to open the NEW inode (post-rename), it reads the
new events and everything is correct in one shot. If it opens the OLD inode, it reads
old content and the retry cycle fixes it — provided RACE-01 is also addressed.

**Suggested guard:** No code change needed in `ingest.c` or `jsonl.c` — the offset
model is correct. This race resolves automatically once RACE-01's gate check is fixed
to use nanosecond mtime or `.tmp` sentinel detection.

---

## RACE-03 — inotify coverage gap between watch stop and watch restart lets Syncthing deliveries go undetected

**Severity:** MEDIUM — causes "new tracks not appearing on phone until periodic rescan" symptom (the manual rescan workaround)

**Files/Lines:**
- `config/systemd/user/nocturne-cycle.service:9` — `ExecStartPre=-systemctl stop nocturne-watch.service`
- `config/systemd/user/nocturne-cycle.service:13` — `ExecStartPost=-systemctl start --no-block nocturne-watch.service`
- `config/systemd/user/nocturne-pin-cycle.service:9,13` — same stop/start pattern
- `src/nocturned/watch.c:437` — `add_watches_recursive` at startup (does not replay missed events)

**Sequence:**
```
T=0    cycle fires: ExecStartPre kills nocturne-watch.service (inotify fd closed).
T=2s   nocturned cycle runs: scan → ingest → resolve → rotate → publish.
       rotate's link() creates resident/NEW-TRACK.flac (new file, Syncthing begins
       uploading it to phone).
T=4s   ExecStartPost kicks off watch restart (--no-block, returns immediately).
T=5s   nocturne-watch.service fully running; add_watches_recursive crawls library.
       But during T=0..T=5, Syncthing may have also delivered a file (e.g., a cover
       sidecar or a FLAC that was queued) to resident/ via its own mechanism.
T=5s   watch sets up inotify, but existing files have no pending IN_MOVED_TO events.
       The file is present on disk but was never seen by the watcher.
       Not in DB. Not in manifest. Not on phone. "Manual rescan" workaround required.
```

Note: The periodic rescan in watch.c fires every 300 seconds (5 minutes), so the file
eventually appears, but the gap can exceed 5 minutes if the cycle fires just after a
periodic rescan.

**Suggested guard:**

*Option A — post-cycle incremental scan of `resident/`:*
Add an explicit `nocturned scan --subtree resident/` as the last step of the cycle
(or as part of `rotate_cmd_main` after the Syncthing rescan POST). This catches any
files delivered to `resident/` during the watch gap without needing full library scan.

*Option B — start watch before stopping it (overlapping handover):*
Change the service ordering to start the new watch instance first, let the old one
drain and stop, then hand over the lock. Requires a graceful lock-transfer protocol
(not trivial with the current `flock` design).

*Option C — shorten periodic rescan from 300s to 30s (tactical):*
Reduce `watch_opts.periodic_rescan_sec` default or add a config key. Doesn't fix the
race but bounds the exposure to 30 seconds instead of 300.

---

## RACE-04 — rotate.c unlink/DB update non-atomicity: track disappears on crash between steps

**Severity:** LOW — requires a crash mid-rotate; no Syncthing trigger needed, but
Syncthing's concurrent rescan POST makes this window more likely to be observed

**Files/Lines:**
- `src/nocturned/rotate.c:603` — `link(archive/X, resident/X)`
- `src/nocturned/rotate.c:651` — `unlink(archive/X)` (source removed)
- `src/nocturned/rotate.c:659` — `track_repo_update_path(db, sha, resident/X)` (not yet run)
- `src/nocturned/rotate.c:17-25` — comment acknowledges partial-atomicity and retry-convergence

**Sequence (crash path):**
```
1. link(archive/X, resident/X) — succeeds; file now at both paths.
2. unlink(archive/X) — succeeds; only resident/X remains.
   [CRASH / SIGKILL here]
3. track_repo_update_path never called; DB still says path=archive/X.
```

**On next rotate run:**
- `lookup_track_path()` returns `archive/X`.
- `swap_segment(library_root, archive/X, "archive", "resident")` → `resident/X`.
- `link(archive/X, resident/X)` → **ENOENT** (archive/X was already unlinked).
- Not EEXIST, not EXDEV → error branch at `rotate.c:642`: logs error, increments
  `out->errors`, returns -1.
- Track is stuck: it exists at `resident/X` on disk but DB thinks it's at `archive/X`.
  The manifest never lists it; Syncthing never syncs it to phone.

The documented retry-convergence (comment lines 17-25) handles crash-between-link-and-unlink
(EEXIST + same inode → completes the unlink). It does **not** handle
crash-after-unlink-before-DB-update (ENOENT case).

**Suggested guard:** Before calling `link(cur, new_path)`, check whether `new_path`
already exists and points to an inode matching a file in the library. If so, treat as
already-applied and skip to the DB update. Alternatively, persist a "pending-move"
sentinel row in a `rotate_pending` table inside the same SQLite transaction as the
DB update — if the row exists on next startup, the daemon knows the filesystem move
was completed but the DB update was not.

A simpler guard: after the unlink, log a structured journal line
(`rotate: unlinked archive/X; DB update follows`) so that a `journalctl` scan can
identify orphaned tracks if the daemon dies between these two lines.

---

## RACE-05 — JSONL offset overshoot after file truncation / phone app reinstall

**Severity:** LOW — only triggered by phone app reinstall or Syncthing conflict
resolution that shortens the JSONL

**Files/Lines:**
- `src/nocturned/ingest.c:533` — `lookup_offset(db, relpath)` returns persisted offset
- `src/nocturned/jsonl.c:132-135` — `lseek(fd, start_offset, SEEK_SET)`; succeeds even
  if `start_offset > file_size` (seek past EOF is allowed)
- `src/nocturned/jsonl.c:274-278` — trailing partial: `new_offset == old_offset` → no update

**Sequence:**
```
1. Desktop has processed pins-phone-X.jsonl to offset N (10 events, 840 bytes).
   ingest_offsets table: { path: "pins-phone-X.jsonl", offset: 840 }.
2. Phone reinstalls app. JSONL reset to empty (0 bytes). Syncthing syncs new file.
3. ingest opens pins-phone-X.jsonl at offset 840. lseek(840) succeeds (past EOF).
   jsonl_read_line returns 0 (EOF). new_offset == 840 == old_offset → not updated.
4. New pin events appended at positions 0..M (M < 840) are permanently invisible:
   the persisted offset (840) always overshoots the new file end (M).
5. Desktop DB still reflects stale pins from the old install; new pins never ingest.
```

**Suggested guard:** In `ingest_one_file` (`ingest.c:522`), after `lookup_offset`
and before `jsonl_open`, `stat` the file and compare `old_offset` to `st.st_size`.
If `old_offset > st.st_size`, reset `old_offset = 0` (re-process from start). LWW
semantics (`WHERE excluded.ts >= pins.ts`) make full re-play idempotent — duplicate
events with the same or older `ts` are silently ignored by the UPSERT.

```c
/* Reset overshoot (file truncated / replaced on phone). */
struct stat st;
if (stat(abs_path, &st) == 0 && old_offset > (long long)st.st_size) {
    old_offset = 0;
}
```

---

## Summary table

| ID      | Where                                   | Trigger                              | Severity | Self-heals?                   | Guard                              |
|---------|-----------------------------------------|--------------------------------------|----------|-------------------------------|------------------------------------|
| RACE-01 | `nocturne-pin-cycle.path:12-13`         | Syncthing .tmp creation fires trigger early; mtime tie | HIGH | After 15-min timer | Nanosecond mtime or .tmp sentinel  |
| RACE-02 | `ingest.c:535`, `jsonl.c:130`           | ingest opens stale inode during rename | MEDIUM | On next PathChanged (if gate passes) | Fix RACE-01 gate check             |
| RACE-03 | `nocturne-cycle.service:9,13`, `watch.c:437` | inotify gap during stop/start | MEDIUM | After 300-s periodic rescan  | Post-cycle subtree scan of `resident/` |
| RACE-04 | `rotate.c:603,651,659`                  | crash between unlink and DB update   | LOW      | No (track stuck)              | Pending-move log row or ENOENT recovery |
| RACE-05 | `ingest.c:533`, `jsonl.c:132`           | JSONL truncated (phone reinstall)    | LOW      | No (offset stuck)             | `old_offset > st.st_size → reset`  |
