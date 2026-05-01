package io.nocturne.phone.data.db.dao

import androidx.paging.PagingSource
import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import io.nocturne.phone.data.db.LetterAnchor
import io.nocturne.phone.data.db.entity.ArtistEntity

@Dao
interface ArtistDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertAll(artists: List<ArtistEntity>)

    @Query("DELETE FROM artists")
    suspend fun deleteAll()

    @Query("SELECT * FROM artists ORDER BY name COLLATE NOCASE")
    fun pagedAll(): PagingSource<Int, ArtistEntity>

    /**
     * Non-paged full artist list. Same ORDER BY as [pagedAll] so the
     * letter-rail's [letterFirstIndex] row indices line up byte-for-byte.
     * Used by [io.nocturne.phone.ui.browser.BrowserViewModel.artistsAll] for
     * the ArtistsScreen LazyColumn — Pager+scrollToItem cannot reach indices
     * outside the loaded window (quick task 260430-vtb Bug 1).
     */
    @Query("SELECT * FROM artists ORDER BY name COLLATE NOCASE")
    suspend fun listAll(): List<ArtistEntity>

    @Query("SELECT * FROM artists WHERE id = :id")
    suspend fun byId(id: String): ArtistEntity?

    @Query("SELECT COUNT(*) FROM artists")
    suspend fun count(): Int

    /**
     * Letter-rail anchors over `pagedAll()`'s ORDER BY name COLLATE NOCASE
     * listing. See [io.nocturne.phone.data.db.dao.AlbumDao.letterFirstIndex]
     * for the bucketing contract.
     */
    @Query(
        """
        SELECT letter, MIN(rn) AS rowIndex FROM (
          SELECT
            CASE
              WHEN UPPER(SUBSTR(name, 1, 1)) BETWEEN 'A' AND 'Z'
                THEN UPPER(SUBSTR(name, 1, 1))
              ELSE '#'
            END AS letter,
            (ROW_NUMBER() OVER (ORDER BY name COLLATE NOCASE)) - 1 AS rn
          FROM artists
        )
        GROUP BY letter
        """,
    )
    suspend fun letterFirstIndex(): List<LetterAnchor>
}
