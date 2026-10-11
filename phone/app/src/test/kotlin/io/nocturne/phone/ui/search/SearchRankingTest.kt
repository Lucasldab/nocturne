package io.nocturne.phone.ui.search

import io.nocturne.phone.data.db.entity.TrackEntity
import org.junit.Assert.assertEquals
import org.junit.Test

class SearchRankingTest {
    private fun track(
        id: String,
        title: String,
        artist: String,
        album: String = "Album",
        disc: Int? = 1,
        no: Int? = 1,
        resident: Boolean = true,
        genre: String = "Rock",
    ) = TrackEntity(
        id = id, title = title, artist = listOf(artist), albumArtist = listOf(artist),
        album = album, albumId = album, albumArtistId = artist, genre = listOf(genre),
        genreId = genre, trackNumber = no, discNumber = disc, year = null,
        durationMs = 0L, sizeBytes = 0L, format = "flac", mtimeNs = 0L, dateAdded = "",
        path = "", isResident = resident, searchBlob = "",
    )

    @Test fun artistNameBeatsTitleMentionAndGenre() {
        val rows = listOf(
            track("a", "Tool Shed Blues", "Someone"),
            track("b", "Anything", "Toolbox Band", genre = "tool"),
            track("c", "Pushit", "Tool", album = "Ænima", no = 11, resident = false),
        )
        assertEquals(listOf("c", "a", "b"), rankSearchResults("tool", rows).map { it.id })
    }

    @Test fun exactTitleBeatsPrefix() {
        val rows = listOf(
            track("a", "Pushing On", "X"),
            track("b", "Pushit", "Tool"),
        )
        assertEquals(listOf("b", "a"), rankSearchResults("pushit", rows).map { it.id })
    }

    @Test fun sameTierGroupsByArtistAlbumThenTrackOrder() {
        val rows = listOf(
            track("3", "Sober", "Tool", album = "Undertow", no = 5),
            track("2", "Pushit", "Tool", album = "Ænima", no = 11),
            track("1", "Stinkfist", "Tool", album = "Ænima", no = 1),
        )
        assertEquals(listOf("1", "2", "3"), rankSearchResults("tool", rows).map { it.id })
    }

    @Test fun foldsQueryBeforeComparing() {
        val rows = listOf(
            track("a", "Other", "Aenimal"),
            track("b", "Ænima", "Tool"),
        )
        assertEquals(listOf("b", "a"), rankSearchResults("AENIMA", rows).map { it.id })
    }
}
