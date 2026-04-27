package io.nocturne.phone.data.db.dao

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import io.nocturne.phone.data.db.entity.LikeEntity
import kotlinx.coroutines.flow.Flow

@Dao
interface LikeDao {

    /**
     * Upsert a like row. Phase 6 always sets `synced = false` on upsert so the
     * LikesWriter drain emits the JSONL tombstone. The composite PK (id, unit)
     * means the same id can have separate track and album like state.
     */
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun upsert(entity: LikeEntity)

    /** Returns the current liked state for a (id, unit) pair, or null if no row exists. */
    @Query("SELECT liked FROM likes WHERE id = :id AND unit = :unit LIMIT 1")
    fun isLiked(id: String, unit: String): Flow<Boolean?>

    @Query("SELECT * FROM likes WHERE synced = 0 ORDER BY likedAt ASC")
    suspend fun unsyncedList(): List<LikeEntity>

    @Query("UPDATE likes SET synced = 1 WHERE id = :id AND unit = :unit")
    suspend fun markSynced(id: String, unit: String)

    @Query("SELECT COUNT(*) FROM likes")
    suspend fun count(): Int
}
