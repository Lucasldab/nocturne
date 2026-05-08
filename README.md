# nocturne

Self-hosted music ecosystem for one user. A Linux desktop holds the full
library (100 GB+ archival); a GrapheneOS phone carries a smart-rotating
~10–20 GB subset that adapts to listening habits. The phone browses the
full catalog by metadata and pins albums or tracks to bring them on-device;
a desktop daemon resolves bucket rules (recent adds, top played, recent
plays, loved, exploration) and pushes only the resident set over Syncthing.
Listening stats flow back from the phone to drive the next rotation.

The phone always has the right music for *now* — without manual playlist
curation, without streaming, without third parties.

## Status

Pre-v1, single-user. Daemon shipped through Phase 8 (bucket-tuning); phone
app is on `0.4.x` with the system surface, NowPlaying, pin-as-download UX,
embedded album art, and predictive-back hooked up. Latest sideload APKs
under [Releases](https://github.com/Lucasldab/nocturne/releases).

## Components

| Path     | What                                                           |
|----------|----------------------------------------------------------------|
| `src/`   | `nocturned` daemon (C / SQLite / TagLib / inotify)             |
| `phone/` | Android app (Kotlin / Jetpack Compose / Media3)                |
| `schema/`| SQL schema + JSONL spec for catalog, manifest, stats           |
| `tools/` | Local helpers (resume hints, fixture scripts)                  |
| `tests/` | C tests + fixtures                                             |
| `docs/`  | Install, phone setup, JSONL format, troubleshooting            |
| `vendor/`| Vendored deps (`toml-c`)                                       |

## Transport

Syncthing only. Two folder pairs:

- `nocturne-meta`  — bidirectional. Catalog, rotation manifest, stats JSONL.
- `nocturne-files` — receive-only on phone. Audio files for the resident set.

No custom HTTP server, no Tailscale, no cloud, no telemetry, no accounts.

## Build

Daemon:

```sh
make           # builds nocturned
make test      # runs C tests
```

Phone (sideload-only — Obtainium or `adb install`):

```sh
cd phone
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/nocturne-phone-debug.apk
```

See `docs/install.md` and `docs/phone-setup.md` for first-run setup.

## Pin-as-download (real-time)

Phone-side pin tap → JSONL written → Syncthing-meta pull → desktop runs
`nocturned cycle` → `manifest.json` + audio file pushed back. The desktop
side is four systemd user units that share the SQLite single-writer lock:

| Unit                        | Role                                                       |
|-----------------------------|------------------------------------------------------------|
| `nocturne-watch.service`    | Long-lived inotify watcher on `~/music`; keeps DB current  |
| `nocturne-cycle.timer`      | Fires `nocturne-cycle.service` every 15 min                |
| `nocturne-pin-cycle.path`   | Watches `~/sync/nocturne/meta` for phone-side JSONL writes |
| `nocturne-pin-cycle.service`| Debounces, drains Syncthing `.tmp`, runs `nocturned cycle` |

The watcher holds the writer lock for its lifetime; the cycle services
stop it via `ExecStartPre` and restart it via `ExecStartPost` so the
pipeline can take the lock for one pass. The pin-cycle runner script
re-stops the watcher after its 5 s debounce and retries on lock-busy
to defeat the start/stop race when both services fire near-simultaneously.

Canonical units and the runner script are checked into the repo:

```sh
sudo install -m 0755 build/nocturned /usr/local/bin/nocturned
install -m 0755 config/bin/nocturne-pin-cycle-runner ~/.local/bin/
install -Dm 0644 config/systemd/user/*.{service,timer,path} \
    -t ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now \
    nocturne-watch.service \
    nocturne-cycle.timer \
    nocturne-pin-cycle.path
```

Pin → resident-on-phone latency: ~30–60 s end-to-end, dominated by the
Syncthing roundtrip.

## License

MIT — see `LICENSE`.
