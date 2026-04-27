package io.nocturne.phone.data.catalog

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CatalogJsonTest {
    private val json = Json { ignoreUnknownKeys = true; isLenient = false }

    private fun resourceText(name: String): String =
        this::class.java.classLoader!!.getResourceAsStream(name)!!
            .bufferedReader().use { it.readText() }

    @Test fun decodes_tiny_catalog_with_three_tracks() {
        val cat = json.decodeFromString(CatalogJson.serializer(), resourceText("catalog-tiny.json"))
        assertEquals(1, cat.v)
        assertEquals(3, cat.tracks.size)
    }

    @Test fun multi_value_arrays_decode_as_lists() {
        val cat = json.decodeFromString(CatalogJson.serializer(), resourceText("catalog-tiny.json"))
        val daft = cat.tracks.first { it.id.endsWith("003") }
        assertEquals(listOf("Daft Punk", "Pharrell"), daft.artist)
        assertEquals(listOf("Daft Punk"), daft.albumArtist)
        assertEquals(listOf("Electronic"), daft.genre)
    }

    @Test fun missing_optional_fields_decode_as_null() {
        val cat = json.decodeFromString(CatalogJson.serializer(), resourceText("catalog-tiny.json"))
        val plain = cat.tracks.first { it.id.endsWith("002") }
        // duration_ms is explicitly null in fixture
        assertNull(plain.durationMs)
    }

    @Test fun decodes_real_699_catalog_without_throwing() {
        val cat = json.decodeFromString(CatalogJson.serializer(), resourceText("catalog-real-699.json"))
        assertEquals(1, cat.v)
        assertTrue(
            "expected real catalog to have 600..800 tracks, got ${cat.tracks.size}",
            cat.tracks.size in 600..800,
        )
    }

    @Test fun decodes_tiny_manifest_with_one_resident_entry() {
        val m = json.decodeFromString(ManifestJson.serializer(), resourceText("manifest-tiny.json"))
        assertEquals(1, m.v)
        assertEquals(1, m.resident.size)
        assertEquals("0000000000000000000000000000000000000000000000000000000000000001", m.resident[0].id)
        assertEquals(listOf("recent_adds"), m.resident[0].buckets)
    }
}
