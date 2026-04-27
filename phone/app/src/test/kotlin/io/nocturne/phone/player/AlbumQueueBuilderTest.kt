package io.nocturne.phone.player

import io.nocturne.phone.data.db.entity.TrackEntity
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class AlbumQueueBuilderTest {

    private fun t(id: String, n: Int): TrackEntity = TrackEntity(
        id = id, title = "T$n", artist = listOf("A"), albumArtist = listOf("A"),
        album = "Alb", albumId = "ab", albumArtistId = "aa",
        genre = listOf(), genreId = null,
        trackNumber = n, discNumber = 1, year = 2024, durationMs = 1000L,
        sizeBytes = 1L, format = "flac", mtimeNs = 0L, dateAdded = "x",
        path = "/p/$id.flac", isResident = true, searchBlob = "t",
    )

    @Test fun startIndexZeroForFirstTrack() {
        val tracks = listOf(t("a", 1), t("b", 2), t("c", 3))
        val (items, idx) = AlbumQueueBuilder.buildFromTrack(tracks, tracks[0])
        check(items.size == 3) { "expected 3 items, got ${items.size}" }
        check(idx == 0) { "expected startIndex=0 for first track, got $idx" }
    }

    @Test fun startIndexCorrectForMidAlbumTrack() {
        val tracks = listOf(t("a", 1), t("b", 2), t("c", 3), t("d", 4))
        val (_, idx) = AlbumQueueBuilder.buildFromTrack(tracks, tracks[2])
        check(idx == 2) { "expected 2, got $idx" }
    }

    @Test fun startIndexClampsToZeroWhenStartTrackNotInList() {
        val tracks = listOf(t("a", 1), t("b", 2))
        val absent = t("z", 99)
        val (_, idx) = AlbumQueueBuilder.buildFromTrack(tracks, absent)
        check(idx == 0) { "expected clamped 0, got $idx" }
    }

    @Test fun outputListPreservesInputOrder() {
        val tracks = listOf(t("a", 1), t("b", 2), t("c", 3))
        val (items, _) = AlbumQueueBuilder.buildFromTrack(tracks, tracks[0])
        check(items.map { it.mediaId } == listOf("a", "b", "c")) {
            "expected [a,b,c] got ${items.map { it.mediaId }}"
        }
    }
}
