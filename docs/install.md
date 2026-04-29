# nocturne — desktop install + Syncthing wiring

This walkthrough takes a clean Arch + Hyprland workstation through to a
working `nocturned` daemon, a path-layout-migrated music library, and a
desktop-side Syncthing peer ready to sync to a phone.

Phone-side wiring is in [docs/phone-setup.md](phone-setup.md).
Symptom-driven recovery is in [docs/troubleshooting.md](troubleshooting.md).

## 1. Prerequisites

Arch Linux. Other distributions work; install commands below assume
`pacman`. The user is expected to know how to substitute equivalents
on Debian/Fedora/etc.

```sh
sudo pacman -S taglib sqlite jansson curl syncthing
sudo pacman -S strace        # for tests/test_no_network.sh runtime layer
sudo pacman -S bear          # optional, for `make compile-commands`
```

Verify versions:

```sh
syncthing --version              # expect 2.0.16 or newer
pkg-config --modversion libcurl  # expect 8.x
pkg-config --modversion taglib_c # expect 2.x
```

## 2. Build nocturned

```sh
cd /path/to/nocturne
make nocturned
make test
```

Expected: `==> All test suites PASSED`. If the CROSS-03 audit fails
with "libcurl linked", confirm you're on a tag at or after Phase 3
plan 03-07.

Optional sanitizer build for paranoia:

```sh
make clean
make test SAN=1
```

## 3. Configure

Create `~/.config/nocturne/nocturne.toml`:

```toml
[library]
path = "/home/<you>/music/library"

[sync_meta]
path = "/home/<you>/sync/nocturne/meta"

[cap]
bytes = 12884901888           # 12 GiB
effective_ratio = 0.70

[syncthing]
desktop_name = "nocturne-desktop"
phone_name   = "nocturne-phone"
# After running Syncthing once, paste in the device IDs:
# desktop_device_id = "ABCDEFG-..."
# phone_device_id   = "HIJKLMN-..."

[syncthing.phone]
sync_files_path = "/storage/emulated/0/Music/nocturne"
sync_meta_path  = "/storage/emulated/0/sync/nocturne/meta"
```

The [cap] block is the resident-set size cap. 12 GiB is the default
target; effective_ratio leaves 30% headroom on the phone's filesystem
to avoid Pitfall 10 (storage floor).

## 4. One-time library migrate (SYNC-01)

Phase 3 lays out your library as
`<root>/archive/<artist>/<album>/<track>` for non-resident files and
`<root>/resident/<artist>/<album>/<track>` for the rotating set.
Hardlinks make this free in disk space; the path layout IS the
selective-sync mechanism (see `.planning/research/PITFALLS.md`
Pitfall 2).

Step 1: scan to populate the DB so migrate has rows to enumerate.

```sh
nocturned scan ~/music/library
```

Expected output:

```
scan: seen=12345 added=12345 updated=0 removed=0 skipped=0 parse_failed=0 hash_failed=0 elapsed=18s
```

Step 2: dry-run the migrate to see what will move.

```sh
nocturned migrate ~/music/library
```

Expected output:

```
migrate: planned=12345 moved=0 already=0 skipped_outside=0 fallback=0 errors=0 (dry-run)
```

Step 3: execute.

```sh
nocturned migrate ~/music/library --apply
```

Expected output:

```
migrate: planned=12345 moved=12345 already=0 skipped_outside=0 fallback=0 errors=0
```

After this command, every track lives under
`~/music/library/archive/<artist>/<album>/<track>`. `~/music/library/resident/`
is empty until rotate runs.

Step 4: doctor sanity check.

```sh
nocturned doctor
```

Expected: `Schema version: 3`, `Issues found: 0 (healthy)`. If
`schema_version < 3`, the migration did not apply — check
`~/.local/share/nocturne/nocturne.db` permissions and rerun.

## 5. Wire Syncthing on desktop (SYNC-02, SYNC-04, SYNC-05)

Start Syncthing once so it generates its config:

```sh
systemctl --user enable --now syncthing
```

Open `https://127.0.0.1:8384` in a browser (accept the self-signed
cert). Set the Device Name to `nocturne-desktop`
(Settings → General → Device Name) — this is Pitfall 20 mitigation;
the default is your machine's hostname which leaks identity.

Disable the privacy-leaking knobs:

- Settings → Connections → uncheck "Global Discovery"
- Settings → Connections → uncheck "Enable Relaying"
- Settings → Connections → uncheck "Enable NAT traversal"

Copy your Syncthing device ID (top-right Identification panel) into
`nocturne.toml` as `[syncthing] desktop_device_id`.

Generate the desktop-side folder XML:

```sh
nocturned sync-config --print --side desktop > /tmp/desktop-folders.xml
cat /tmp/desktop-folders.xml
```

The output is a complete `<configuration>` block with both folder
pairs declared. Two ways to apply it:

### Option A: REST PUT (no Syncthing restart needed)

```sh
nocturned sync-config --apply
```

Expected silently exits 0. Refresh the Syncthing GUI; the two folders
(`sync-meta`, `sync-files`) appear in the sidebar.

### Option B: hand-edit config.xml

Stop Syncthing, paste the relevant `<folder>` blocks into
`~/.config/syncthing/config.xml` after the existing folders, restart
Syncthing.

```sh
systemctl --user restart syncthing
```

## 6. First rotation

Compute the manifest:

```sh
nocturned resolve
```

Expected output:

```
resolve: residents=8412 used=12348902400 cap=12884901888 effective=9019431321 cold_start=yes
note: run `nocturned rotate` to apply the manifest to disk
```

Apply it:

```sh
nocturned rotate
```

Expected:

```
rotate: to_add=8412 added=8412 to_remove=0 removed=0 already=0 fallback=0 errors=0
```

If you see `warn: syncthing config.xml not loaded`, set
`NOCTURNE_SYNCTHING_CONFIG=$HOME/.config/syncthing` and retry; the
default location is correct unless you've moved Syncthing's config dir.

Inspect the result:

```sh
ls ~/music/library/resident/   # should NOT be empty
find ~/music/library/resident -type f -name '*.flac' | wc -l
```

## 7. Run as a service

Reference unit files live under `config/systemd/user/`. Five units cover
the full automation chain:

| Unit | Role |
|------|------|
| `nocturne-watch.service` | inotify watcher; sub-second DB updates on FS events |
| `nocturne-cycle.service` | one-shot scan→ingest→resolve→rotate→publish |
| `nocturne-cycle.timer` | fires cycle every 15 min |
| `nocturne-pin-cycle.service` | runs cycle when phone JSONL writes land |
| `nocturne-pin-cycle.path` | path watcher that triggers pin-cycle |

Install:

```sh
mkdir -p ~/.config/systemd/user
cp config/systemd/user/*.{service,timer,path} ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now \
    nocturne-watch.service \
    nocturne-cycle.timer \
    nocturne-pin-cycle.path
```

Verify:

```sh
systemctl --user status nocturne-watch.service
# active (running) — "library=/home/$USER/music dirs=N watches=N"
systemctl --user list-timers | grep nocturne
# nocturne-cycle.timer firing every 15 min
```

### Lock coordination

`nocturned` enforces single-writer access via a flock on its pidfile.
Watch holds it indefinitely; cycle / pin-cycle take it for the duration
of their run. The reference units coordinate via `ExecStartPre` (stop
watch) + `ExecStartPost` (restart watch). Handover takes ≈2 seconds of
lost inotify coverage per cycle tick, which is fine — `nocturned scan`
on next watch start picks up anything that changed during the gap.

### Latency budget

- Drop a song in `~/music/` → DB updated ≤2 s (watch).
- Catalog/manifest published ≤15 min (next cycle tick — tighten via
  `OnUnitActiveSec=` in the timer if you want faster).
- Phone Syncthing pulls + Nocturne app reconciles ≤45 s after publish.
- **End-to-end worst case: ≈16 min.**

## 8. Next: phone

[docs/phone-setup.md](phone-setup.md) walks through Syncthing-Fork on
F-Droid, device pairing, and the manual SYNC-06 (5-minute timing) and
SYNC-07 (Receive-Only tampering) verification procedures.

## Troubleshooting

If anything in this walkthrough errors out, check
[docs/troubleshooting.md](troubleshooting.md) — symptom-keyed recipes
for the common pitfalls.
