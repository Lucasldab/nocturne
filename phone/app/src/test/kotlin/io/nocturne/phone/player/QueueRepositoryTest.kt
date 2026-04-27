package io.nocturne.phone.player

import android.content.Context
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import androidx.media3.common.Player
import androidx.test.core.app.ApplicationProvider
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.util.UUID

/**
 * Robolectric tests for QueueRepository.
 * Verifies: roundtrip write→read, malformed JSON fallback, version mismatch
 * fallback, immediate emission from observeQueue, and field boundary coverage.
 *
 * Each test creates a QueueRepository with a unique DataStore name so tests
 * run in any order without state bleed (Robolectric reuses the same
 * application instance across tests in a class).
 */
@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class QueueRepositoryTest {

    private lateinit var ctx: Context

    @Before
    fun setUp() {
        ctx = ApplicationProvider.getApplicationContext()
    }

    /** Create an isolated QueueRepository backed by a unique DataStore name. */
    private fun freshRepo(): QueueRepository =
        QueueRepository(ctx, storeName = "test_queue_${UUID.randomUUID().toString().replace('-', '_')}")

    @Test
    fun loadReturnsEmptyOnFreshDataStore() = runTest {
        val result = freshRepo().loadQueue()
        assertEquals(SavedQueue.EMPTY, result)
    }

    @Test
    fun saveThenLoadRoundTrips() = runTest {
        val repo = freshRepo()
        val queue = SavedQueue(
            mediaIds = listOf("a", "b", "c"),
            currentIndex = 1,
            currentPositionMs = 4242L,
            shuffleMode = true,
            repeatMode = Player.REPEAT_MODE_ALL,
        )
        repo.saveQueue(queue)
        val loaded = repo.loadQueue()
        assertEquals(queue.mediaIds, loaded.mediaIds)
        assertEquals(queue.currentIndex, loaded.currentIndex)
        assertEquals(queue.currentPositionMs, loaded.currentPositionMs)
        assertEquals(queue.shuffleMode, loaded.shuffleMode)
        assertEquals(queue.repeatMode, loaded.repeatMode)
        assertEquals(SavedQueue.SCHEMA_VERSION, loaded.schema_version)
    }

    @Test
    fun malformedJsonFallsBackToEmpty() = runTest {
        // Directly write a non-JSON string under the repository's key.
        val repo = freshRepo()
        repo.writeRawForTest("NOT_VALID_JSON{{{")
        val result = repo.loadQueue()
        assertEquals(SavedQueue.EMPTY, result)
    }

    @Test
    fun versionMismatchFallsBackToEmpty() = runTest {
        // Syntactically valid JSON, but schema_version is unknown future version.
        val repo = freshRepo()
        val futureJson = """{"schema_version":999,"mediaIds":["x"],"currentIndex":0,"currentPositionMs":0,"shuffleMode":false,"repeatMode":0}"""
        repo.writeRawForTest(futureJson)
        val result = repo.loadQueue()
        assertEquals(SavedQueue.EMPTY, result)
    }

    @Test
    fun observeEmitsCurrentValueImmediately() = runTest {
        val repo = freshRepo()
        val queue = SavedQueue(mediaIds = listOf("id1"), currentIndex = 0)
        repo.saveQueue(queue)
        val emitted = repo.observeQueue().first()
        assertEquals(listOf("id1"), emitted.mediaIds)
    }

    @Test
    fun repeatModeAndShuffleModePersisted() = runTest {
        val repo = freshRepo()
        val queue = SavedQueue(
            mediaIds = listOf("z"),
            shuffleMode = true,
            repeatMode = Player.REPEAT_MODE_ONE,
        )
        repo.saveQueue(queue)
        val loaded = repo.loadQueue()
        assertTrue(loaded.shuffleMode)
        assertEquals(Player.REPEAT_MODE_ONE, loaded.repeatMode)
        assertFalse(loaded.mediaIds.isEmpty())
    }
}
