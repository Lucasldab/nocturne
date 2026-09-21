package io.nocturne.phone.data.catalog

import io.nocturne.phone.data.db.entity.TrackEntity
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Residency must mean "the audio is on this phone", not "the daemon intends
 * it to be".
 *
 * manifest.json rides the `sync-meta` Syncthing folder (~25 KB, seconds);
 * the FLACs ride `sync-files` (hundreds of MB, minutes to hours). Measured on
 * 2026-09-21: sync-meta 100% complete, sync-files 98.49% — 40 items / 28 MB
 * outstanding. Every track in that gap used to be flagged resident with no
 * file behind it, which put it in shuffle queues (ExoPlayer then threw
 * ERROR_CODE_IO_FILE_NOT_FOUND and PlaybackService auto-skipped it) and
 * rendered it undimmed in the browser.
 */
class ResidencyResolverTest {

    private fun track(id: String, path: String, sizeBytes: Long) = TrackEntity(
        id = id,
        title = "T $id",
        artist = listOf("A"),
        albumArtist = listOf("A"),
        album = "Alb",
        albumId = "alb",
        albumArtistId = "art",
        genre = listOf("G"),
        genreId = "gen",
        trackNumber = 1,
        discNumber = 1,
        year = 2020,
        durationMs = 200_000L,
        sizeBytes = sizeBytes,
        format = "flac",
        mtimeNs = 0L,
        dateAdded = "2026-01-01",
        path = path,
        isResident = false,
        searchBlob = "t $id",
    )

    @Test
    fun `declared track whose file is fully present is resident`() {
        val t = track("a", "resident/A/Alb/01.flac", 1_000L)
        val got = ResidencyResolver.verify(listOf(t)) { 1_000L }
        assertEquals(listOf("a"), got)
    }

    @Test
    fun `declared track whose file has not arrived is NOT resident`() {
        // The bug: manifest says resident, sync-files hasn't delivered it.
        val t = track("b", "resident/A/Alb/02.flac", 1_000L)
        val got = ResidencyResolver.verify(listOf(t)) { null }
        assertEquals(emptyList<String>(), got)
    }

    @Test
    fun `declared track still transferring is NOT resident`() {
        // Syncthing writes a partial file; short bytes must not count.
        val t = track("c", "resident/A/Alb/03.flac", 1_000L)
        val got = ResidencyResolver.verify(listOf(t)) { 640L }
        assertEquals(emptyList<String>(), got)
    }

    @Test
    fun `file larger than catalog size still counts as present`() {
        // Re-tagged on hearth after the catalog was published — bytes differ,
        // the audio is there. Must not disappear from the library.
        val t = track("d", "resident/A/Alb/04.flac", 1_000L)
        val got = ResidencyResolver.verify(listOf(t)) { 1_200L }
        assertEquals(listOf("d"), got)
    }

    @Test
    fun `unknown catalog size falls back to any non-empty file`() {
        val t = track("e", "resident/A/Alb/05.flac", 0L)
        assertEquals(listOf("e"), ResidencyResolver.verify(listOf(t)) { 12L })
        assertEquals(emptyList<String>(), ResidencyResolver.verify(listOf(t)) { 0L })
    }

    @Test
    fun `only the present subset of a declared set is returned`() {
        val declared = listOf(
            track("p1", "resident/A/Alb/01.flac", 100L),
            track("p2", "resident/A/Alb/02.flac", 100L),
            track("p3", "resident/A/Alb/03.flac", 100L),
        )
        val onDisk = mapOf("p1" to 100L, "p3" to 100L)
        val got = ResidencyResolver.verify(declared) { onDisk[it.id] }
        assertEquals(listOf("p1", "p3"), got)
    }

    @Test
    fun `probe failure is treated as absent rather than resident`() {
        // A SAF query that throws must not fail open — failing open is what
        // put unplayable tracks in the shuffle queue in the first place.
        val t = track("f", "resident/A/Alb/06.flac", 100L)
        val got = ResidencyResolver.verify(listOf(t)) { throw RuntimeException("SAF died") }
        assertEquals(emptyList<String>(), got)
    }

    @Test
    fun `empty declared set yields empty result`() {
        assertEquals(emptyList<String>(), ResidencyResolver.verify(emptyList()) { 1L })
    }

    @Test
    fun `music tree relative path strips the residency prefix`() {
        assertEquals("A/Alb/01.flac", ResidencyResolver.relPath("resident/A/Alb/01.flac"))
        assertEquals("A/Alb/01.flac", ResidencyResolver.relPath("archive/A/Alb/01.flac"))
        // Already relative (defensive — catalog rows should carry a prefix).
        assertEquals("A/Alb/01.flac", ResidencyResolver.relPath("A/Alb/01.flac"))
    }

    @Test
    fun `relPath does not strip a prefix occurring mid-path`() {
        assertEquals(
            "Archive/resident/01.flac",
            ResidencyResolver.relPath("resident/Archive/resident/01.flac"),
        )
    }
}
