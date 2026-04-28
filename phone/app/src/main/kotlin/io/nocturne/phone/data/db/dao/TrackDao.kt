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

    /**
     * Non-paged album track list for AlbumQueueBuilder.
     * Albums are bounded (typically <30 tracks) — no Paging needed for queue
     * construction in plan 05-03 (PLAY-07).
     */
    @Query("SELECT * FROM tracks WHERE albumId = :albumId ORDER BY discNumber, trackNumber")
    suspend fun listByAlbum(albumId: String): List<TrackEntity>

    /**
     * Non-paged full library list, same ordering as [pagedAll]. Used when
     * tapping a row in the Tracks tab queues the entire library from that
     * track forward, so playback flows like one giant playlist.
     */
    @Query("SELECT * FROM tracks ORDER BY title COLLATE NOCASE")
    suspend fun listAll(): List<TrackEntity>

    /**
     * First resident track of an album. Used as the embedded-artwork source
     * when rendering AlbumRow cover art via MediaMetadataRetriever — pick the
     * lowest disc/track number that's actually on disk.
     */
    @Query("SELECT * FROM tracks WHERE albumId = :albumId AND isResident = 1 ORDER BY discNumber, trackNumber LIMIT 1")
    suspend fun firstResidentByAlbum(albumId: String): TrackEntity?
}
