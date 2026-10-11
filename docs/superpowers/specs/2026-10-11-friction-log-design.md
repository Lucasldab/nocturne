# Friction log and PM review agent — design

Date: 2026-10-11 · Status: approved in chat, awaiting spec review

## Problem

Missing features in nocturne surface **while listening**, and are gone by the time
there is a laptop in reach. Today they end up as loose lines in `~/notes/todo.md`, or
nowhere. An AI reviewer cannot guess what the one user of this app wants; it needs
those moments written down.

## Goals

1. Capture a gap from the phone in a few seconds, from inside or outside the app.
2. Each gap carries enough context to be acted on later (where, what was playing,
   which build).
3. A Claude Code agent turns the accumulated gaps plus the code into a ranked list of
   proposals, and asks instead of deciding.

## Non-goals

- No daemon (nocturned) behaviour. It must not ingest, store or react to gaps.
- No issue tracker, no sync of triage back to the phone, no in-app list of past gaps.
- No network: the app keeps its no-`INTERNET` rule (CROSS-01); gaps travel by
  Syncthing like every other phone stream.

## 1. Capture on the phone

### Entry points

- **Launcher shortcut.** A static app shortcut "Log a gap" (long-press the nocturne
  icon). It launches `MainActivity` with action `io.nocturne.phone.action.LOG_GAP`;
  `MainActivity` opens the gap sheet over whatever screen is restored. It is
  available even when the app is not running.
- **Now Playing.** The top row has free space where the heart was removed. It gets a
  `+gap` text button, in the same JetBrains Mono label style as `~/now-playing`.

The mini player's long-press is **not** used. It is reserved for the
hold-to-unpin feature already on the wishlist.

### The gap sheet

A modal bottom sheet with a single text field (autofocused, keyboard up) and a Send
button. Behaviour:

- Send is disabled while the trimmed text is empty.
- Text is capped at **1000 characters** (the field stops accepting input). The cap
  keeps a line well under the 4096-byte `PIPE_BUF` bound from jsonl-spec §6, even
  with multi-byte UTF-8 (1000 × 3 bytes + the other fields < 4096).
- On Send: write the event (below), dismiss, toast **"Gap logged"**. On write
  failure: toast **"Gap not saved — sync folder unavailable"** and keep the sheet
  open with the text intact, so nothing typed is lost.
- Back / swipe-down dismisses without writing.

### Context captured

| Field | Source |
|---|---|
| `screen` | The current Navigation route **pattern** (`now-playing`, `album/{albumId}`, `settings`, …). Patterns, not concrete routes, so no ids leak into free text. `"launcher"` when opened from the shortcut before any route is resolved. |
| `track` | sha256 of the current MediaController item, or `null` when nothing is loaded. |
| `app` | `BuildConfig.VERSION_NAME`. |

## 2. The stream: `gaps-<writer>.jsonl`

A new append-only stream at the top level of the meta folder, written with the existing
`JsonlFileWriter` (SAF append + fsync, LF only). The new `GapsWriter` is a direct
passthrough, like `ActionsWriter`: no Room table, no retry queue. A failed write is
reported to the sheet instead.

```json
{"v":1,"ts":1760212345678,"text":"queue can't reorder by drag","screen":"now-playing","track":"<sha256>","app":"0.4.48"}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `v` | int | yes | `1` |
| `ts` | int | yes | unix ms, same range rule as jsonl-spec §5 |
| `text` | string | yes | 1–1000 chars, trimmed. Embedded newlines are replaced with a space before encoding. |
| `screen` | string | yes | route pattern, or `"launcher"`, or `"desktop"` |
| `track` | string \| null | yes | 64-char lowercase hex, or `null` |
| `app` | string | yes | phone version name, or `"desktop"` |

Writers: the phone writes `gaps-phone-<deviceid>.jsonl`; the desktop tool (§3) writes
`gaps-desktop.jsonl`. Each file has exactly one writer, matching jsonl-spec §3.

`docs/jsonl-spec.md` gains a §4.x for this stream. That section states that the ingester
**does not read it**: `run_glob` matches named patterns only, so the file is inert to
nocturned. A test asserts it (§5).

## 3. Desktop: `config/bin/nocturne-gap`

A POSIX sh script beside `nocturne-cycle-run`, installed to `~/.local/bin` on soot. The
meta folder lives on **hearth** (`/home/lucas/sync/nocturne/meta`), so the script
works over `ssh hearth`. The host and path can be overridden with `NOCTURNE_GAP_HOST`
and `NOCTURNE_GAP_META`.

- `nocturne-gap "text"` appends a line to `gaps-desktop.jsonl`, with
  `screen:"desktop"`, `track:null` and `app:"desktop"`. The JSON is built with `jq -cn`,
  so no hand-escaping is needed.
- `nocturne-gap -l` prints every gap from every `gaps-*.jsonl`, oldest first:
  `2026-10-11 22:31  now-playing  queue can't reorder by drag`.

**Seeding.** Once the script exists, the two wishlist items move out of
`~/notes/todo.md` into the log with `nocturne-gap`:
- "Improve queue view"
- "Hold to delete/unpin the mini player's song"

## 4. PM review agent: `.claude/agents/nocturne-pm.md`

A project subagent the user invokes on demand ("run the nocturne PM"). It runs under
the subscription, with no API billing.

**Reads:**
- every `gaps-*.jsonl` file, through `nocturne-gap -l` and the raw files over ssh
- `docs/gaps-triage.md`
- `README.md`, `docs/`, `CLAUDE.md`
- the code, read selectively

**Produces**, in chat:
1. **Proposals, ranked.** A proposal may cover several gaps. Each one names the gaps
   behind it (by timestamp), the files it would touch, a rough size (S/M/L), and any
   conflict with a hard boundary (CROSS-01, CROSS-03, the no-network daemon).
2. **Half-built paths.** Code that is most of the way to something useful: unused
   columns, dead routes, a TODO with a body.
3. **Questions.** Anything where the user's intent is ambiguous. The agent asks; it
   does not pick for the user.

**Writes** only `docs/gaps-triage.md`. That is a table of gap `ts` → status
(`open` / `planned` / `done` / `declined`) with a one-line note. Gaps not in the table
are new. The agent never edits code, never commits, and never touches the JSONL files.

## 5. Testing

- **Byte shape:** a `GapEvent` golden is added to the existing `ByteShapeGoldenTest`
  pattern, plus `tests/fixtures/jsonl-goldens/gaps-golden.jsonl`, checked by `tests/test_jsonl_goldens.sh`.
- **GapsWriter (Robolectric):**
  - it appends to `gaps-phone-<id>.jsonl`
  - newlines are flattened
  - text over 1000 characters is rejected (`require`), mirroring `ActionsWriter`
  - empty text is rejected
- **Sheet state** (a ViewModel unit test): Send is disabled when empty, the text is
  kept on failure, and it is cleared on success.
- **Ingest stays inert:** a C test in `tests/test_ingest.c` runs ingest over a
  meta dir that contains a `gaps-phone-x.jsonl` and asserts the stats come out the
  same as without it.
- **`nocturne-gap`:** `tests/test_nocturne_gap.sh`, following `test_pin_cycle_runner.sh`. It runs with `NOCTURNE_GAP_HOST=` (empty means local) and
  a temp `NOCTURNE_GAP_META`. It writes two lines, then checks that `-l` prints
  them in order and that each line parses with `jq`.
- **Manual, once:**
  1. Log a gap from the launcher shortcut, and one from Now Playing.
  2. Confirm both appear on hearth and in `nocturne-gap -l`.
  3. Run the agent once against them.

## Rollout

1. Implement on branch `feat/friction-log`, with the phone version bumped.
2. A `v*` tag ships the APK through CI, and Obtainium picks it up.
3. Install `nocturne-gap` on soot.
4. Seed the log from `todo.md`, and remove those two lines from `todo.md`.
