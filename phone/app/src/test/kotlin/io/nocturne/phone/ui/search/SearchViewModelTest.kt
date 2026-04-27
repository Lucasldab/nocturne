package io.nocturne.phone.ui.search

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.db.knownAccentFixture
import io.nocturne.phone.data.db.sha256Hex
import io.nocturne.phone.data.db.synthCatalog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

/**
 * SearchViewModel uses Room's suspend DAOs which run on Room's internal
 * transaction executor. Virtual-time runTest does not wait for that real
 * async work, so the tests below use runBlocking + a flow predicate
 * `first { it is Terminal }` that waits for actual state propagation.
 */
@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class SearchViewModelTest {
    private lateinit var db: NocturneDatabase
    private lateinit var container: AppContainer
    private lateinit var vm: SearchViewModel

    @Before
    fun setup() {
        Dispatchers.setMain(Dispatchers.Unconfined)
        val ctx = ApplicationProvider.getApplicationContext<Context>()
        db = Room.inMemoryDatabaseBuilder(ctx, NocturneDatabase::class.java)
            .allowMainThreadQueries()
            .build()
        container = AppContainer(ctx, dbOverride = db)
        vm = SearchViewModel(container)
    }

    @After
    fun tearDown() {
        db.close()
        Dispatchers.resetMain()
    }

    @Test
    fun emptyDb_queryEmitsEmpty() = runBlocking {
        vm.onQueryChange("cafe")
        // Skip the initial Idle, then wait for terminal after debounce + query.
        val state = withTimeout(5_000) {
            vm.results.first { it is SearchResult.Empty || it is SearchResult.Results }
        }
        assertEquals(SearchResult.Empty, state)
    }

    @Test
    fun synth15k_caféRock_returnsCappedRows() = runBlocking {
        db.trackDao().insertAll(synthCatalog(15_000))
        vm.onQueryChange("café rock")
        val state = withTimeout(10_000) {
            vm.results.first { it is SearchResult.Results || it is SearchResult.Empty }
        }
        assertTrue("expected Results, got $state", state is SearchResult.Results)
        // synthCatalog tags 1/5 of 15k rows with genre "Café Rock"; LIMIT 50
        // caps result count.
        assertEquals(50, (state as SearchResult.Results).items.size)
    }

    @Test
    fun knownAccentFixture_caféOrange_returnsKnownId() = runBlocking {
        db.trackDao().insertAll(knownAccentFixture())
        vm.onQueryChange("café orange")
        val state = withTimeout(5_000) {
            vm.results.first { it is SearchResult.Results || it is SearchResult.Empty }
        }
        assertTrue("expected Results, got $state", state is SearchResult.Results)
        val knownId = sha256Hex("known-cafe-orange")
        assertTrue(
            "expected known-cafe-orange row in results",
            (state as SearchResult.Results).items.any { it.id == knownId },
        )
    }

    @Test
    fun rapidInput_collectLatestKeepsOnlyFinalQuery() = runBlocking {
        db.trackDao().insertAll(knownAccentFixture())
        // Rapid keystrokes inside debounce window — collectLatest cancels the
        // intermediate flow values; only "cafe" should drive a DB hit.
        vm.onQueryChange("c")
        vm.onQueryChange("ca")
        vm.onQueryChange("caf")
        vm.onQueryChange("cafe")
        val state = withTimeout(5_000) {
            vm.results.first { it is SearchResult.Results || it is SearchResult.Empty }
        }
        assertTrue("expected Results from final 'cafe', got $state", state is SearchResult.Results)
    }

    @Test
    fun allOperatorsInput_emitsIdleAfterDebounce() = runBlocking {
        // Initial state is Idle; build returns null for "***", so we expect
        // Idle to remain. Wait long enough for the debounce to fire then
        // assert state hasn't moved off Idle.
        vm.onQueryChange("***")
        // 250ms is well past the 120ms debounce; if the pipeline emits
        // anything other than Idle, we'd see it here.
        kotlinx.coroutines.delay(250)
        assertEquals(SearchResult.Idle, vm.results.value)
    }

    @Test
    fun clearResetsState() = runBlocking {
        db.trackDao().insertAll(knownAccentFixture())
        vm.onQueryChange("cafe")
        withTimeout(5_000) {
            vm.results.first { it is SearchResult.Results || it is SearchResult.Empty }
        }
        vm.clear()
        // clear() synchronously resets both StateFlows.
        assertEquals("", vm.query.value)
        assertEquals(SearchResult.Idle, vm.results.value)
    }
}
