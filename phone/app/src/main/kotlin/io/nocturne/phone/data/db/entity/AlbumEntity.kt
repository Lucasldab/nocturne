package io.nocturne.phone.data.db.entity

import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey

/**
 * Album row. Synthetic primary key; stored alongside tracks for fast browse.
 * `hasResident` is cached during import to short-circuit "any resident track?"
 * queries on the album-list screen (CAT-05 dim).
 */
@Entity(
    tableName = "albums",
    indices = [Index("title"), Index("year")],
)
data class AlbumEntity(
    @PrimaryKey val id: String,
    val title: String,
    val albumArtist: List<String>,
    val year: Int?,
    val trackCount: Int,
    val totalSizeBytes: Long,
    val hasResident: Boolean,
)
