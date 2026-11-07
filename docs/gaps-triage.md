# Gaps triage

Friction-log entries (`nocturne-gap -l`) and what became of them. Maintained by
the `nocturne-pm` agent; the JSONL files stay append-only. A ts missing from
this table is untriaged.

Status: `open` · `planned` · `done` · `declined`

| ts | gap | status | note |
|---|---|---|---|
| 1791787470606 | test from launcher | declined | acceptance test |
| 1791787516225 | test from now playing | declined | acceptance test |
| 1791787774903 | Improve the queue view | done | 0.4.51-dev: queue shows play order (follows shuffle) as `↑ N played` (tap to expand) · `▸` now · `── up next · N ──`; `+` marks hand-queued tracks; drag `≡` to reorder (shuffle off), swipe to remove; play next is FIFO and stays next with shuffle on (QueueShuffleOrder); albums get the same swipe → play next / add to queue; Tracks tab queues in the shown sort order (was always a-z); search swipe/long-press actions were no-ops, now wired; duplicate tracks no longer share a row key |
| 1791787775037 | Hold to delete/unpin the mini player's song | done | 0.4.51-dev: hold unpins directly (haptic + `unpinned · <title>` snackbar with undo, no sheet). The unpin is staged; pin tombstone + unsync action are written to meta only when the snackbar closes, so undo leaves the pin untouched. Delete stays on the Now Playing queue row long-press and track rows |
