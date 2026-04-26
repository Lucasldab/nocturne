# nocturne — phone wiring + SYNC-06 / SYNC-07 hands-on verification

This doc is a **hands-on followup** to the desktop install
([docs/install.md](install.md)). Phase 3 ships desktop-complete; the
daemon is fully functional without performing this walkthrough — but
no music will reach the phone until a phone is paired.

This is also the formal verification procedure for SYNC-06 (cross-device
5-minute timing) and SYNC-07 (Receive-Only tampering test). Both
requirements are recorded in `.planning/STATE.md` as **deferred until
the user executes this document**.

The walkthrough assumes a GrapheneOS phone — most steps work on stock
Android with minor naming differences in the F-Droid UI.

## 1. Why Syncthing-Fork (not Syncthing-Android)

Per [`.planning/research/STACK.md`][stack]: the official Syncthing-Android
was discontinued after Dec 2024. **Syncthing-Fork** by Catfriend1 is
the active community fork on F-Droid, version 2.0.16.0 as of 2026-04-09.
Wire-compatible with desktop Syncthing 2.0.16.

[stack]: ../.planning/research/STACK.md

Listing: <https://f-droid.org/en/packages/com.github.catfriend1.syncthingfork/>

## 2. Install on phone

1. If F-Droid not installed, get it from <https://f-droid.org/>.
   (Droid-ify or Neo Store also work.)
2. In F-Droid, search "Syncthing-Fork" by Catfriend1. Install. Open.
3. Accept first-run prompts. Grant the storage permission when
   prompted (it's needed for the music folder).

## 3. Initial Syncthing-Fork config

In Syncthing-Fork:

### Device name (Pitfall 20 mitigation)

Settings → General → Device name → set to `nocturne-phone`. Do NOT
leave the default; on most phones the default is the make+model which
identifies the hardware over the LAN.

### Disable global discovery + relays (SYNC-05)

Settings → Syncthing options:

- "Global Discovery" → OFF
- "Use Relays" → OFF
- "Local Discovery" → ON (leave on; LAN-only is fine)
- "NAT traversal" → OFF

### WiFi-only sync per folder (Pitfall 25)

Settings → Per-folder Power Conditions → "Run on WiFi only" → enable
**for the sync-files folder specifically** (we'll create it in step 5).
Mobile-data sync of the music folder would burn through phone data
silently; the desktop XML cannot encode this knob, so it's enforced
phone-side.

For sync-meta you can leave WiFi-only off — JSONL deltas are tiny.

## 4. Pair the devices

On phone, Syncthing-Fork → Devices tab → tap the QR-code icon
(top right) to display the phone's Device ID.

On desktop, open `https://127.0.0.1:8384`:

1. "Add Remote Device".
2. Paste the phone's Device ID.
3. Set Name to `nocturne-phone`.
4. Save.

Within 30 seconds (on the same WiFi), the phone gets a "New device"
prompt. Accept; set the desktop's name to `nocturne-desktop`. Save.

The two devices now know about each other but share no folders yet.

Copy the phone's Device ID into your desktop `nocturne.toml`:

```toml
[syncthing]
phone_device_id = "PASTED-FROM-PHONE-7CHARS-MORE..."
```

(Re-running `nocturned sync-config --apply` is now meaningful: the XML
the daemon emits will reference the right phone ID.)

## 5. Add the two folders on phone

The cleanest path uses **Option A (recommended)** — share the desktop
folders to the phone, which prompts the phone-side accept dialog.

### Option A — Share from desktop (recommended)

On the desktop Syncthing GUI:

1. Click the `sync-meta` folder → Edit → Sharing tab.
2. Check `nocturne-phone` to share. Save.
3. Repeat for `sync-files`.

On phone, Syncthing-Fork shows two new "Folder shared by
nocturne-desktop" prompts. For each:

- **sync-meta**: Accept. Path: `/storage/emulated/0/sync/nocturne/meta`
  (or whatever `[syncthing.phone] sync_meta_path` says in the desktop
  config). **Type: Send & Receive.** Versioning: **None**.
- **sync-files**: Accept. Path:
  `/storage/emulated/0/Music/nocturne` (or whatever
  `[syncthing.phone] sync_files_path` says). **Type: Receive Only.**
  Versioning: **None**. (Pitfall 24 — DO NOT pick "Trash Can" or
  "Simple" versioning here; they double phone storage cost.)

### Option B — Manual on phone

If Option A doesn't surface a prompt (sometimes happens on first pair
when discovery hasn't finished), add each folder manually on the
phone:

1. Run on desktop: `nocturned sync-config --print --side phone > /tmp/phone-folders.xml`.
2. Open `/tmp/phone-folders.xml` and read the path / type fields.
3. On phone, Syncthing-Fork → Folders tab → "+" → fill the same
   Folder ID, path, and type. Versioning: None.

### Initial sync

After both folders are added, the phone shows "Out of Sync" briefly,
then "Up to Date". The first sync of `~/music/library/resident/` can
be hours on a 12 GB initial set — see [troubleshooting.md] Pitfall 23.

[troubleshooting.md]: troubleshooting.md

## 6. SYNC-07 verification — Receive-Only tampering test

Goal: prove that any local modification to `sync-files` on the phone
is reverted by Syncthing on the next sync, because the folder is
declared Receive Only.

### Steps

1. On the phone, navigate to
   `/storage/emulated/0/Music/nocturne/<some-artist>/<some-album>/<some-track>.flac`
   using a file manager (Material Files / Simple File Manager / etc).
2. Open the track in a tag editor app — e.g. **Music Tag Editor**
   (free on F-Droid) or Auxio's built-in editor.
3. Change the title from "X" to "X-modified". Save.
4. Open Syncthing-Fork → sync-files folder. Within ~30 seconds it
   shows **"Local Additions"** with the modified file listed.
5. Tap "Rescan" (or wait for the next interval).
6. Observe: the file reverts to the desktop version. The "Local
   Additions" warning clears.

### Pass criterion

The modified file's content is restored on the phone WITHOUT manual
intervention. The "Local Additions" state must clear (NOT escalate to
"Sync Conflict").

### Record

Once observed, tick SYNC-07 in `.planning/STATE.md`:

```diff
-| Phase 3 | SYNC-07 verification — receive-only tampering test | Open | 2026-04-26 (Phase 3 close) |
+| Phase 3 | SYNC-07 verification — receive-only tampering test | Closed (verified <date>) |
```

If the file does NOT revert: see troubleshooting.md "Modifying the
audio file on phone DOESN'T revert (SYNC-07 fails)" — almost certainly
folder type is Send & Receive, not Receive Only (Pitfall 9).

## 7. SYNC-06 verification — 5-minute cross-device timing

Goal: prove that a track newly added to the manifest on the desktop
appears on the phone within 5 minutes on home WiFi.

### Steps

1. On desktop, force the manifest to include a specific track that
   isn't currently resident. Easiest: pin one explicitly.

   Edit `~/.local/state/nocturne/nocturne.db` directly (until Phase 7
   ingest lands a phone-side pin UI):

   ```sh
   sqlite3 ~/.local/state/nocturne/nocturne.db <<EOF
   INSERT OR REPLACE INTO pins (unit, id, pinned, updated_at)
   VALUES ('track', 'PASTE-SHA256-OF-A-NON-RESIDENT-TRACK',
           1, '2026-04-26T20:00:00Z');
   EOF
   ```

   Find a sha256 with:

   ```sh
   sqlite3 ~/.local/state/nocturne/nocturne.db \
       "SELECT sha256, path FROM tracks WHERE path LIKE '%/archive/%' LIMIT 5"
   ```

2. Note the desktop time. Run:

   ```sh
   nocturned resolve && nocturned rotate
   ```

3. On phone, watch Syncthing-Fork's sync-files folder. Within 5
   minutes (typical: 30 seconds on home WiFi) the new track appears at
   `/storage/emulated/0/Music/nocturne/<artist>/<album>/<track>.flac`.

4. Reverse direction: remove the pin, run resolve+rotate again,
   observe the file vanish from the phone within 5 minutes.

   ```sh
   sqlite3 ~/.local/state/nocturne/nocturne.db \
       "UPDATE pins SET pinned=0, updated_at='2026-04-26T20:30:00Z' WHERE id='PASTE-SHA256...'"
   nocturned resolve && nocturned rotate
   ```

### Pass criterion

Both the addition and the removal complete within 5 minutes.

### Record

Tick SYNC-06 in `.planning/STATE.md` once observed.

## 8. Troubleshooting

If any step here misfires, check [troubleshooting.md] — entries are
symptom-keyed for ctrl-F.

## Continuing operation

The phone needs no further attention day-to-day. The desktop's nightly
`nocturned-rotate.timer` (set up in install.md §7) refreshes the
manifest, the rotate engine moves files between archive/resident, the
REST POST nudges Syncthing to scan, the phone receives changes the
next time it's on WiFi.

When you observe drift (phone storage filling, "Out of Sync" stuck,
sync-conflict files), the [troubleshooting.md] symptom list is your
first stop.
