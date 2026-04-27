package io.nocturne.phone.data.db.entity

import androidx.room.ColumnInfo
import androidx.room.Entity
import androidx.room.Fts4

/**
 * FTS4 virtual table over the pre-folded `searchBlob` column on TrackEntity.
 * Tokenizer: unicode61 (Pitfall 5: stick with the standard tokenizer; do not
 * try to roll a custom one). Caller is responsible for folding queries before
 * MATCH (Plan 04-06 owns the query construction).
 */
@Fts4(contentEntity = TrackEntity::class, tokenizer = "unicode61")
@Entity(tableName = "track_fts")
data class TrackFts(
    @ColumnInfo(name = "searchBlob") val searchBlob: String,
)
