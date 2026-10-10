package io.nocturne.phone.player

import androidx.media3.common.Player
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.db.knownAccentFixture
import io.nocturne.phone.data.db.synthCatalog
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * PlaybackResumption must hand back shuffle mode, repeat mode and the
 * shuffle play order alongside the queue — the queue alone survived process
 * death (profile switch) while shuffle / repeat were silently dropped.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class PlaybackResumptionTest {

    private lateinit var db: NocturneDatabase

    @Before
    fun setUp() {
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
    fun restoreCarriesShuffleRepeatAndOrder() = runTest {
        val tracks = knownAccentFixture() // all resident
        db.trackDao().insertAll(tracks)
        val saved = SavedQueue(
            mediaIds = tracks.map { it.id },
            currentIndex = 2,
            currentPositionMs = 1234L,
            shuffleMode = true,
            repeatMode = Player.REPEAT_MODE_ALL,
            shuffleOrder = listOf(3, 0, 4, 2, 1),
        )

        val r = PlaybackResumption.restore(saved, db.trackDao())

        assertTrue(r.shuffleMode)
        assertEquals(Player.REPEAT_MODE_ALL, r.repeatMode)
        assertEquals(tracks.map { it.id }, r.mediaIds)
        assertEquals(2, r.items.startIndex)
        assertEquals(1234L, r.items.startPositionMs)
        assertArrayEquals(intArrayOf(3, 0, 4, 2, 1), r.shuffleOrder)
    }

    @Test
    fun restoreRemapsOrderAroundDroppedTracks() = runTest {
        val resident = knownAccentFixture().take(3)
        val gone = synthCatalog(2)[1] // i % 3 != 0 -> not resident
        db.trackDao().insertAll(resident + gone)
        // Queue: [r0, gone, r1, r2]; shuffle walks r2, gone, r0, r1.
        val saved = SavedQueue(
            mediaIds = listOf(resident[0].id, gone.id, resident[1].id, resident[2].id),
            currentIndex = 0,
            shuffleMode = true,
            repeatMode = Player.REPEAT_MODE_ONE,
            shuffleOrder = listOf(3, 1, 0, 2),
        )

        val r = PlaybackResumption.restore(saved, db.trackDao())

        assertEquals(resident.map { it.id }, r.mediaIds)
        assertEquals(Player.REPEAT_MODE_ONE, r.repeatMode)
        // r2, r0, r1 at their new positions 2, 0, 1.
        assertArrayEquals(intArrayOf(2, 0, 1), r.shuffleOrder)
    }

    @Test
    fun restoreWithoutSavedOrderLeavesOrderUnset() = runTest {
        val tracks = knownAccentFixture()
        db.trackDao().insertAll(tracks)
        val saved = SavedQueue(mediaIds = tracks.map { it.id }, shuffleMode = true)

        val r = PlaybackResumption.restore(saved, db.trackDao())

        assertTrue(r.shuffleMode)
        assertNull(r.shuffleOrder)
    }

    @Test
    fun remapRejectsNonPermutations() {
        val kept = listOf(0, 1, 2)
        assertNull(PlaybackResumption.remapShuffleOrder(listOf(0, 1), 3, kept))
        assertNull(PlaybackResumption.remapShuffleOrder(listOf(0, 0, 1), 3, kept))
        assertNull(PlaybackResumption.remapShuffleOrder(listOf(0, 1, 3), 3, kept))
        assertNull(PlaybackResumption.remapShuffleOrder(emptyList(), 0, emptyList()))
    }
}
