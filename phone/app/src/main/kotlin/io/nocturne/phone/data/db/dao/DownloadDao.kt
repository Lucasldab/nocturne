package io.nocturne.phone.data.db.dao

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import io.nocturne.phone.data.db.entity.DownloadEntity
import kotlinx.coroutines.flow.Flow

/**
 * DAO for the phone-initiated download request log. See [DownloadEntity] for
 * the state machine. Reads return rows ordered newest-first so the Sync
 * screen's "$ download" section renders the most-recent attempt at the top.
 */
@Dao
interface DownloadDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun upsert(row: DownloadEntity)

    @Query("SELECT * FROM downloads ORDER BY requestedAt DESC LIMIT :limit")
    fun flowRecent(limit: Int = 50): Flow<List<DownloadEntity>>

    @Query("SELECT * FROM downloads WHERE synced = 0 ORDER BY requestedAt ASC")
    suspend fun unsyncedList(): List<DownloadEntity>

    @Query("UPDATE downloads SET synced = 1, state = 'queued' WHERE id = :id AND state = 'pending'")
    suspend fun markQueued(id: String)

    /**
     * Apply a status line from the desktop status JSONL. The `ts` gate
     * prevents an older line (e.g. a "running" arriving after a "done") from
     * stomping the final state — LWW on statusUpdatedAt.
     */
    @Query(
        "UPDATE downloads SET state = :state, message = :message, " +
            "statusUpdatedAt = :ts WHERE id = :id AND :ts >= statusUpdatedAt",
    )
    suspend fun updateStatus(id: String, state: String, message: String?, ts: Long)

    @Query("SELECT * FROM downloads WHERE id = :id")
    suspend fun byId(id: String): DownloadEntity?
}
