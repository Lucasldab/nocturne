package io.nocturne.phone.data.db.dao

import androidx.paging.PagingSource
import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import io.nocturne.phone.data.db.entity.GenreEntity

@Dao
interface GenreDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertAll(genres: List<GenreEntity>)

    @Query("DELETE FROM genres")
    suspend fun deleteAll()

    @Query("SELECT * FROM genres ORDER BY name COLLATE NOCASE")
    fun pagedAll(): PagingSource<Int, GenreEntity>

    @Query("SELECT * FROM genres WHERE id = :id")
    suspend fun byId(id: String): GenreEntity?

    @Query("SELECT COUNT(*) FROM genres")
    suspend fun count(): Int
}
