package io.nocturne.phone.ui.system

import io.nocturne.phone.data.catalog.ManifestJson
import io.nocturne.phone.data.catalog.ResidentEntry
import io.nocturne.phone.data.db.entity.TrackEntity
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Quick task 260428-7zc — RotationAggregator unit tests.
 *
 * Pure JVM. No Android, no Robolectric. Validates the bucket-roll-up that
 * the System / Rotation screen consumes (joins manifest.json residents with
 * Room TrackEntity sizeBytes).
 */
class RotationAggregatorTest {
    private fun track(id: String, bytes: Long): TrackEntity =
        TrackEntity(
            id = id,
            title = "t-$id",
            artist = listOf("a"),
            albumArtist = listOf("a"),
            album = "alb",
            albumId = "alb-id",
            albumArtistId = "a-id",
            genre = emptyList(),
            genreId = null,
            trackNumber = null,
            discNumber = null,
            year = null,
            durationMs = null,
            sizeBytes = bytes,
            format = "flac",
            mtimeNs = 0L,
            dateAdded = "2026-01-01",
            path = "p/$id.flac",
            isResident = true,
            searchBlob = id,
        )

    @Test
    fun empty_manifest_yields_empty_view() {
        val v = RotationAggregator.aggregate(null, emptyMap())
        assertTrue(v.buckets.isEmpty())
        assertEquals(0L, v.capBytes)
        assertEquals(0L, v.totalUsedBytes)
        assertNull(v.generatedAt)
    }

    @Test
    fun three_residents_canonical_order_and_byte_rollup() {
        val tracks = mapOf(
            "id1" to track("id1", 1_000_000L),
            "id2" to track("id2", 2_000_000L),
            "id3" to track("id3", 3_000_000L),
        )
        val manifest = ManifestJson(
            v = 1,
            generatedAt = "2026-04-28T09:14:00Z",
            capBytes = 12_000_000_000L,
            usedBytes = 6_000_000L,
            resident = listOf(
                ResidentEntry("id1", listOf("recent_adds")),
                ResidentEntry("id2", listOf("loved", "top_played")),
                ResidentEntry("id3", listOf("recent_adds", "manual_pins")),
            ),
        )
        val v = RotationAggregator.aggregate(manifest, tracks)
        // Canonical order: recent_adds, top_played, recent_plays, loved, exploration, manual_pins.
        // Only buckets that appear should be listed (recent_plays / exploration are absent).
        assertEquals(
            listOf("recent_adds", "top_played", "loved", "manual_pins"),
            v.buckets.map { it.id },
        )
        // recent_adds: id1 + id3 = 2 tracks, 1M + 3M = 4_000_000 bytes
        val recentAdds = v.buckets.first { it.id == "recent_adds" }
        assertEquals(2, recentAdds.count)
        assertEquals(4_000_000L, recentAdds.bytes)
        // top_played: id2 only
        val topPlayed = v.buckets.first { it.id == "top_played" }
        assertEquals(1, topPlayed.count)
        assertEquals(2_000_000L, topPlayed.bytes)
        // loved: id2 only
        val loved = v.buckets.first { it.id == "loved" }
        assertEquals(1, loved.count)
        assertEquals(2_000_000L, loved.bytes)
        // manual_pins: id3 only
        val pins = v.buckets.first { it.id == "manual_pins" }
        assertEquals(1, pins.count)
        assertEquals(3_000_000L, pins.bytes)
        // Totals come from manifest, not summed locally.
        assertEquals(6_000_000L, v.totalUsedBytes)
        assertEquals(12_000_000_000L, v.capBytes)
        assertEquals("2026-04-28T09:14:00Z", v.generatedAt)
    }

    @Test
    fun missing_track_in_map_counts_but_contributes_zero_bytes() {
        // id1 present in tracks map; id2 absent.
        val tracks = mapOf("id1" to track("id1", 1_000L))
        val manifest = ManifestJson(
            v = 1,
            generatedAt = "2026-04-28T00:00:00Z",
            capBytes = 100L,
            usedBytes = 50L,
            resident = listOf(
                ResidentEntry("id1", listOf("recent_adds")),
                ResidentEntry("id2", listOf("recent_adds")),
            ),
        )
        val v = RotationAggregator.aggregate(manifest, tracks)
        val recent = v.buckets.first { it.id == "recent_adds" }
        assertEquals(2, recent.count)             // both counted
        assertEquals(1_000L, recent.bytes)        // only id1's bytes
        assertEquals(50L, v.totalUsedBytes)        // manifest authoritative
    }

    @Test
    fun unknown_bucket_appended_after_canonical_with_raw_label() {
        val tracks = mapOf("id1" to track("id1", 100L), "id2" to track("id2", 200L))
        val manifest = ManifestJson(
            v = 1,
            generatedAt = "2026-04-28T00:00:00Z",
            capBytes = 1_000L,
            usedBytes = 300L,
            resident = listOf(
                ResidentEntry("id1", listOf("recent_adds")),
                ResidentEntry("id2", listOf("weird_bucket")),
            ),
        )
        val v = RotationAggregator.aggregate(manifest, tracks)
        // Canonical first, unknown last.
        assertEquals(listOf("recent_adds", "weird_bucket"), v.buckets.map { it.id })
        val weird = v.buckets.first { it.id == "weird_bucket" }
        assertEquals("weird_bucket", weird.label)  // raw string
        assertEquals(1, weird.count)
        assertEquals(200L, weird.bytes)
        // Sanity: canonical ones still get human labels.
        val recent = v.buckets.first { it.id == "recent_adds" }
        assertNotNull(recent.label)
        assertEquals("recent adds", recent.label)
    }
}
