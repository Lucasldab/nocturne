# nocturne JSONL stream specification

> **Status: Normative.** This document is the protocol contract between
> the Phase 6 phone stats writer and the Phase 7 desktop ingester. It
> was locked at the Phase 7 ship date (2026-04-26). The Phase 6
> implementer MUST produce JSONL bytes that match the goldens in
> `tests/fixtures/jsonl-goldens/` byte-for-byte (modulo `<sha256>`,
> `<album-key>`, `<deviceid>`, `<ts>` substitutions).

The key words MUST, MUST NOT, REQUIRED, SHALL, SHALL NOT, SHOULD, SHOULD
NOT, RECOMMENDED, MAY, OPTIONAL in this document are to be interpreted
as described in [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

---

## 1. Status

Normative for Phase 6 (Android stats writer). Locked at the Phase 7
ship date 2026-04-26. Future protocol-breaking revisions MUST bump the
per-line schema version (`v`) field; backward-compatible additions
(new optional fields) MAY be made without a version bump.

---

## 2. Scope

This document covers ONLY the three JSONL streams written by the phone
into the metadata sync folder:

- `stats/phone-<deviceid>.jsonl`
- `likes-phone-<deviceid>.jsonl`
- `pins-phone-<deviceid>.jsonl`

It does NOT cover:

- `catalog.json` and `manifest.json` — those have their own JSON
  Schemas under `schema/` (Phase 2 publisher).
- Syncthing folder configuration — see `docs/install.md`.

The desktop ingester (`build/nocturned ingest`, `src/nocturned/ingest.{h,c}`)
is the canonical reader for these streams. Source of truth for ambiguities
is the goldens directory and `tests/test_ingest.c`.

---

## 3. File naming convention

| File | Glob | Location |
|------|------|----------|
| Stats events | `stats/phone-<deviceid>.jsonl` | under `stats/` subdir |
| Like events | `likes-phone-<deviceid>.jsonl` | top-level of meta_dir |
| Pin events | `pins-phone-<deviceid>.jsonl` | top-level of meta_dir |

`<deviceid>` is a stable opaque token unique per phone install. It MUST
be:

- **NOT** the hostname.
- **NOT** the IMEI / hardware serial.
- **NOT** the user-visible device name.

Recommended generation: 4 random bytes hex-encoded (8 hex chars) chosen
at first run, stored in DataStore on the phone. This gives a privacy-
preserving stable identifier that survives reinstall as long as the
DataStore is restored.

The phone MUST be the single writer for any file bearing its own
`<deviceid>`. The ingester opens these files with `O_RDONLY` only.

---

## 4. Per-event JSON shape

### 4.1 Stats events (`stats/phone-<deviceid>.jsonl`)

```json
{"v":1,"ts":1745678910123,"kind":"play","track":"<sha256>","played_ms":231400,"duration_ms":237000}
{"v":1,"ts":1745679200000,"kind":"skip","track":"<sha256>","played_ms":4200,"duration_ms":237000}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `v` | integer | yes | Line schema version. MUST be `1`. |
| `ts` | integer | yes | Unix milliseconds. See §5 for range. |
| `kind` | string | yes | `"play"` or `"skip"`. Other values → ingester logs and skips the line. |
| `track` | string | yes | sha256 of the track audio bytes, hex-encoded, lowercase, exactly 64 characters. |
| `played_ms` | integer | yes | How long the user actually listened. MUST be `>= 0`. |
| `duration_ms` | integer | yes | The track's full duration in ms. MUST be `>= 0`. |

A "play" is an event the user materially listened to; a "skip" is one
they bailed out of early. The phone-side classifier rule is documented
in PROJECT.md (Phase 6) and is not normative here — the ingester
treats them only as labels.

### 4.2 Like events (`likes-phone-<deviceid>.jsonl`)

```json
{"v":1,"ts":1745679200000,"unit":"track","id":"<sha256>","liked":true}
{"v":1,"ts":1745679400000,"unit":"track","id":"<sha256>","liked":false}
{"v":1,"ts":1745679500000,"unit":"album","id":"<album-key>","liked":true}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `v` | integer | yes | Line schema version. MUST be `1`. |
| `ts` | integer | yes | Unix milliseconds. See §5 for range. |
| `unit` | string | yes | `"track"` or `"album"`. |
| `id` | string | yes | For `unit="track"`: 64-char lowercase hex sha256. For `unit="album"`: opaque album key (the phone's chosen album-grouping identifier; treated as opaque by the ingester). |
| `liked` | bool | yes | `true` = liked, `false` = unliked tombstone. |

### 4.3 Pin events (`pins-phone-<deviceid>.jsonl`)

```json
{"v":1,"ts":1745679500000,"unit":"album","id":"<album-key>","pinned":true}
{"v":1,"ts":1745679600000,"unit":"track","id":"<sha256>","pinned":true}
{"v":1,"ts":1745679700000,"unit":"album","id":"<album-key>","pinned":false}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `v` | integer | yes | Line schema version. MUST be `1`. |
| `ts` | integer | yes | Unix milliseconds. See §5 for range. |
| `unit` | string | yes | `"track"` or `"album"`. |
| `id` | string | yes | Same shape rules as §4.2. |
| `pinned` | bool | yes | `true` = pinned, `false` = unpinned tombstone. |

---

## 5. Encoding rules

- One JSON object per line, terminated by **one** `\n` (LF, 0x0A) byte.
  CRLF (`\r\n`) is NOT allowed.
- UTF-8 only. ASCII identifiers are RECOMMENDED for `<album-key>` to
  minimise byte-shape ambiguity, but the ingester accepts arbitrary
  UTF-8.
- No trailing whitespace before the `\n`. No pretty-printing. No
  embedded `\n` inside a single line.
- The `ts` field MUST be a JSON integer (no quotes, no decimal point),
  representing **unix milliseconds** (NOT seconds, NOT microseconds).
- Allowed `ts` range: `1577836800000` (2020-01-01T00:00:00Z) inclusive
  to `now + 86400000` (one day in the future, accommodating clock
  skew). Lines outside this range are skipped by the ingester with
  `parse_error` incremented.

---

## 6. fsync discipline (phone-side, normative)

The phone MUST:

1. Open the target file with `O_WRONLY | O_APPEND | O_CREAT`.
2. Write each event line as a **single** `write(2)` containing the
   complete JSON object plus the trailing `\n`. The line MUST fit in
   `PIPE_BUF` (4096 bytes on Linux/Android) for POSIX append-atomicity
   to apply. The goldens demonstrate lines well under this bound.
3. Call `fsync(fd)` BEFORE returning from the user-visible action
   callback (the listen-classifier callback for plays, the like/pin
   button handler for those).
4. On `write(2)` or `fsync(2)` failure: queue the event in memory and
   retry on the next event. Lost events are acceptable per the Phase 6
   threat model — the multi-day-offline tolerance test (Phase 6 highest-
   risk gate) is the pinch point we optimise for, not perfect at-most-
   once durability under worst-case storage failures.

The ingester MAY observe a "trailing partial line" at any time (a
write completed but `\n` hasn't been flushed yet by Syncthing): see §10.

---

## 7. Idempotency / replay rules

- **Stats events** are append-only. The desktop ingester uses a byte-
  offset table (`ingest_offsets`) so each event is processed at most
  once. The phone MUST NOT rewrite already-emitted bytes.

- **Likes / pins** use last-writer-wins per `(unit, id)` keyed on max
  `ts`. Tombstones are explicit (`liked: false`, `pinned: false`),
  not implied by absence. Out-of-order replay (an older event arriving
  after a newer one) converges deterministically because the ingester
  applies `WHERE excluded.ts >= existing.ts` — note the `>=`, not `>`.

- **Files MUST NOT shrink.** If the ingester observes a file's current
  size to be less than the persisted offset, it logs a warning and
  resets the offset to 0 (treating the shrink as a Syncthing-anomaly
  truncation). The phone MUST NOT rotate or truncate these files —
  compaction is a future concern (Pitfall 15) and will be coordinated
  via a separate protocol when needed.

---

## 8. Versioning / forward compatibility

- `v` is the line-level schema version. Currently `v == 1`.
- Adding new OPTIONAL fields: NO version bump. The ingester ignores
  unknown fields.
- Removing or renaming REQUIRED fields, or changing the type of an
  existing field: bump `v`. The phone SHOULD write both old and new
  for at least one release; the ingester reads either.
- Extending an enum (e.g. adding a new `kind` value): the ingester
  logs and skips lines with unknown values without aborting the file.
  Counts as `parse_error`.

---

## 9. Goldens

The reference byte shapes are committed under
`tests/fixtures/jsonl-goldens/`:

- `stats-golden.jsonl` (3 lines)
- `likes-golden.jsonl` (3 lines)
- `pins-golden.jsonl` (3 lines)

Phase 6 implementers SHOULD: produce JSONL on Android, byte-diff your
output line against the corresponding golden line. They MUST be
identical up to substitution of `<sha256>`, `<album-key>`,
`<deviceid>`, and `<ts>` placeholders.

`tests/test_jsonl_goldens.sh` is the integration test that ingests the
goldens via `nocturned ingest` and asserts the resulting DB state.
That test is run on every `make test`.

---

## 10. Limits

- **Max line length: 64 KiB** (`JSONL_MAX_LINE` in
  `src/nocturned/jsonl.h`). Lines exceeding the cap are rejected by
  the ingester with `EMSGSIZE`; the file is NOT permanently stuck —
  the ingester drains past the offending line and resumes. The phone
  MUST never emit anywhere near this cap; expected line lengths are
  well under 256 bytes.
- **Trailing partial line** (no `\n` yet): tolerated by the ingester.
  The persisted offset stops at the last full `\n` boundary. Next
  ingest pass picks up the rest of the line once Syncthing has
  flushed it. The phone is REQUIRED to flush a complete `\n`-
  terminated line per event.

---

## 11. Single-writer per file

- The phone MUST never write to a file with a `<deviceid>` not its
  own. Multi-writer is impossible by construction (per-device naming).
- The desktop ingester opens source files with `O_RDONLY` only and
  never modifies them.
- Future devices (e.g. a second phone) follow the same naming. Each
  has its own deviceid; their files don't collide.

---

## 12. Error handling on phone

- If `write(2)` or `fsync(2)` fails, the phone MUST:
  - Queue the event in memory with backoff.
  - Retry on the next event tick OR when the user takes the next
    listen / like / pin action, whichever is first.
- Lost events (memory full, app killed before retry) are acceptable
  per Phase 6 design. The protocol does NOT guarantee at-least-once
  delivery — it guarantees that whatever events DO get written are
  processed at-most-once by the ingester.

---

## 13. Examples

### 13.1 Complete `stats/phone-G0LD.jsonl`

```
{"v":1,"ts":1745678910123,"kind":"play","track":"9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d","played_ms":231400,"duration_ms":237000}
{"v":1,"ts":1745679200000,"kind":"skip","track":"9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d","played_ms":4200,"duration_ms":237000}
{"v":1,"ts":1745679300000,"kind":"play","track":"b1c2d3e4f50617283940a1b2c3d4e5f60718293a4b5c6d7e8f900112233445dd","played_ms":180000,"duration_ms":180000}
```

### 13.2 Complete `likes-phone-G0LD.jsonl`

```
{"v":1,"ts":1745679200000,"unit":"track","id":"9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d","liked":true}
{"v":1,"ts":1745679400000,"unit":"track","id":"9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d","liked":false}
{"v":1,"ts":1745679500000,"unit":"album","id":"alb_abcd1234","liked":true}
```

### 13.3 Complete `pins-phone-G0LD.jsonl`

```
{"v":1,"ts":1745679500000,"unit":"album","id":"alb_abcd1234","pinned":true}
{"v":1,"ts":1745679600000,"unit":"track","id":"b1c2d3e4f50617283940a1b2c3d4e5f60718293a4b5c6d7e8f900112233445dd","pinned":true}
{"v":1,"ts":1745679700000,"unit":"album","id":"alb_abcd1234","pinned":false}
```

These are byte-identical to the goldens in
`tests/fixtures/jsonl-goldens/`.

---

## 14. References

- `src/nocturned/jsonl.h` — line reader API (offset tracking, 64 KiB
  cap).
- `src/nocturned/ingest.h` / `ingest.c` — engine and validation gates.
- `tests/fixtures/jsonl-goldens/` — byte-frozen reference fixtures.
- `tests/test_jsonl_goldens.sh` — integration assertion against the
  goldens.
- `tests/test_ingest.c` — unit tests for the ingester (each validation
  rule has a case).
- `.planning/phases/07-desktop-ingester/PLAN.md` — phase context and
  threat model.
