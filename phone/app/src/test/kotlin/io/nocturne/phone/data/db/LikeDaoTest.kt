package io.nocturne.phone.data.db

import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import io.nocturne.phone.data.db.entity.LikeEntity
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class LikeDaoTest {

    private lateinit var db: NocturneDatabase

    @Before
    fun setUp() {
        val ctx = ApplicationProvider.getApplicationContext<android.content.Context>()
        db = Room.inMemoryDatabaseBuilder(ctx, NocturneDatabase::class.java)
            .addMigrations(MIGRATION_2_3, MIGRATION_3_4)
            .allowMainThreadQueries()
            .build()
    }

    @After
    fun tearDown() {
        db.close()
    }

    @Test
    fun upsert_and_read_back_track_like() = runBlocking {
        db.likeDao().upsert(
            LikeEntity(id = "t1", unit = "track", liked = true, likedAt = 1L),
        )
        val liked = db.likeDao().isLiked("t1", "track").first()
        check(liked == true) { "expected isLiked('t1','track') = true, got $liked" }
    }

    @Test
    fun unliked_tombstone_lwwOverwritesLikedRow() = runBlocking {
        db.likeDao().upsert(LikeEntity(id = "t1", unit = "track", liked = true, likedAt = 1L))
        db.likeDao().upsert(LikeEntity(id = "t1", unit = "track", liked = false, likedAt = 2L))
        check(db.likeDao().isLiked("t1", "track").first() == false)
        check(db.likeDao().count() == 1) { "composite PK should keep one row, not two" }
    }

    @Test
    fun composite_pk_separates_track_and_album_state() = runBlocking {
        db.likeDao().upsert(LikeEntity(id = "x", unit = "track", liked = true, likedAt = 1L))
        db.likeDao().upsert(LikeEntity(id = "x", unit = "album", liked = false, likedAt = 2L))
        check(db.likeDao().isLiked("x", "track").first() == true)
        check(db.likeDao().isLiked("x", "album").first() == false)
    }

    @Test
    fun unsyncedList_and_markSynced_drain_round_trip() = runBlocking {
        db.likeDao().upsert(
            LikeEntity(id = "t1", unit = "track", liked = true, likedAt = 1L, synced = false),
        )
        db.likeDao().upsert(
            LikeEntity(id = "t2", unit = "track", liked = true, likedAt = 2L, synced = false),
        )
        check(db.likeDao().unsyncedList().size == 2)
        db.likeDao().markSynced("t1", "track")
        val remaining = db.likeDao().unsyncedList()
        check(remaining.size == 1 && remaining.first().id == "t2")
    }
}
