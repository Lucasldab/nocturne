# systemd user units — nocturne

Reference templates for the server-side daemon's systemd integration.
Copy these into `~/.config/systemd/user/`, reload, enable. They assume the
binary at `/usr/local/bin/nocturned`, the library at `~/music` and the meta
folder at `~/sync/nocturne/meta`; edit the units if yours differ.

## Files

| Unit | Role |
|------|------|
| `nocturne-watch.service` | Long-lived inotify watcher; rescans subtrees on FS change. Sub-second latency for new files in `~/music`. Holds the single-writer PID lock for its lifetime. |
| `nocturne-cycle.service` | One-shot scan→ingest→resolve→rotate→publish. Stops watch first, restarts it after. Triggered by the timer or manually. |
| `nocturne-cycle.timer` | Fires `nocturne-cycle.service` every 15 minutes. Pairs with watch to drive the catalog/manifest publish loop. |
| `nocturne-pin-cycle.service` | Wrapper that runs cycle when phone JSONL writes land in the meta dir. Same lock-handover dance as cycle. |
| `nocturne-pin-cycle.path` | Path watcher that fires `nocturne-pin-cycle.service` on changes inside `~/sync/nocturne/meta/`. |
| `nocturne-download.service` | Runs `nocturned download` to dispatch phone-side download requests via `flacget`. Holds its own lockfile — independent of the cycle lock. |
| `nocturne-download.path` | Watches the meta dir and fires `nocturne-download.service`. Loop-safe: the dispatcher's id-set dedup makes spurious fires no-ops. |
| `nocturne-discover.service` | Runs `nocturned discover` to pick the week's discovery tracks, then starts a cycle. |
| `nocturne-discover.timer` | Fires `nocturne-discover.service` on Mondays at midnight. |

The pin-cycle service's `ExecStart` calls `~/.local/bin/nocturne-pin-cycle-runner`
— a shell wrapper in `config/bin/` that implements the 5 s debounce, waits for
Syncthing `.tmp` sentinels to drain, then gates the cycle on nanosecond ctime
comparison. It uses ctime because Syncthing copies the phone's mtime onto the
file, so a pin that syncs late would otherwise look older than the manifest.
The cycle service calls `~/.local/bin/nocturne-cycle-run`, which arbitrates the
lock with the watcher and retries `nocturned cycle`.

## Lock coordination

`nocturned` enforces single-writer access via a flock on its pidfile. Watch
holds it indefinitely; cycle / pin-cycle / scan / publish each take it for
the duration of their run. Coexistence pattern in this repo's units:

```
ExecStartPre=-/usr/bin/systemctl --user stop nocturne-watch.service
ExecStart=...                       # the actual cycle work
ExecStartPost=-/usr/bin/systemctl --user start --no-block nocturne-watch.service
```

Leading `-` makes the lock-handover non-fatal (watch may already be
stopped). `--no-block` lets cycle exit before watch fully boots back up.
End-to-end handover is ~2 seconds of lost inotify coverage.

## Install

```sh
mkdir -p ~/.config/systemd/user ~/.local/bin
cp config/systemd/user/*.{service,timer,path} ~/.config/systemd/user/
install -m 0755 config/bin/nocturne-cycle-run config/bin/nocturne-pin-cycle-runner ~/.local/bin/
systemctl --user daemon-reload
systemctl --user enable --now \
    nocturne-watch.service \
    nocturne-cycle.timer \
    nocturne-discover.timer \
    nocturne-pin-cycle.path
systemctl --user list-timers | grep nocturne
```

To let the phone request downloads, also enable `nocturne-download.path`. It
fires `nocturned download` on every meta dir change. The dispatcher reads
`downloads-phone-*.jsonl`, dedups against terminal-state entries it has already
written to `downloads-desktop.jsonl`, then runs the command set in
`[download] flacget_path` (default `~/.local/bin/flacget`) once per fresh request.
That command is not part of this repo. It should end by running `nocturned cycle`,
so the new track flows into the phone's catalog.

Verify watch is up:

```sh
systemctl --user status nocturne-watch.service
# look for: "library=/home/$USER/music dirs=N watches=N debounce_ms=1000"
```

## Latency expectations

- Drop a song into `~/music/` → DB updated in ≤2 s (watch).
- Catalog/manifest published to `~/sync/nocturne/meta/` within ≤15 min (next cycle tick).
- Phone Syncthing pulls meta + Nocturne app reconciles → ≤45 s after publish.
- **End-to-end worst case: ≈16 min.** Tighten by lowering `OnUnitActiveSec=` in the timer (5 min is fine; 1 min is overkill).

## GrapheneOS / battery exemption

Phone-side: Syncthing-Fork and the nocturne app both need the battery
optimization exemption ("Unrestricted") to keep syncing while the screen is off.
See `docs/phone-setup.md`.
