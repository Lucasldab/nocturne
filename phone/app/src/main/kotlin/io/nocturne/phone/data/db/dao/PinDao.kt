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

    @Query("SELECT COUNT(*) FROM pins")
    suspend fun count(): Int
}
