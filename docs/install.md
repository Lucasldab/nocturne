# nocturne — server install

This takes a Linux machine from a clean checkout to a running `nocturned`, a
migrated music library and a Syncthing peer ready to pair with a phone.

Next steps: [phone setup](phone-setup.md). If something goes wrong, see
[troubleshooting](troubleshooting.md).

The examples use `~/music` for the library and `~/sync/nocturne/meta` for the meta
folder, which are the paths the shipped systemd units expect.

## 1. Prerequisites

nocturned needs **TagLib 2.x**. It doesn't link against 1.13, the version in
Ubuntu 24.04.

Debian 13 (trixie):

```sh
sudo apt install build-essential pkg-config xxd libtag-c-dev libsqlite3-dev \
    libjansson-dev libcurl4-openssl-dev syncthing python3 ffmpeg
sudo apt install curl flac jq sqlite3 strace   # for make test
```

Arch:

```sh
sudo pacman -S base-devel tinyxxd taglib sqlite jansson curl syncthing python ffmpeg
sudo pacman -S flac jq strace                  # for make test
sudo pacman -S bear                            # optional, for make compile-commands
```

`ffmpeg` is required for [transcode mode](transcode.md), and the test suite uses it
to generate fixtures.

## 2. Build and install

```sh
cd /path/to/nocturne
make nocturned
make test
sudo install -m 0755 build/nocturned /usr/local/bin/nocturned
```

`make test` should end with `==> All test suites PASSED`. For a sanitizer build, run
`make clean`, then `make test SAN=1`.

## 3. Configure

Create `~/.config/nocturne/config.toml`, replacing `<you>` with your username:

```toml
[library]
path = "/home/<you>/music"

[sync_meta]
path = "/home/<you>/sync/nocturne/meta"

[cap]
bytes = 12884901888           # 12 GiB
effective_ratio = 0.70

[syncthing]
desktop_name = "nocturne-server"
phone_name   = "nocturne-phone"
# After running Syncthing once, paste in the device IDs:
# desktop_device_id = "ABCDEFG-..."
# phone_device_id   = "HIJKLMN-..."

[syncthing.phone]
sync_files_path = "/storage/emulated/0/nocturne/music"
sync_meta_path  = "/storage/emulated/0/nocturne/meta"
```

`[cap]` limits how much music goes to the phone. With `effective_ratio = 0.70`, the
resolver stays under 70% of `bytes` to leave headroom on the phone's filesystem.

Every option is described in
[`config/nocturne.toml.example`](../config/nocturne.toml.example).

Also create the meta folder:

```sh
mkdir -p ~/sync/nocturne/meta
```

## 4. Migrate the library (once)

nocturne keeps non-resident files in `<library>/archive/` and the rotating set in
`<library>/resident/`. Only `resident/` is synced, so the directory layout itself
selects what reaches the phone. Moves use hardlinks, so they take no extra space.

Scan to fill the database:

```sh
nocturned scan ~/music
# scan: seen=12345 added=12345 updated=0 removed=0 skipped=0 parse_failed=0 hash_failed=0 elapsed=18s
```

Do a dry run to see what will move:

```sh
nocturned migrate ~/music
# migrate: planned=12345 moved=0 already=0 skipped_outside=0 fallback=0 errors=0 (dry-run)
```

Then apply it:

```sh
nocturned migrate ~/music --apply
# migrate: planned=12345 moved=12345 already=0 skipped_outside=0 fallback=0 errors=0
```

Every track now lives under `~/music/archive/`. `resident/` stays empty until the
first rotation.

Check the result with `nocturned doctor`. `Orphan rows` should be 0, and
`Issues found` shows anything that needs attention.

## 5. Set up Syncthing

Start Syncthing once so it generates its config:

```sh
systemctl --user enable --now syncthing
```

Open `https://127.0.0.1:8384` and accept the self-signed certificate. Set the
device name to `nocturne-server` under Settings → General, because the default is
your hostname.

Turn off the settings that reach outside your network:

- Settings → Connections → uncheck "Global Discovery"
- Settings → Connections → uncheck "Enable Relaying"
- Settings → Connections → uncheck "Enable NAT traversal"

Copy your device ID (Actions → Show ID) into `config.toml` as
`[syncthing] desktop_device_id`.

Check the generated folder config, then apply it. `desktop` is the code's name for
the server side:

```sh
nocturned sync-config --print --side desktop
nocturned sync-config --apply
```

Refresh the GUI: `sync-meta` and `sync-files` should appear in the sidebar. If you'd
rather edit by hand, stop Syncthing, paste the `<folder>` blocks into
`~/.config/syncthing/config.xml` and start it again.

## 6. First rotation

Compute which tracks belong on the phone, apply that, and publish the result:

```sh
nocturned resolve
# resolve: residents=840 used=9000000000 cap=12884901888 effective=9019431321 cold_start=yes
nocturned rotate
# rotate: to_add=840 added=840 to_remove=0 removed=0 already=0 ... errors=0
nocturned publish
# publish: out=/home/<you>/sync/nocturne/meta
```

`cold_start=yes` is expected at this point. Until the phone has logged 10 plays, the
resolver only uses recent adds, exploration and pins, so weekly discovery picks
don't appear on the phone yet.

On the very first rotate you may see an `ensure_resident_nomedia` error, because
`resident/` doesn't exist yet. The rotation still completes.

If you see `warn: syncthing config.xml not loaded`, set
`NOCTURNE_SYNCTHING_CONFIG` to your Syncthing config directory and retry. The
default, `~/.config/syncthing`, is correct unless you moved it.

To confirm files arrived:

```sh
find ~/music/resident -type f | wc -l
ls ~/sync/nocturne/meta      # catalog.json  manifest.json
```

## 7. Run as a service

The units are in `config/systemd/user/`, and
[their README](../config/systemd/user/README.md) describes each one.

```sh
mkdir -p ~/.config/systemd/user ~/.local/bin
install -m 0644 config/systemd/user/*.{service,timer,path} ~/.config/systemd/user/
install -m 0755 config/bin/nocturne-cycle-run config/bin/nocturne-pin-cycle-runner ~/.local/bin/
systemctl --user daemon-reload
systemctl --user enable --now \
    nocturne-watch.service \
    nocturne-cycle.timer \
    nocturne-discover.timer \
    nocturne-pin-cycle.path
```

Check that they're running:

```sh
systemctl --user status nocturne-watch.service
systemctl --user list-timers | grep nocturne
```

To keep the services running while you're logged out, run
`sudo loginctl enable-linger $USER`.

Timings to expect:

- A new song in `~/music/` reaches the database within about 2 seconds.
- The catalog and manifest are published within 15 minutes, at the next cycle. Lower
  `OnUnitActiveSec=` in the timer for faster updates.
- A pin from the phone starts a cycle right away instead of waiting for the timer.

## 8. Next: the phone

Continue with [phone setup](phone-setup.md).
