package io.nocturne.phone.data.db

import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class SearchDaoTest {
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
    fun typeaheadFindsAccentFoldedTitle() = runTest {
        // The known-accent fixture inserts five rows; "café orange" folds to
        // "cafe orange" in searchBlob. Querying "cafe*" must return it.
        db.trackDao().insertAll(knownAccentFixture())
        val hits = db.searchDao().typeahead("cafe*")
        val titles = hits.map { it.title }.toSet()
        assertTrue(
            "expected typeahead('cafe*') to surface accented row, got: $titles",
            "café orange" in titles,
        )
    }

    @Test
    fun typeaheadOnFifteenKWallClockUnderThreshold() = runTest {
        val rows = synthCatalog(15_000) + knownAccentFixture()
        db.trackDao().insertAll(rows)
        // Warm the FTS engine; the first MATCH primes pages.
        db.searchDao().typeahead("track*", limit = 1)
        val start = System.nanoTime()
        val hits = db.searchDao().typeahead("cafe*", limit = 50)
        val elapsedMs = (System.nanoTime() - start) / 1_000_000
        // synthCatalog has genre "Café Rock" (-> "cafe rock" in blob) on every
        // 5th row. With 15k tracks there are 3000 such rows; we expect at
        // least 1 hit (limit=50 caps the result count).
        assertTrue("expected at least one cafe* hit, got ${hits.size}", hits.isNotEmpty())
        // Soft gate (Robolectric JVM tests run slower than ART; the plan
        // documents <100ms target with relaxed <500ms hard fail).
        if (elapsedMs > 100) {
            println("[SearchDaoTest] typeahead 15k cafe*: ${elapsedMs}ms (>100ms soft target, <500ms hard)")
        }
        assertTrue("typeahead too slow on 15k corpus: ${elapsedMs}ms", elapsedMs < 500)
    }

    @Test
    fun typeaheadMultiTokenAndSemantics() = runTest {
        db.trackDao().insertAll(knownAccentFixture())
        // FTS4 implicit AND between tokens. "cafe rock*" must match
        // rows whose searchBlob contains BOTH "cafe" and a "rock"-prefix
        // token. Of the fixture, "Café Bar / Café Rock Band / Café Rock
        // Album / Café Rock" satisfies both.
        val hits = db.searchDao().typeahead("cafe rock*")
        val ids = hits.map { it.title }.toSet()
        assertTrue("expected 'Café Bar' in multi-token hits, got: $ids", "Café Bar" in ids)
        // "café orange / Sigur Rós / Ágætis byrjun / Post-Rock" — "post-rock"
        // tokenizes to "post" and "rock" (unicode61 splits on hyphen). It
        // should also match because "rock" is a token prefix and "cafe"
        // appears in "café orange" -> blob "cafe orange ...". So expect
        // the "café orange" row too.
        assertTrue(
            "expected typeahead('cafe rock*') to include accent-folded matches",
            hits.any { it.title.contains("café", ignoreCase = true) },
        )
        // The plain non-accented row (no "cafe" token) must NOT appear.
        val plainTitles = hits.map { it.title }
        assertEquals(false, "Plain Song" in plainTitles)
    }
}
