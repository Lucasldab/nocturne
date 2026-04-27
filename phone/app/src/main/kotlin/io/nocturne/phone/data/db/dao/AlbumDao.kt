package io.nocturne.phone.data.db.dao

import androidx.paging.PagingSource
import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import io.nocturne.phone.data.db.entity.AlbumEntity
import kotlinx.coroutines.flow.Flow

@Dao
interface AlbumDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertAll(albums: List<AlbumEntity>)

    @Query("DELETE FROM albums")
    suspend fun deleteAll()

    @Query("SELECT * FROM albums ORDER BY title COLLATE NOCASE")
    fun pagedAll(): PagingSource<Int, AlbumEntity>

    @Query("SELECT * FROM albums WHERE id = :id")
    suspend fun byId(id: String): AlbumEntity?

    @Query(
        "SELECT * FROM albums WHERE albumArtistId = :artistId " +
            "ORDER BY year DESC, title COLLATE NOCASE",
    )
    fun albumsByArtist(artistId: String): Flow<List<AlbumEntity>>

    @Query("SELECT COUNT(*) FROM albums")
    suspend fun count(): Int
}
