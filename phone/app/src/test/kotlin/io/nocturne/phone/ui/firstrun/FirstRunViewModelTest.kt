package io.nocturne.phone.ui.firstrun

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.catalog.CatalogSource
import io.nocturne.phone.data.db.NocturneDatabase
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config
import java.io.ByteArrayInputStream
import java.io.IOException
import java.io.InputStream

@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class FirstRunViewModelTest {
    private lateinit var ctx: Context
    private lateinit var db: NocturneDatabase
    private lateinit var container: AppContainer
    private lateinit var vm: FirstRunViewModel

    @Before
    fun setup() {
        ctx = ApplicationProvider.getApplicationContext()
        db = Room.inMemoryDatabaseBuilder(ctx, NocturneDatabase::class.java).allowMainThreadQueries().build()
        container = AppContainer(ctx, dbOverride = db)
        vm = FirstRunViewModel(container)
    }

    @After
    fun tearDown() {
        db.close()
    }

    private class FakeCatalogSource(
        private val catalogBytes: ByteArray,
        private val manifestBytes: ByteArray? = null,
        private val throwOnOpen: Boolean = false,
    ) : CatalogSource {
        override fun openCatalog(): InputStream {
            if (throwOnOpen) throw IOException("synthetic open failure")
            return ByteArrayInputStream(catalogBytes)
        }

        override fun openManifest(): InputStream? = manifestBytes?.let(::ByteArrayInputStream)
    }

    private fun loadResource(name: String): ByteArray =
        this::class.java.classLoader!!.getResourceAsStream(name)!!.use { it.readBytes() }

    @Test
    fun initialStateIsNotStarted() {
        assertEquals(ImportState.NotStarted, vm.state.value)
    }

    @Test
    fun importingTinyFixtureEndsAtSucceeded() = runTest {
        val src = FakeCatalogSource(
            catalogBytes = loadResource("catalog-tiny.json"),
            manifestBytes = loadResource("manifest-tiny.json"),
        )
        vm.importFromForTest(src)
        val final = vm.state.value
        assertTrue("expected Succeeded, got $final", final is ImportState.Succeeded)
        val result = (final as ImportState.Succeeded).result
        assertEquals(3, result.tracksImported)
        assertEquals(1, result.residentMarked)
        assertEquals(3, db.trackDao().count())
    }

    @Test
    fun importerThrownExceptionTransitionsToFailed() = runTest {
        val src = FakeCatalogSource(
            catalogBytes = ByteArray(0),
            throwOnOpen = true,
        )
        vm.importFromForTest(src)
        val final = vm.state.value
        assertTrue("expected Failed, got $final", final is ImportState.Failed)
        val msg = (final as ImportState.Failed).message
        assertTrue("expected message to mention IOException, got '$msg'", msg.contains("IOException"))
    }

    @Test
    fun importingWithoutManifestSucceeds_residentMarkedIsMinusOne() = runTest {
        val src = FakeCatalogSource(
            catalogBytes = loadResource("catalog-tiny.json"),
            manifestBytes = null,
        )
        vm.importFromForTest(src)
        val final = vm.state.value
        assertTrue("expected Succeeded, got $final", final is ImportState.Succeeded)
        assertEquals(-1, (final as ImportState.Succeeded).result.residentMarked)
    }
}
