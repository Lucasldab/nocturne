package io.nocturne.phone.data.db

import androidx.room.Database
import androidx.room.RoomDatabase
import androidx.room.TypeConverters
import io.nocturne.phone.data.db.entity.AlbumEntity
import io.nocturne.phone.data.db.entity.ArtistEntity
import io.nocturne.phone.data.db.entity.Converters
import io.nocturne.phone.data.db.entity.GenreEntity
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.data.db.entity.TrackFts

@Database(
    entities = [
        TrackEntity::class,
        AlbumEntity::class,
        ArtistEntity::class,
        GenreEntity::class,
        TrackFts::class,
    ],
    version = 1,
    exportSchema = true,
)
@TypeConverters(Converters::class)
abstract class NocturneDatabase : RoomDatabase() {
    // DAO accessors added in Task 2.
}
