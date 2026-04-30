package io.nocturne.phone.ui.browser

import io.nocturne.phone.data.db.entity.TrackEntity
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Quick task 260430-s5u — TrackSorter unit tests (pure JVM).
 *
 * Exercises every sort mode (Alphabetical / MostListened / RecentlyListened
 * / RecentlyDownloaded) plus the empty-input contract and the deterministic
 * tiebreak chain (lowercase title is the final tie-breaker for every mode).
 */
class TrackSorterTest {

    /**
     * Minimal TrackEntity factory. The non-sort-relevant fields are zeroed /
     * empty. Tests must NOT rely on equality of the elided fields.
     */
    private fun mkTrack(
        id: String,
        title: String,
        dateAdded: String = "",
    ): TrackEntity = TrackEntity(
        id = id,
        title = title,
        artist = emptyList(),
        albumArtist = emptyList(),
        album = "",
        albumId = "",
        albumArtistId = "",
        genre = emptyList(),
        genreId = null,
        trackNumber = null,
        discNumber = null,
        year = null,
        durationMs = null,
        sizeBytes = 0L,
        format = "",
        mtimeNs = 0L,
        dateAdded = dateAdded,
        path = "",
        isResident = false,
        searchBlob = "",
    )

    @Test
    fun empty_input_yields_empty_output_for_every_mode() {
        TrackSortMode.values().forEach { mode ->
            assertTrue(
                "mode=$mode",
                TrackSorter.sort(emptyList(), mode).isEmpty(),
            )
        }
    }

    @Test
    fun alphabetical_is_case_insensitive_ascending() {
        val tracks = listOf(
            mkTrack("1", "banana"),
            mkTrack("2", "Apple"),
            mkTrack("3", "cherry"),
        )
        val sorted = TrackSorter.sort(tracks, TrackSortMode.Alphabetical)
        assertEquals(listOf("Apple", "banana", "cherry"), sorted.map { it.title })
    }

    @Test
    fun most_listened_orders_by_play_count_desc_with_missing_default_zero() {
        val a = mkTrack("a", "Alpha")
        val b = mkTrack("b", "Bravo")
        val c = mkTrack("c", "Charlie")
        val plays = mapOf("a" to 5, "b" to 2)  // c missing -> 0
        val sorted = TrackSorter.sort(
            tracks = listOf(c, a, b),
            mode = TrackSortMode.MostListened,
            perTrackPlays = plays,
        )
        assertEquals(listOf("a", "b", "c"), sorted.map { it.id })
    }

    @Test
    fun most_listened_tiebreak_falls_through_to_last_ts_then_title() {
        // A and B both have 3 plays; B was played more recently -> B first.
        val a = mkTrack("a", "Alpha")
        val b = mkTrack("b", "Bravo")
        val plays = mapOf("a" to 3, "b" to 3)
        val lastTs = mapOf("a" to 1000L, "b" to 2000L)
        val sorted = TrackSorter.sort(
            tracks = listOf(a, b),
            mode = TrackSortMode.MostListened,
            perTrackPlays = plays,
            perTrackLastTs = lastTs,
        )
        assertEquals(listOf("b", "a"), sorted.map { it.id })
    }

    @Test
    fun most_listened_full_tiebreak_chain_uses_title_when_plays_and_ts_tied() {
        // Same play count, same lastTs -> ascending title.lowercase.
        val a = mkTrack("a", "Zulu")
        val b = mkTrack("b", "Alpha")
        val plays = mapOf("a" to 3, "b" to 3)
        val lastTs = mapOf("a" to 1000L, "b" to 1000L)
        val sorted = TrackSorter.sort(
            tracks = listOf(a, b),
            mode = TrackSortMode.MostListened,
            perTrackPlays = plays,
            perTrackLastTs = lastTs,
        )
        assertEquals(listOf("b", "a"), sorted.map { it.id })
    }

    @Test
    fun recently_listened_orders_by_last_ts_desc_with_missing_at_bottom() {
        val a = mkTrack("a", "Alpha")
        val b = mkTrack("b", "Bravo")
        val c = mkTrack("c", "Charlie")
        val lastTs = mapOf("a" to 300L, "b" to 100L)  // c missing -> last
        val sorted = TrackSorter.sort(
            tracks = listOf(c, a, b),
            mode = TrackSortMode.RecentlyListened,
            perTrackLastTs = lastTs,
        )
        assertEquals(listOf("a", "b", "c"), sorted.map { it.id })
    }

    @Test
    fun recently_downloaded_orders_by_date_added_desc_with_alpha_tiebreak() {
        val newA = mkTrack("1", "Zulu", dateAdded = "2026-04-30")
        val newB = mkTrack("2", "Alpha", dateAdded = "2026-04-30")
        val older = mkTrack("3", "Mike", dateAdded = "2026-04-15")
        val sorted = TrackSorter.sort(
            tracks = listOf(newA, older, newB),
            mode = TrackSortMode.RecentlyDownloaded,
        )
        // Both 2026-04-30 first (alpha tiebreak: Alpha then Zulu), then 2026-04-15.
        assertEquals(listOf("Alpha", "Zulu", "Mike"), sorted.map { it.title })
    }

    @Test
    fun persisted_keys_round_trip_through_from_persisted_key() {
        TrackSortMode.values().forEach { mode ->
            assertEquals(mode, TrackSortMode.fromPersistedKey(mode.persistedKey))
        }
        assertEquals(TrackSortMode.Alphabetical, TrackSortMode.fromPersistedKey(null))
        assertEquals(TrackSortMode.Alphabetical, TrackSortMode.fromPersistedKey("not-a-key"))
    }
}
