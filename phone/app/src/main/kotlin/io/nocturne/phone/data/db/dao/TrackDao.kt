package io.nocturne.phone.data.db.dao

import androidx.paging.PagingSource
import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import androidx.room.Upsert
import io.nocturne.phone.data.db.entity.TrackEntity

@Dao
interface TrackDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertAll(tracks: List<TrackEntity>)

    @Upsert
    suspend fun upsert(track: TrackEntity)

    @Query("DELETE FROM tracks")
    suspend fun deleteAll()

    @Query("SELECT COUNT(*) FROM tracks")
    suspend fun count(): Int

    @Query("SELECT * FROM tracks WHERE id = :id")
    suspend fun byId(id: String): TrackEntity?

    @Query("SELECT * FROM tracks ORDER BY title COLLATE NOCASE")
    fun pagedAll(): PagingSource<Int, TrackEntity>

    @Query("SELECT * FROM tracks WHERE albumId = :albumId ORDER BY discNumber, trackNumber")
    fun pagedByAlbum(albumId: String): PagingSource<Int, TrackEntity>

    @Query("SELECT * FROM tracks WHERE albumArtistId = :artistId ORDER BY year, album, discNumber, trackNumber")
    fun pagedByArtist(artistId: String): PagingSource<Int, TrackEntity>

    @Query("SELECT * FROM tracks WHERE genreId = :genreId ORDER BY title COLLATE NOCASE")
    fun pagedByGenre(genreId: String): PagingSource<Int, TrackEntity>

    /** For manifest reconciliation: bulk update isResident flag. */
    @Query("UPDATE tracks SET isResident = :resident WHERE id IN (:ids)")
    suspend fun setResidentFor(ids: List<String>, resident: Boolean)

    /** Clear all residency before re-applying manifest (idempotent). */
    @Query("UPDATE tracks SET isResident = 0")
    suspend fun clearAllResident()
}
