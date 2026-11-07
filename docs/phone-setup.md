# nocturne — phone setup

This follows the [server install](install.md). The daemon runs without a phone,
but no music reaches one until these steps are done.

It assumes GrapheneOS. Other Android versions should work with small naming
differences, but only GrapheneOS gets regular use.

## 1. Install the apps

Install both through [Obtainium](https://github.com/ImranR98/Obtainium), adding
each GitHub repository as an app:

| App | Repository |
|-----|------------|
| Syncthing-Fork | <https://github.com/researchxxl/syncthing-android> |
| nocturne | <https://github.com/Lucasldab/nocturne> |

Syncthing-Fork is the maintained continuation of the discontinued official
Syncthing Android app. It's also listed on
[F-Droid](https://f-droid.org/en/packages/com.github.catfriend1.syncthingfork/).

Open Syncthing-Fork, accept the first-run prompts and grant storage access. Then
set **battery usage to Unrestricted** for both apps in Android's app settings, or
syncing stops while the screen is off.

## 2. Configure Syncthing-Fork

**Device name:** Settings → General → Device name → `nocturne-phone`. The default
is usually the make and model, which identifies the hardware on the LAN.

**Discovery:** Settings → Syncthing options:

- Global Discovery: off
- Use Relays: off
- NAT traversal: off
- Local Discovery: on

With these settings the devices only find each other on the same LAN, or over a
VPN if you give each device the other's address.

**Wi-Fi only:** Settings → Per-folder Power Conditions → enable "Run on WiFi only"
for `sync-files` once you've added it in step 4. The server can't set this, and
syncing music over mobile data uses a lot of data. `sync-meta` can stay
unrestricted, because its files are tiny.

## 3. Pair the devices

1. On the phone: Syncthing-Fork → Devices → QR-code icon (top right) shows the
   Device ID.
2. On the server: open the GUI at `https://127.0.0.1:8384`, choose **Add Remote
   Device**, paste the ID, set the name to `nocturne-phone` and save.
3. On the same Wi-Fi, the phone shows a "New device" prompt within about 30
   seconds. Accept it and name the device `nocturne-server`.

Add the phone's ID to `~/.config/nocturne/config.toml` on the server:

```toml
[syncthing]
phone_device_id = "PASTE-THE-PHONE-ID"
```

Then re-run `nocturned sync-config --apply` so the folders reference that device.

## 4. Add the two folders

In the server's Syncthing GUI, share `sync-meta` and `sync-files` with
`nocturne-phone` (folder → Edit → Sharing). The phone then prompts for each one:

| Folder | Path on the phone | Type | Versioning |
|--------|-------------------|------|------------|
| `sync-meta` | `/storage/emulated/0/nocturne/meta` | Send & Receive | None |
| `sync-files` | `/storage/emulated/0/nocturne/music` | **Receive Only** | **None** |

The paths should match `[syncthing.phone]` in your config. Keep versioning off on
`sync-files`, because versioned copies of rotated tracks would pile up in
`.stversions` and use the storage the cap is meant to protect.

If no prompt appears, add the folders by hand with the same IDs, paths and types.
`nocturned sync-config --print --side phone` prints the values.

The first sync of a 12 GB set can take hours.

## 5. Set up the app

Open nocturne and pick the two folders when asked: first the metadata folder
(`nocturne/meta`), then the music folder (`nocturne/music`). The app builds its
index and shows the catalog.

## 6. Check that it works

**Pinning:** pin a track that isn't on the phone. With both devices online it
should arrive in about a minute. Unpin it and it should leave again after the next
sync.

**Folder protection:** change a file in `nocturne/music` on the phone. Syncthing
should list it under locally changed items on that folder. **Revert** restores the
server's copy. Syncthing doesn't revert it by itself, and the server's copy is
never affected.

## Day to day

The phone needs no further attention. The cycle timer on the server refreshes the
manifest, rotate moves files, and the phone receives the changes the next time it
can reach the server.

If the phone's storage fills up, or a folder stays out of sync, see
[troubleshooting](troubleshooting.md).
