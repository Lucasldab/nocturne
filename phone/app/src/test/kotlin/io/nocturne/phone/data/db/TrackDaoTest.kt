package io.nocturne.phone.data.db

import androidx.paging.PagingSource
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class TrackDaoTest {
    private lateinit var db: NocturneDatabase

    @Before
    fun setup() {
        db = Room.inMemoryDatabaseBuilder(
            ApplicationProvider.getApplicationContext(),
            NocturneDatabase::class.java,
        ).allowMainThreadQueries().build()
    }

    @After
    fun tearDown() {
        db.close()
    }

    @Test
    fun insertAndCount() = runTest {
        val rows = synthCatalog(100)
        db.trackDao().insertAll(rows)
        assertEquals(100, db.trackDao().count())
    }

    @Test
    fun byIdReturnsRow() = runTest {
        val rows = synthCatalog(100)
        db.trackDao().insertAll(rows)
        val target = rows[42]
        val fetched = db.trackDao().byId(target.id)
        assertNotNull(fetched)
        assertEquals(target.title, fetched!!.title)
        assertEquals(target.artist, fetched.artist)
        assertEquals(target.album, fetched.album)
        assertEquals(target.searchBlob, fetched.searchBlob)
    }

    @Test
    fun byIdMissReturnsNull() = runTest {
        db.trackDao().insertAll(synthCatalog(10))
        assertNull(db.trackDao().byId("0".repeat(64)))
    }

    @Test
    fun setResidentForFlipsFlag() = runTest {
        val rows = synthCatalog(20)
        db.trackDao().insertAll(rows)
        val targets = listOf(rows[0].id, rows[1].id, rows[2].id)
        // First flip everything off, then flip the targets back on.
        db.trackDao().clearAllResident()
        db.trackDao().setResidentFor(targets, true)
        for (id in targets) {
            val r = db.trackDao().byId(id)
            assertNotNull(r)
            assertTrue("expected resident=true for $id", r!!.isResident)
        }
        // Untargeted row should still be non-resident.
        val untouched = db.trackDao().byId(rows[5].id)
        assertNotNull(untouched)
        assertEquals(false, untouched!!.isResident)
    }

    @Test
    fun clearAllResidentSetsFalse() = runTest {
        val rows = synthCatalog(50)
        db.trackDao().insertAll(rows)
        db.trackDao().clearAllResident()
        // Sample a few rows; all must be non-resident.
        for (i in listOf(0, 3, 6, 9, 12, 15)) {
            val r = db.trackDao().byId(rows[i].id)
            assertNotNull(r)
            assertEquals(false, r!!.isResident)
        }
    }

    @Test
    fun pagedByAlbumReturnsTwelveTracksInOrder() = runTest {
        // synthCatalog packs 12 tracks per album (i / 12).
        val rows = synthCatalog(36)
        db.trackDao().insertAll(rows)
        val albumId = rows[0].albumId
        val source = db.trackDao().pagedByAlbum(albumId)
        val page = source.load(
            PagingSource.LoadParams.Refresh(key = 0, loadSize = 50, placeholdersEnabled = false),
        )
        assertTrue(page is PagingSource.LoadResult.Page)
        val data = (page as PagingSource.LoadResult.Page).data
        assertEquals(12, data.size)
        // disc=1 for all; trackNumber should be 1..12 ascending.
        val nums = data.map { it.trackNumber }
        assertEquals((1..12).toList(), nums)
    }
}
