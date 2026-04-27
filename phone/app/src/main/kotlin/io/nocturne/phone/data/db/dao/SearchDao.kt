package io.nocturne.phone.data.db.dao

import androidx.room.Dao
import androidx.room.Query
import io.nocturne.phone.data.db.entity.TrackEntity

@Dao
interface SearchDao {
    /**
     * FTS4 typeahead. Caller is responsible for:
     * - accent-folding the query (use the same algorithm as
     *   CatalogImporter — Plan 04-06 owns the construction).
     * - appending `*` for prefix match (e.g. "cafe*").
     * - stripping or escaping FTS operators on hostile input.
     */
    @Query(
        """
        SELECT tracks.* FROM tracks
        JOIN track_fts ON tracks.rowid = track_fts.rowid
        WHERE track_fts MATCH :ftsQuery
        LIMIT :limit
        """,
    )
    suspend fun typeahead(ftsQuery: String, limit: Int = 50): List<TrackEntity>
}
