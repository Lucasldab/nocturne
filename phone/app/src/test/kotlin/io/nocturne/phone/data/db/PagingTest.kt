package io.nocturne.phone.data.db

import androidx.paging.PagingSource
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class PagingTest {
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
    fun pagedAllRefreshReturnsHundredRows() = runTest {
        db.trackDao().insertAll(synthCatalog(15_000))
        val source = db.trackDao().pagedAll()
        val page = source.load(
            PagingSource.LoadParams.Refresh(key = null, loadSize = 100, placeholdersEnabled = false),
        )
        assertTrue("expected Page", page is PagingSource.LoadResult.Page)
        val data = (page as PagingSource.LoadResult.Page).data
        assertEquals(100, data.size)
        // ORDER BY title COLLATE NOCASE; titles are "Track 00000".."Track 14999",
        // so the first page must be the lexically lowest 100 titles. Synthetic
        // titles are zero-padded so lexical == numeric for this fixture.
        assertEquals("Track 00000", data.first().title)
        assertEquals("Track 00099", data.last().title)
        assertNotNull(page.nextKey)
    }

    @Test
    fun pagedAllAppendReturnsNextHundred() = runTest {
        db.trackDao().insertAll(synthCatalog(15_000))
        val source = db.trackDao().pagedAll()
        val refresh = source.load(
            PagingSource.LoadParams.Refresh(key = null, loadSize = 100, placeholdersEnabled = false),
        ) as PagingSource.LoadResult.Page<Int, *>
        val nextKey = refresh.nextKey ?: error("expected nextKey from initial Refresh")

        val source2 = db.trackDao().pagedAll()
        val append = source2.load(
            PagingSource.LoadParams.Append(key = nextKey, loadSize = 100, placeholdersEnabled = false),
        )
        assertTrue("expected Page", append is PagingSource.LoadResult.Page)
        val data = (append as PagingSource.LoadResult.Page).data
        assertEquals(100, data.size)
        assertEquals("Track 00100", data.first().title)
        assertEquals("Track 00199", data.last().title)
    }
}
