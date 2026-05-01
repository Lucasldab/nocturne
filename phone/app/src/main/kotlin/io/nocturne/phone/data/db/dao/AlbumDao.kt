package io.nocturne.phone.data.db.dao

import androidx.paging.PagingSource
import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import io.nocturne.phone.data.db.LetterAnchor
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

    /**
     * Non-paged full album list. Same ORDER BY as [pagedAll] so the
     * letter-rail's [letterFirstIndex] row indices line up byte-for-byte.
     * Used by [io.nocturne.phone.ui.browser.BrowserViewModel.albumsAll] for
     * the AlbumsScreen LazyColumn — Pager+scrollToItem cannot reach indices
     * outside the loaded window (quick task 260430-vtb Bug 1).
     */
    @Query("SELECT * FROM albums ORDER BY title COLLATE NOCASE")
    suspend fun listAll(): List<AlbumEntity>

    /**
     * Room-Flow alphabetical album list. Same ORDER BY as [pagedAll] / [listAll].
     * Used by BrowserViewModel.albumsAll for live re-render on catalog updates
     * (quick task 260430-wt0 Bug 1).
     */
    @Query("SELECT * FROM albums ORDER BY title COLLATE NOCASE")
    fun flowAll(): Flow<List<AlbumEntity>>

    @Query("SELECT * FROM albums WHERE id = :id")
    suspend fun byId(id: String): AlbumEntity?

    @Query(
        "SELECT * FROM albums WHERE albumArtistId = :artistId " +
            "ORDER BY year DESC, title COLLATE NOCASE",
    )
    fun albumsByArtist(artistId: String): Flow<List<AlbumEntity>>

    @Query("SELECT COUNT(*) FROM albums")
    suspend fun count(): Int

    /**
     * Letter-rail anchor list. One row per starting-letter bucket, paired
     * with the 0-based row index of the first album under that letter
     * within `pagedAll()`'s ORDER BY title COLLATE NOCASE listing. Buckets
     * non-A..Z starts (digits, accents, punctuation, empty) into '#'.
     *
     * Caller must add `+1` to `rowIndex` if the screen prepends a
     * SectionLabel header item — the DAO is UI-chrome-unaware.
     */
    @Query(
        """
        SELECT letter, MIN(rn) AS rowIndex FROM (
          SELECT
            CASE
              WHEN UPPER(SUBSTR(title, 1, 1)) BETWEEN 'A' AND 'Z'
                THEN UPPER(SUBSTR(title, 1, 1))
              ELSE '#'
            END AS letter,
            (ROW_NUMBER() OVER (ORDER BY title COLLATE NOCASE)) - 1 AS rn
          FROM albums
        )
        GROUP BY letter
        """,
    )
    suspend fun letterFirstIndex(): List<LetterAnchor>
}
