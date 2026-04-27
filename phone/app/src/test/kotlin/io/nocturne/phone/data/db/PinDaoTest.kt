package io.nocturne.phone.data.db

import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import io.nocturne.phone.data.db.entity.PinEntity
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class PinDaoTest {
    private lateinit var db: NocturneDatabase

    @Before
    fun setUp() {
        val ctx = ApplicationProvider.getApplicationContext<android.content.Context>()
        db = Room.inMemoryDatabaseBuilder(ctx, NocturneDatabase::class.java)
            .addMigrations(MIGRATION_2_3)
            .allowMainThreadQueries()
            .build()
    }

    @After
    fun tearDown() { db.close() }

    @Test
    fun upsertThenAllPinnedIdsContainsId() = runBlocking {
        db.pinDao().upsert(PinEntity(id = "abc", unit = "track", pinnedAt = 1000L))
        val ids = withTimeout(5_000) {
            db.pinDao().allPinnedIds().first { it.contains("abc") }
        }
        check("abc" in ids)
    }

    @Test
    fun upsertReplacesOnConflict() = runBlocking {
        db.pinDao().upsert(PinEntity(id = "abc", unit = "track", pinnedAt = 1000L))
        db.pinDao().upsert(PinEntity(id = "abc", unit = "track", pinnedAt = 2000L))
        check(db.pinDao().count() == 1)
    }

    @Test
    fun unsyncedReturnsOnlySyncedFalse() = runBlocking {
        db.pinDao().upsert(PinEntity(id = "a", unit = "track", pinnedAt = 1000L, synced = false))
        db.pinDao().upsert(PinEntity(id = "b", unit = "track", pinnedAt = 1000L, synced = true))
        val rows = withTimeout(5_000) { db.pinDao().unsynced().first() }
        check(rows.size == 1 && rows.first().id == "a")
    }
}
