package io.nocturne.phone.data.db.dao

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import io.nocturne.phone.data.db.entity.PinEntity
import kotlinx.coroutines.flow.Flow

@Dao
interface PinDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun upsert(pin: PinEntity)

    @Query("SELECT * FROM pins WHERE synced = 0 ORDER BY pinnedAt ASC")
    fun unsynced(): Flow<List<PinEntity>>

    @Query("SELECT id FROM pins")
    fun allPinnedIds(): Flow<List<String>>

    @Query("SELECT * FROM pins ORDER BY pinnedAt DESC")
    fun flowAllPinned(): Flow<List<PinEntity>>

    @Query("SELECT COUNT(*) FROM pins")
    suspend fun count(): Int

    // --- Phase 6 (STATS-03 / D-17 / D-19) drain methods ---

    /**
     * Snapshot list of unsynced pins (suspend, NOT Flow). Used by PinsWriter
     * to iterate, append-JSONL, and markSynced per row.
     */
    @Query("SELECT * FROM pins WHERE synced = 0 ORDER BY pinnedAt ASC")
    suspend fun unsyncedList(): List<PinEntity>

    @Query("UPDATE pins SET synced = 1 WHERE id = :id")
    suspend fun markSynced(id: String)

    /**
     * Toggle the pin/unpin state. Resets synced=0 and updates pinnedAt so the
     * PinsWriter drains a fresh JSONL line for the tombstone (D-17 / D-18).
     */
    @Query("UPDATE pins SET pinned = :pinned, synced = 0, pinnedAt = :ts WHERE id = :id")
    suspend fun setPinned(id: String, pinned: Boolean, ts: Long)
}
