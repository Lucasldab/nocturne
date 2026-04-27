package io.nocturne.phone.data.db.dao

import androidx.paging.PagingSource
import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import io.nocturne.phone.data.db.entity.ArtistEntity

@Dao
interface ArtistDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertAll(artists: List<ArtistEntity>)

    @Query("DELETE FROM artists")
    suspend fun deleteAll()

    @Query("SELECT * FROM artists ORDER BY name COLLATE NOCASE")
    fun pagedAll(): PagingSource<Int, ArtistEntity>

    @Query("SELECT * FROM artists WHERE id = :id")
    suspend fun byId(id: String): ArtistEntity?

    @Query("SELECT COUNT(*) FROM artists")
    suspend fun count(): Int
}
