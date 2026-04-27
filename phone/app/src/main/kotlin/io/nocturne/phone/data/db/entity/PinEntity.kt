package io.nocturne.phone.data.db.entity

import androidx.compose.runtime.Immutable
import androidx.room.Entity
import androidx.room.PrimaryKey

/**
 * User-generated pin record. Phase 5 owns local persistence; Phase 6 emits
 * one JSONL line per row to `pins-phone-<deviceid>.jsonl`. The `synced`
 * column flips to `true` when Phase 6 has committed the row to JSONL.
 *
 * Schema v=4 (Phase 6 / D-17): adds `pinned: Boolean` for unpin tombstone
 * support. Row presence + column flip — row stays in the table after unpin,
 * but `pinned = false` is the LWW tombstone PinsWriter emits as JSONL.
 *
 * PRIMARY KEY is `id` alone (a track sha256 or a synthetic albumId); the
 * `unit` column tells the daemon's resolver which join to use. Room treats
 * this as the simplest possible append-via-upsert table.
 */
@Entity(tableName = "pins")
@Immutable
data class PinEntity(
    @PrimaryKey val id: String,
    val unit: String,            // "track" or "album"
    val pinnedAt: Long,          // System.currentTimeMillis()
    val synced: Boolean = false, // Phase 6 sets true after JSONL write
    val pinned: Boolean = true,  // Phase 6 (D-17): false = unpinned tombstone awaiting drain
)
