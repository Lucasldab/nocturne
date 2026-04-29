package io.nocturne.phone.data.stats

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * JSONL line schemas — byte-frozen contract per docs/jsonl-spec.md §4
 * and tests/fixtures/jsonl-goldens/.
 *
 * INVARIANTS (do NOT change without bumping `v` AND coordinating with
 * Phase 7 ingester):
 *   - Property declaration order = JSON field order. kotlinx.serialization
 *     emits properties in declaration order; reordering breaks byte parity.
 *   - `v` has NO default. Callers always pass v = 1 explicitly so encoding
 *     does not depend on `encodeDefaults` (per D-06).
 *   - `played_ms` / `duration_ms` are snake_case in JSON; @SerialName is the
 *     only correct way to get that byte shape.
 *   - String fields ("kind", "unit", "track", "id") are emitted as JSON
 *     strings; Boolean fields ("liked", "pinned") as bare true/false; Long
 *     fields ("ts", playedMs, durationMs) as bare integers.
 */
@Serializable
data class StatsEvent(
    val v: Int,
    val ts: Long,
    val kind: String,                                  // "play" or "skip"
    val track: String,                                 // 64-char lowercase hex sha256
    @SerialName("played_ms") val playedMs: Long,
    @SerialName("duration_ms") val durationMs: Long,
)

@Serializable
data class LikeEvent(
    val v: Int,
    val ts: Long,
    val unit: String,                                  // "track" or "album"
    val id: String,
    val liked: Boolean,
)

@Serializable
data class PinEvent(
    val v: Int,
    val ts: Long,
    val unit: String,                                  // "track" or "album"
    val id: String,
    val pinned: Boolean,
)

/**
 * Long-press track/album action — "unsync" (unpin and unload) or "delete"
 * (didn't like; nuke from archive + resident, blacklist sha). Daemon
 * dispatches to actions.c. Field order frozen.
 */
@Serializable
data class ActionEvent(
    val v: Int,
    val ts: Long,
    val unit: String,                                  // "track" or "album"
    val id: String,
    val action: String,                                // "unsync" or "delete"
)
