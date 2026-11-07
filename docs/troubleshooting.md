# nocturne — troubleshooting

Organized by symptom. Search for what you're seeing; each entry has a **Cause**
and a **Fix**.

Paths use the examples from the [install guide](install.md): the library at
`~/music` and the meta folder at `~/sync/nocturne/meta`.

---

## Symptom: phone storage fills up despite the manifest cap

**Cause:** the `sync-files` folder has versioning enabled on the phone (Trash Can,
Staggered, Simple or External). Each file rotate removes is kept in `.stversions/`.

**Fix:**

1. Phone: Syncthing-Fork → `sync-files` → Edit → Versioning → **None**. Save.
2. Delete `.stversions/` from the folder (`/storage/emulated/0/nocturne/music`).
3. In the server's Syncthing GUI, check that `sync-files` also has versioning off.

The config from `nocturned sync-config --print` specifies
`<versioning type="none"/>`, but a folder added by hand on the phone may have
different defaults.

---

## Symptom: `nocturned rotate` succeeds but tracks don't appear on the phone

**Cause:** Syncthing on the phone hasn't synced since the rotation, or the phone
can't reach the server. Pins and deletes made while the phone is away only land
when it reconnects.

**Fix:**

1. In the server's Syncthing GUI, check whether `nocturne-phone` is connected and
   up to date.
2. Phone: Syncthing-Fork → `sync-files` → **Rescan**.
3. If `sync-files` is set to Wi-Fi only, confirm the phone is on Wi-Fi.
4. If `rotate` printed a warning about the rescan POST, see the entry for that
   below.

---

## Symptom: sync-conflict files (`*.sync-conflict-*`) appear in the meta folder

**Cause:** the server and the phone wrote the same path between syncs. For
`sync-files` this shouldn't happen (Send Only on the server, Receive Only on the
phone). The phone's event files are named per device to prevent it in `sync-meta`.

**Fix:**

1. Find them:
   ```sh
   find ~/sync/nocturne/meta -name '*.sync-conflict-*' -print
   ```
2. Inspect each one and decide which copy is correct.
3. Merge by hand if needed, then `rm` the conflict copies.

Syncthing-Fork's conflict list is cached, so restart the app if an entry stays
after you delete the file.

---

## Symptom: `nocturned migrate --apply` reports `errors=N` for some tracks

**Cause:** a per-track filesystem error, usually `EACCES` (another user owns the
files) or an unusual path. `EXDEV` (library across filesystems) is handled by a
copy fallback and isn't a real error: it's counted in `fallback=N`.

**Fix:**

1. Read the stderr from the run. Each error line names the track and what failed.
2. For `EACCES`, check you're running as the owner of the library:
   ```sh
   stat ~/music/some.flac
   ```
3. Fix the cause and rerun `nocturned migrate ~/music --apply`. Migrate is
   idempotent, so tracks already moved are skipped.

---

## Symptom: phone's `sync-files` stays "Out of Sync"

There are three common causes, so check them in this order.

**Case-only name differences.** Android shared storage is case-insensitive, so the
phone can't hold both `ERRA/` and `Erra/`, and Syncthing blocks. Find them on the
server:

```sh
cd ~/music/resident && find . -type d | sort -f | uniq -Di
```

Rename the directories in your library so the spelling matches.

**Not enough free space.** Syncthing's minimum free disk check stops the sync.
Free some space on the phone, or lower the cap in `~/.config/nocturne/config.toml`:

```toml
[cap]
bytes = 8589934592    # 8 GiB instead of 12 GiB
```

Then run `nocturned resolve && nocturned rotate` to evict the bottom of the set.

**A stuck `.nomedia`.** Android's storage layer treats `.nomedia` specially, so the
phone's copy may never match the server's, with no error logged. Add `.nomedia` to
the ignore patterns of `sync-files` **on the phone**.

---

## Symptom: `nocturned rotate` warns "rotate will skip the rescan POST" or "rescan POST returned HTTP"

**Cause:** Syncthing isn't running, or the daemon can't read its config.

**Fix:**

1. Confirm Syncthing is up:
   ```sh
   systemctl --user status syncthing
   ```
   If it's inactive, run `systemctl --user start syncthing`.
2. Confirm its config exists and has an API key:
   ```sh
   grep -oP '<apikey>\K[^<]+' ~/.config/syncthing/config.xml
   ```
3. If the config lives somewhere else, point the daemon at it:
   ```sh
   export NOCTURNE_SYNCTHING_CONFIG="$HOME/somewhere-else/syncthing"
   nocturned rotate
   ```

The files already moved, so rotate itself succeeded. Without the rescan request,
Syncthing picks the changes up at its next scan.

---

## Symptom: Syncthing shows a hostname or phone model instead of `nocturne-phone`

**Cause:** the device name was never set.

**Fix:** phone: Syncthing-Fork → Settings → General → Device name →
`nocturne-phone`. On the server, set it under Settings → General in the GUI.

---

## Symptom: a file changed on the phone isn't restored

**Cause:** Syncthing doesn't revert a Receive Only folder by itself. Changed files
stay listed as locally changed until you act. If nothing is listed at all, the
folder type is probably wrong.

**Fix:**

1. Phone: Syncthing-Fork → `sync-files` → Edit → Folder type → **Receive Only**.
2. Server: Syncthing GUI → `sync-files` → Edit → Folder type → **Send Only**.
3. Rescan on the phone, then tap **Revert** on the folder.

---

## Symptom: `tests/test_no_network.sh` fails

**Cause:** something in the daemon gained a network path beyond loopback. The audit
allows libcurl only for talking to Syncthing on `127.0.0.1`.

**Fix:** the output names the failing layer (`ldd`, `nm`, source search, `strace`
or `strings`). Any non-loopback `connect()` in the `strace` layer is a real
regression. Network work belongs in a separate program, not in `nocturned`.

---

## Symptom: `.stignore` evaluation is slow on `sync-files`

**Cause:** someone added patterns to `~/music/resident/.stignore`. It should be
empty, because the `archive/` and `resident/` layout already selects what syncs.

**Fix:**

```sh
wc -l ~/music/resident/.stignore   # expect 0
: > ~/music/resident/.stignore
```

Then restart Syncthing or wait for the next scan.

---

## Symptom: `sync-files` syncs over mobile data

**Cause:** Wi-Fi only is a **per-folder** setting in Syncthing-Fork, and the global
toggle doesn't cover it.

**Fix:**

1. Phone: Syncthing-Fork → Settings → Per-folder Power Conditions.
2. Enable "Run on WiFi only" for `sync-files`.
3. Turn Wi-Fi off. `sync-files` should pause within about 30 seconds.

---

## Symptom: a command fails with a "lock busy" error, or does nothing

**Cause:** another `nocturned` holds the single-writer lock, usually
`nocturned watch` under systemd.

**Fix:**

1. See who's holding it:
   ```sh
   ps -p "$(cat ~/.cache/nocturne/nocturned.pid)"
   ```
2. If it's the watcher, stop it, run your command, then start it again:
   ```sh
   systemctl --user stop nocturne-watch
   nocturned migrate ~/music --apply
   systemctl --user start nocturne-watch
   ```

`nocturned doctor` doesn't take the lock, so it works while the watcher runs.

---

## Symptom: `nocturned doctor` reports an old schema version

**Cause:** a migration didn't run, usually because the daemon crashed during one.
The latest version is the highest number in `schema/`.

**Fix:**

1. Check the version:
   ```sh
   sqlite3 ~/.local/share/nocturne/nocturne.db 'PRAGMA user_version'
   ```
2. The next `nocturned` command should apply pending migrations. If it doesn't,
   look for error messages in its stderr.
3. As a last resort, back up the database and rebuild it. **This loses pins, likes
   and play history that can't be replayed from the meta folder.**
   ```sh
   cp ~/.local/share/nocturne/nocturne.db ~/.local/share/nocturne/nocturne.db.backup
   rm ~/.local/share/nocturne/nocturne.db
   nocturned scan ~/music
   ```

---

## Symptom: `make test-integration-rotate` skips with "syncthing not in PATH"

**Cause:** the test spawns its own Syncthing in a temporary directory, and exits
with the skip code 77 if there's no `syncthing` binary.

**Fix:** install Syncthing and `curl`, then run it again.

---

## Where to look when nothing here matches

1. `nocturned doctor --json` gives a full health report.
2. `nocturned why <track-id>` explains why a track is on the phone.
3. Open `~/.local/share/nocturne/nocturne.db` with `sqlite3` and inspect `tracks`,
   `residency_state`, `manifest_current` and `manifest_meta`.
4. Check the Syncthing logs in the GUI (`https://127.0.0.1:8384`, under Logs).
5. Check `dmesg` if you suspect a filesystem problem such as corruption or `ENOSPC`.
