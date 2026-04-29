# Phase 8 Hardware-Acceptance Checklist

The Phase 8 close-out is half install / half observe. The install side
(CROSS-04) is a one-shot acceptance of the full nocturne stack on the
user's actual Arch + Hyprland desktop and GrapheneOS phone — the only
target environments. The observe side (TUNE-01) is a structured 7-day
listening window that ratifies the bucket model against real listening,
captured in `.planning/phases/08-bucket-tuning-v1-milestone/08-RATIFICATION.md`.

Two requirements gate v1:
- **TUNE-02 (1 GiB margin)** — must hold throughout the observation window.
- **Rubric Q5 (rotation cycles where used_bytes > cap_bytes)** — target
  MUST be 0; any non-zero count blocks v1.

Three requirements are advisory (subjective; recorded but not v1-blocking):
- TUNE-01 — "resident set has shifted in a way the user finds reasonable"
  is the user's call.
- TUNE-03 — `nocturned why <id>` answers correctly (sanity-checked once;
  the empirical use across the window is informational).
- CROSS-04 — daemon + APK install runs unmodified on real hardware.

Out of scope per Phase 8 SCOPE AMENDMENT (2026-04-27):
- Cross-machine F-Droid reproducible build (CROSS-02) → POLISH-11 (v1.x).
  Same-machine `phone/scripts/repro-build-check.sh` retained as the
  Phase 4 sanity gate; cross-machine container-pinned reproducibility
  is NOT a Phase 8 gate.
- Phone-side "Why pinned" Compose sheet → POLISH-08 (v1.x). The CLI
  `nocturned why <id>` (Plan 08-01) is the v1 surface for TUNE-03.

Prerequisites:
- All Phase 8 plans (08-01 / 08-02 / 08-03) shipped and `make test` green.
- All Phase 6 audit gates green (`audit-permissions.sh`, `audit-network.sh`,
  `lintDebug`, `testDebugUnitTest`).
- User has paired their Arch + Hyprland desktop + GrapheneOS phone with
  Syncthing-Fork on the phone side per `docs/phone-setup.md`.
- Phase 6 hardware-acceptance procedures (STATS-01/02/03/05/06) green
  (or in flight — Phase 6 hardware tests can run *during* the Phase 8
  observation window per RESEARCH Decision 7).

---

## CROSS-04 — Both halves install unmodified on the user's hardware

**Requirement:** Both daemon and phone app install on the user's actual
Arch + Hyprland desktop and GrapheneOS phone (the only target environments)
without modifications. Phone APK sideloaded via Obtainium / `adb install`
(NOT F-Droid for v1 per SCOPE AMENDMENT).

### Setup

1. Desktop is at clean git HEAD on `main`.
2. `nocturned` build dependencies installed (`pacman -S sqlite taglib jansson curl base-devel`).
3. GrapheneOS phone has Syncthing-Fork installed and paired with desktop
   Syncthing per `docs/phone-setup.md`.
4. Phone has F-Droid + Obtainium installed (Obtainium can sideload from a
   git-release pointer; `adb install` is the fallback path).

### Procedure

1. **Build daemon from HEAD on the user's box.**

   ```
   cd /home/projects/nocturne
   make clean && make
   ```

   Expected: `make` exits 0; `build/nocturned` and
   `build/nocturne-tagcheck` binaries produced; `make test` green.

2. **Install daemon binary + systemd user units.**

   ```
   sudo make install                    # or `make install PREFIX=$HOME/.local`
   systemctl --user daemon-reload
   systemctl --user enable --now nocturned-watch.service
   systemctl --user enable --now nocturned-rotate.timer
   systemctl --user is-active nocturned-watch.service          # → active
   systemctl --user is-active nocturned-rotate.timer           # → active
   ```

   Expected: both units active; `journalctl --user -u nocturned-watch -n 50`
   shows the daemon parsing config + opening the DB without errors.

3. **Configure `~/.config/nocturne/nocturne.toml`** per `docs/install.md`.
   At minimum: `[library].path`, `[cap].bytes`, `[sync_meta].path`,
   `[syncthing].desktop_device_id`, `[syncthing].phone_device_id`.

   ```
   nocturned doctor --json
   ```

   Expected: exit 0 (clean); JSON contains a populated `library_root`,
   sane `mount_free_bytes`, and `lock_held=1` (the watch service is
   holding the lock — that's the success signal that single-writer
   discipline is intact across processes).

4. **Run a full cycle once (scan → ingest → resolve → rotate → publish).**

   ```
   nocturned cycle
   ```

   Expected: exits 0; `manifest.json` and `catalog.json` present in the
   sync-meta folder; `<library_root>/resident/` populated with hardlinks
   to the resolved set; `<library_root>/archive/` holds everything else.

5. **Build the phone APK on the desktop.**

   ```
   cd /home/projects/nocturne/phone
   ./gradlew :app:assembleRelease --quiet
   ls -la app/build/outputs/apk/release/
   ```

   Expected: `nocturne-phone-release.apk` exists. (Per SCOPE AMENDMENT,
   cross-machine byte-identical reproducibility is NOT verified here;
   only that the build runs unmodified on the user's box.)

6. **Sideload the APK onto the phone.**

   Two paths — pick whichever the user already uses:

   - **Obtainium path:** push the APK to a personal git release (private
     repo over Syncthing or a GitHub release; `gh release create v1.0
     app/build/outputs/apk/release/nocturne-phone-release.apk` if using
     GitHub). In Obtainium on the phone: Add App → URL pointing at the
     release; Obtainium pulls the APK and installs it.

   - **`adb install` path:** `adb install -r phone/app/build/outputs/apk/release/nocturne-phone-release.apk`

   Expected: install succeeds; the app appears in the launcher.

7. **First-run flow on the phone.**

   1. Tap the launcher icon. Expected: the app opens to the SAF folder
      picker (Phase 4 first-run flow).
   2. Select the Syncthing `sync-meta` folder. Expected: ImportProgressScreen
      progresses to "Catalog imported"; the app navigates into the
      catalog browser.
   3. Tap a track to confirm playback (Phase 5 acceptance ratifies this
      formally; here it's a smoke test).

   Expected: the catalog imported, residents are visually distinguishable,
   at least one track plays.

8. **End-to-end pin-to-rotate smoke.** This isn't TUNE-01 — it's a
   single-cycle confirmation that the loop closes on real hardware
   before the 7-day window opens.

   1. On the phone, find a non-resident track (dimmed in the browser).
   2. Tap its PinChip. Wait ≤5 minutes (per SYNC-06 timing).
   3. On the desktop, run `nocturned cycle`.
   4. Confirm the file is now under `<library_root>/resident/` and the
      phone's Syncthing-Fork "Out of Sync" count is 0.

### Expected Behaviour

- Steps 1-4: zero modifications to source needed; daemon parses the
  user's real `nocturne.toml`; `nocturned doctor` is clean.
- Steps 5-7: phone APK builds, installs, opens to first-run picker,
  imports the desktop catalog without errors.
- Step 8: pinned track propagates desktop ↔ phone in ≤5 minutes.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| `make` fails on missing `taglib_c.h` | TagLib C bindings not on system | `pacman -S taglib`; verify `pkg-config --cflags taglib_c` resolves |
| `nocturned doctor` reports `inotify_headroom < 100` | `fs.inotify.max_user_watches` too low | `sudo sysctl fs.inotify.max_user_watches=524288` and persist in `/etc/sysctl.d/` |
| `nocturned cycle` exits non-zero with "Syncthing config not loaded" | Syncthing not running or config.xml unreadable | `systemctl --user start syncthing.service`; verify `~/.local/state/syncthing/config.xml` is readable |
| `:app:assembleRelease` fails on signing key missing | No release keystore configured | Per SCOPE AMENDMENT, debug APK is also acceptable for personal use; `assembleDebug` produces an unsigned-but-installable APK |
| Obtainium reports "no APK found at URL" | Release URL not pointing at a `.apk` direct download | Use the asset's "browser_download_url" from `gh release view --json assets`; or fall back to `adb install` |
| Phone first-run picker doesn't appear | App previously installed; DataStore already has metaTreeUri | `adb shell pm clear io.nocturne.phone` then relaunch |
| Pinned file does not appear under `resident/` after `nocturned cycle` | Pin event not yet ingested from `pins-phone-*.jsonl` | `nocturned ingest --dry-run` to confirm the file is reachable; then re-run `nocturned cycle` |

---

## TUNE-02 — Disk-margin sanity gate (`nocturned diskcheck`)

**Requirement:** Phone disk has ≥1 GiB free margin against the
user-configured `cap_bytes` AND against Syncthing's `minHomeDiskFree`
floor throughout normal operation.

Plan 08-03 ships the read-only probe `nocturned diskcheck` that answers
this question at any moment via statvfs(2) on `<library_root>` plus a
loopback GET against Syncthing's `/rest/config/options`. This section
confirms the probe behaves on real hardware and (optionally) wires it
into the 7-day observation window.

### Setup

1. CROSS-04 procedure complete; `nocturned cycle` has run at least once
   so the resident set is populated.
2. Syncthing is running locally; `nocturned doctor` shows
   `lock_held` is healthy.

### Procedure

1. **One-shot probe.**

   ```
   nocturned diskcheck
   ```

   Expected: text report with both margins displayed; exit 0 if both are
   ≥1.0 GiB. Exit 1 means the cap math is too tight for the volume —
   lower `[cap].bytes` in `nocturne.toml` and rerun. Exit 2 means
   Syncthing is unreachable; restart Syncthing and rerun.

2. **JSON probe (machine-readable).**

   ```
   nocturned diskcheck --json | jq
   ```

   Expected: single-line JSON with `cap_safe: true` AND `floor_safe: true`
   AND `degraded: false`. Note `min_margin_required_bytes` is exactly
   1073741824 (1 GiB binary).

3. **(Optional) Cron / systemd timer.** If the user wants
   continuous TUNE-02 enforcement during the observation window, drop
   a `~/.config/systemd/user/nocturned-diskcheck.{service,timer}` pair
   that runs `nocturned diskcheck` every hour and writes failures to
   a log. Sample units (informational; not required for v1):

   ```
   # nocturned-diskcheck.service
   [Service]
   Type=oneshot
   ExecStart=%h/.local/bin/nocturned diskcheck

   # nocturned-diskcheck.timer
   [Timer]
   OnUnitActiveSec=1h
   Persistent=true

   [Install]
   WantedBy=timers.target
   ```

   Document any failures (exit code 1) in the rubric's "Disk-margin
   failures observed" row.

### Expected Behaviour

- Probe exits 0 with both `cap_safe` and `floor_safe` reported true on
  a freshly-populated resident set whose `cap_bytes` is at least 1 GiB
  smaller than mount_avail.
- Text output cross-checks against `nocturned doctor`'s `mount_free_bytes`
  number — if they disagree by more than rounding noise, something
  is wrong.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| Exit 1, `cap_safe: false` | `cap_bytes` set too aggressively for the volume | Lower `[cap].bytes` in `nocturne.toml`; rerun `nocturned cycle` then `nocturned diskcheck` |
| Exit 1, `floor_safe: false` | Syncthing's `minHomeDiskFree` is too aggressive (e.g. set to 25%) | Adjust Syncthing GUI → Actions → Settings → "Minimum Free Disk Space"; rerun `diskcheck` |
| Exit 2, `degraded: true`, `syncthing_floor_bytes: null` | Syncthing not running OR config.xml unreadable | `systemctl --user status syncthing.service`; ensure `~/.local/state/syncthing/config.xml` exists and is readable |
| Exit 2, `mount_avail_bytes: null` | `[library].path` doesn't exist OR statvfs fails | `ls -la <library_root>`; verify the daemon runs as the same user that owns the path |

---

## TUNE-03 — Explainability sanity gate (`nocturned why <id>` + `resolve --dry-run --diff`)

**Requirement:** A "why is this track on my phone?" query (CLI for v1;
phone Compose sheet deferred to POLISH-08) explains for any resident
track which bucket(s) qualified it.

### Setup

1. CROSS-04 procedure complete; `nocturned cycle` populated the manifest.

### Procedure A — `nocturned why <id>` round-trip

1. **Pick three resident tracks.** From the manifest:

   ```
   jq -r '.resident[].id' < $SYNC_META/manifest.json | head -3
   ```

   Note the three sha256s.

2. **Query each.**

   ```
   nocturned why <full-sha-1>
   nocturned why <prefix-2>      # 8-char prefix
   nocturned why <full-sha-3> --json
   ```

   Expected: each call exits 0; text output reads `<sha-12>  <buckets-csv>`;
   JSON output is `{"id":"...","buckets":[...]}`. The bucket attribution
   must match what `jq '.resident[] | select(.id == "<sha>")' < manifest.json`
   reports.

3. **Negative case.** A made-up sha or one not in the manifest:

   ```
   nocturned why deadbeefdeadbeef
   echo "exit=$?"
   ```

   Expected: exit 1; stderr says "track-id not in manifest".

### Procedure B — `nocturned resolve --dry-run --diff` tuning workflow

1. **Idempotency check.** Without changing anything:

   ```
   nocturned resolve --dry-run --diff
   ```

   Expected: text output reads `NET: +0 tracks, +0 MiB` (the resolver
   is idempotent against the current `manifest_current` row).

2. **Tuning iteration.** Edit `~/.config/nocturne/nocturne.toml` —
   e.g. `buckets[recent_plays].count = 50` (raise from default).

   ```
   nocturned resolve --dry-run --diff
   ```

   Expected: ADDED / REMOVED / SHIFTED sections appear; `NET` line
   shows the delta. Revert the edit before continuing if you don't
   want the change applied — `--dry-run` does NOT write anything.

3. **Read-only contract sanity.** While `nocturned watch` is running:

   ```
   nocturned resolve --dry-run --diff
   ```

   Expected: stderr prints "another instance holds the writer lock"
   (skip-on-busy warning), but the diff still emits and the binary
   exits 0. The watch service keeps holding its lock — verify with:

   ```
   nocturned doctor | grep Lockfile     # held (pid=<watch pid>)
   ```

### Expected Behaviour

- Procedure A: each `nocturned why` lookup matches manifest.json content;
  negative case exits 1.
- Procedure B: idempotent dry-run prints `NET: +0`; an edit-then-diff
  round shows the expected delta; coexistence with watch works.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| `nocturned why` exits 1 with "manifest unreadable" | `<sync_meta>/manifest.json` not present | Run `nocturned publish` to regenerate; or pass `--manifest <path>` directly |
| `nocturned why` reports buckets that disagree with `manifest_current` | Resolver ran without follow-up publish (Pitfall 2 from RESEARCH) | Run `nocturned publish` to re-emit; the warning on stderr was a hint |
| `--dry-run --diff` fails to emit when watch is running | skip-on-busy stderr warning misread as fatal | Check exit code (must be 0); the warning is informational |
| `--dry-run --diff` returns non-zero exit on a config that currently rejects | `--diff` REQUIRES `--dry-run`; using it bare is rejected with NOCT_EXIT_USAGE | Add `--dry-run` to the command |

---

## TUNE-01 — 7-day observation window (rubric is the deliverable)

**Requirement:** After at least 7 days of real listening on the phone,
the resident set has shifted in a way the user finds reasonable.
Subjective; ratified at v1 review via the structured rubric.

The rubric file lives at
`.planning/phases/08-bucket-tuning-v1-milestone/08-RATIFICATION.md` and
starts empty (yes/no/notes columns blank). The user fills it after the
7-day window and ratifies v1 by either approving (sign-off line at the
bottom) or returning to the tuning loop.

Per RESEARCH Decision 7: the 72h-offline simulation (STATS-05 from Phase 6)
can be subsumed inside the 7-day TUNE-01 window. Plan a continuous 72h
block of WiFi-off listening inside the 7 days and the same observation
closes both gates.

### Setup

1. CROSS-04, TUNE-02, TUNE-03 all green (Procedures above).
2. `08-RATIFICATION.md` exists, columns blank.
3. The phone has at least 12 GiB of resident music; pin a few albums you
   actually want to listen to so day 1 isn't dominated by cold-start
   fallback.

### Procedure

1. **Day 0 — record baseline.**

   ```
   nocturned doctor --json > /tmp/nocturne-baseline-day0.json
   nocturned diskcheck --json > /tmp/nocturne-diskcheck-day0.json
   jq '.resident | length' < $SYNC_META/manifest.json     # baseline resident count
   ```

   Note these in the rubric's "Baseline" row.

2. **Days 1-3 — normal listening.** Listen as you normally would. Pin
   a few non-resident tracks. Let the rotate timer fire (default: every
   night). Plan a 72h continuous WiFi-off block inside the 7 days to
   close STATS-05 in the same window.

3. **Days 4-7 — keep listening.** After day 4, sample three "surprising"
   resident tracks (residents you did NOT expect to be on the phone)
   and three "missing" tracks (catalog tracks you listened to recently
   on the desktop or expected to be resident, but aren't).

   For each:

   ```
   nocturned why <sha>                          # for surprising
   # for missing — use catalog.json to find sha, then:
   nocturned why <sha>                          # exit 1 confirms it's not resident
   jq '.tracks[] | select(.sha256 == "<sha>")' < $SYNC_META/catalog.json
   # → confirms it exists in catalog but not manifest
   ```

   Record these in the rubric's "Surprising tracks" and "Missing tracks"
   tables.

4. **Day 7 — fill the rubric.**

   Open `08-RATIFICATION.md` in `$EDITOR`. Answer Q1..Q7 with target
   answers per the rubric. Record disk-margin observations from
   `nocturned diskcheck` runs over the window. Mark the rotation cycle
   count where `used_bytes > cap_bytes` (Q5 — must be 0 for v1).

5. **Decision point.**

   - If all "blocking" rubric rows are green AND the user is satisfied
     (Q7 is "yes"): sign off the bottom of `08-RATIFICATION.md`. v1
     ships.
   - If a blocking row is red: do NOT ship v1. Return to the tuning
     loop via `nocturned resolve --dry-run --diff`, edit
     `nocturne.toml`, run `nocturned cycle`, observe for another 3
     days, refill the rubric.
   - If only advisory rows are red and the user wants more iteration:
     note the deferral in the rubric, capture the change in
     `nocturne.toml`, ship v1 anyway.

### Expected Behaviour

- 7-day window completes without a single `cap_safe: false` from the
  hourly `diskcheck` (if the user wired the timer).
- At least one rotation has happened during the window (the resident
  set is not byte-identical to the day-0 baseline).
- The rubric is fillable in well under an hour at the end of day 7.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| Resident set hasn't shifted at all over 7 days | Bucket weights too sticky or no plays / pins logged | `nocturned ingest --dry-run` to confirm phone JSONL reaching the desktop; check `pins-phone-*.jsonl` line count grew |
| `used_bytes > cap_bytes` on at least one rotation cycle | Resolver bug (RESEARCH says target = 0; v1-blocking) | File a P0 deferral; do NOT ship v1; investigate `resolver.c` budget-enforcement path |
| `diskcheck` reports `cap_safe: false` mid-window | Rotation added more than expected this cycle | Lower `[cap].bytes` OR investigate why the resolver picked > cap (should not happen with `cap_effective_ratio = 0.70`) |
| User cannot decide whether resident set is "reasonable" | Run another 3 days; the 7-day floor is a minimum, not a maximum | Refill the rubric; if still unclear after day 10, defer v1 to a follow-up window |
| Pinned track does not become resident over multiple cycles | Pin event lost OR LWW lost to an unpin | Inspect `pins-phone-*.jsonl` directly; rerun `nocturned ingest`; confirm Phase 7 LWW behaved |

---

## Hardware-Blocked Status (Phase 8)

| Requirement | Status | Notes |
|-------------|--------|-------|
| TUNE-01 | Code-complete; rubric pending user fill | The 7-day observation procedure is the only path to closure; deliverable is `08-RATIFICATION.md` filled in |
| TUNE-02 | Code-complete (hardware-blocked) | `nocturned diskcheck` ships in 08-03; sanity-gated above; continuous enforcement via systemd timer is optional |
| TUNE-03 | Code-complete (hardware-blocked) | `nocturned why <id>` (08-01) + `resolve --dry-run --diff` (08-02) sanity-gated above; phone-side Compose sheet (POLISH-08) deferred to v1.x per SCOPE AMENDMENT |
| CROSS-04 | Hardware-blocked | The install procedure above IS the test; closes when the user signs off `08-RATIFICATION.md` |
| CROSS-02 | De-scoped to v1.x (POLISH-11) | Cross-machine F-Droid reproducibility deferred per SCOPE AMENDMENT (2026-04-27); same-machine `repro-build-check.sh` retained as Phase 4 sanity gate |

See also: `phone/docs/phase-5-hardware-acceptance.md` and
`phone/docs/phase-6-hardware-acceptance.md` for the upstream player and
stats-writer procedures these v1 acceptance steps layer on top of.
