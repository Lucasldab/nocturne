package io.nocturne.phone.data.db.entity

import androidx.compose.runtime.Immutable
import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey

@Entity(
    tableName = "genres",
    indices = [Index("name")],
)
@Immutable
data class GenreEntity(
    @PrimaryKey val id: String,
    val name: String,
    val trackCount: Int,
)
