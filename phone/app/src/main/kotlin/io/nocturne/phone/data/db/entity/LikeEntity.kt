package io.nocturne.phone.data.db.entity

import androidx.compose.runtime.Immutable
import androidx.room.Entity

/**
 * Phase 6 (D-15 / D-19 / STATS-03): like state for tracks and albums.
 *
 * Composite PK (id, unit): the same id can appear once per unit
 * (e.g. a user likes both an album and one of its tracks individually).
 *
 * `liked = false` is the unliked tombstone (LWW per docs/jsonl-spec.md §7).
 * `synced` flips to `true` once LikesWriter has emitted the JSONL line.
 */
@Entity(
    tableName = "likes",
    primaryKeys = ["id", "unit"],
)
@Immutable
data class LikeEntity(
    val id: String,            // track sha256 or AlbumEntity.id
    val unit: String,          // "track" or "album"
    val liked: Boolean,        // false = unliked tombstone awaiting drain
    val likedAt: Long,         // System.currentTimeMillis() at toggle
    val synced: Boolean = false,
)
