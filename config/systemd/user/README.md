# systemd user units — nocturne

Reference templates for the desktop-side daemon's systemd integration.
Copy these into `~/.config/systemd/user/`, reload, enable.

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

The service's `ExecStart` calls `~/.local/bin/nocturne-pin-cycle-runner` — a
shell wrapper in `config/bin/` that implements the 5 s debounce, waits for
Syncthing `.tmp` sentinels to drain, then gates the cycle on nanosecond mtime
comparison (so a Syncthing rename and a manifest write that land in the same
wall-clock second don't silently suppress the retry).

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
cp config/bin/nocturne-pin-cycle-runner ~/.local/bin/
chmod +x ~/.local/bin/nocturne-pin-cycle-runner
systemctl --user daemon-reload
systemctl --user enable --now \
    nocturne-watch.service \
    nocturne-cycle.timer \
    nocturne-pin-cycle.path \
    nocturne-download.path
systemctl --user list-timers | grep nocturne
```

The `nocturne-download.path` unit fires `nocturned download` on every meta
dir change. The dispatcher reads `downloads-phone-*.jsonl`, dedups against
terminal-state entries it has already written to `downloads-desktop.jsonl`,
then shells out to `~/.local/bin/flacget` once per fresh request. `flacget`
itself calls `nocturned cycle` at the end so the new track flows into the
phone's catalog via the normal pin/Syncthing path.

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

Phone-side: Syncthing-Fork AND the Nocturne app both need the GrapheneOS
"battery optimization → don't optimize" exemption to keep syncing while the
screen is off. See `docs/phone-setup.md`.
