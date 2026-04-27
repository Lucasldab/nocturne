package io.nocturne.phone.data.catalog

import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import io.nocturne.phone.data.db.NocturneDatabase
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config
import java.io.ByteArrayInputStream
import java.io.InputStream

@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class CatalogImporterTest {
    private lateinit var db: NocturneDatabase
    private lateinit var importer: CatalogImporter

    @Before
    fun setup() {
        db = Room.inMemoryDatabaseBuilder(
            ApplicationProvider.getApplicationContext(),
            NocturneDatabase::class.java,
        ).allowMainThreadQueries().build()
        importer = CatalogImporter(db)
    }

    @After
    fun tearDown() {
        db.close()
    }

    private fun resourceStream(name: String): InputStream =
        this::class.java.classLoader!!.getResourceAsStream(name)
            ?: error("test resource not found: $name")

    private fun resourceBytes(name: String): ByteArray =
        resourceStream(name).use { it.readBytes() }

    private fun openCat(name: String): InputStream = ByteArrayInputStream(resourceBytes(name))

    @Test
    fun tinyCatalogImportsAllThreeTracks() = runTest {
        val result = importer.importAll(openCat("catalog-tiny.json"), null)
        assertEquals(3, result.tracksImported)
        assertEquals(3, db.trackDao().count())
        assertEquals(-1, result.residentMarked) // manifest absent
    }

    @Test
    fun tinyCatalogDerivesThreeAlbums() = runTest {
        importer.importAll(openCat("catalog-tiny.json"), null)
        assertEquals(3, db.albumDao().count())
    }

    @Test
    fun tinyCatalogDerivesThreeArtists() = runTest {
        importer.importAll(openCat("catalog-tiny.json"), null)
        // Sigur Rós, ASCII, Daft Punk (album_artist groups)
        assertEquals(3, db.artistDao().count())
    }

    @Test
    fun tinyCatalogDerivesThreeGenres() = runTest {
        importer.importAll(openCat("catalog-tiny.json"), null)
        assertEquals(3, db.genreDao().count())
    }

    @Test
    fun tinyManifestMarksOnlyOneResident() = runTest {
        val result = importer.importAll(
            openCat("catalog-tiny.json"),
            openCat("manifest-tiny.json"),
        )
        assertEquals(1, result.residentMarked)
        val first = db.trackDao().byId("0".repeat(63) + "1")
        assertNotNull(first)
        assertEquals(true, first!!.isResident)
        val second = db.trackDao().byId("0".repeat(63) + "2")
        assertNotNull(second)
        assertEquals(false, second!!.isResident)
    }

    @Test
    fun withoutManifestAllTracksAreNonResident() = runTest {
        importer.importAll(openCat("catalog-tiny.json"), null)
        // The path-prefix "resident/" is informational only; manifest is the
        // authority on residency.
        for (i in 1..3) {
            val r = db.trackDao().byId("0".repeat(63) + "$i")
            assertNotNull(r)
            assertEquals(false, r!!.isResident)
        }
    }

    @Test
    fun searchBlobIsAccentFoldedOnInsert() = runTest {
        importer.importAll(openCat("catalog-tiny.json"), null)
        val first = db.trackDao().byId("0".repeat(63) + "1")
        assertNotNull(first)
        // "Café Bar / Sigur Rós / Ágætis byrjun / Sigur Rós / Post-Rock" →
        // accent-folded blob must contain "cafe bar" and "sigur ros".
        val blob = first!!.searchBlob
        assertTrue("blob missing 'cafe bar': $blob", blob.contains("cafe bar"))
        assertTrue("blob missing 'sigur ros': $blob", blob.contains("sigur ros"))
    }

    @Test
    fun realCatalogImportsBetween600And800Tracks() = runTest {
        val result = importer.importAll(
            openCat("catalog-real-699.json"),
            openCat("manifest-real-699.json"),
        )
        assertTrue(
            "expected 600..800 tracks, got ${result.tracksImported}",
            result.tracksImported in 600..800,
        )
        // Real fixture asserts a known sha256 from the catalog (track 0 in
        // the file: "Three Days Grace / Somebody That I Used to Know").
        val known = db.trackDao().byId("0087b905582fe9b487725b1aa4d2b556f01180f8bf1256b3edb606d87e4d384a")
        assertNotNull("expected known track id present after real import", known)
        assertEquals("Somebody That I Used to Know", known!!.title)
    }

    @Test
    fun reImportIsIdempotent() = runTest {
        importer.importAll(openCat("catalog-tiny.json"), openCat("manifest-tiny.json"))
        val countAfterFirst = db.trackDao().count()
        val albumsAfterFirst = db.albumDao().count()
        val artistsAfterFirst = db.artistDao().count()
        importer.importAll(openCat("catalog-tiny.json"), openCat("manifest-tiny.json"))
        assertEquals(countAfterFirst, db.trackDao().count())
        assertEquals(albumsAfterFirst, db.albumDao().count())
        assertEquals(artistsAfterFirst, db.artistDao().count())
    }

    @Test
    fun progressCallbackFiresForEachStage() = runTest {
        val seen = java.util.EnumSet.noneOf(Stage::class.java)
        importer.importAll(openCat("catalog-tiny.json"), null) { stage, _, _ ->
            seen.add(stage)
        }
        assertTrue("PARSING_CATALOG missing: $seen", Stage.PARSING_CATALOG in seen)
        assertTrue("INSERTING_TRACKS missing: $seen", Stage.INSERTING_TRACKS in seen)
        assertTrue("DERIVING_GROUPS missing: $seen", Stage.DERIVING_GROUPS in seen)
        assertTrue("INSERTING_GROUPS missing: $seen", Stage.INSERTING_GROUPS in seen)
        assertTrue("APPLYING_MANIFEST missing: $seen", Stage.APPLYING_MANIFEST in seen)
        assertTrue("DONE missing: $seen", Stage.DONE in seen)
    }
}
