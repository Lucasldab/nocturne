package io.nocturne.phone.data.db

import io.nocturne.phone.data.db.entity.TrackEntity
import java.security.MessageDigest
import java.text.Normalizer

internal fun sha256Hex(s: String): String =
    MessageDigest.getInstance("SHA-256")
        .digest(s.toByteArray())
        .joinToString("") { "%02x".format(it) }

/**
 * Inline accent-fold algorithm — a duplicate of the importer's algorithm
 * (Plan 04-03 owns the canonical implementation). Steps:
 *  1. NFKD normalize.
 *  2. Strip combining diacritical marks.
 *  3. lowercase(Locale.ROOT).
 *  4. Collapse whitespace runs.
 */
internal fun foldForSearch(s: String): String {
    val nfkd = Normalizer.normalize(s, Normalizer.Form.NFKD)
    val noMarks = nfkd.replace(Regex("\\p{InCombiningDiacriticalMarks}+"), "")
    val lower = noMarks.lowercase()
    return lower.replace(Regex("\\s+"), " ").trim()
}

/**
 * Deterministic synthetic catalog. n=15_000 default matches the sizing used
 * by SearchDao wall-clock assertion on the typeahead path. Properties:
 *  - 150 distinct artists (i / 100), 1250 albums of 12 tracks each,
 *    2 genres ("Café Rock" 1/5, "Synth Pop" 4/5), one disc per album.
 *  - 1 in 3 rows is resident (path begins "resident/..."), rest "archive/...".
 */
internal fun synthCatalog(n: Int = 15_000): List<TrackEntity> {
    val out = ArrayList<TrackEntity>(n)
    for (i in 0 until n) {
        val artist = "Synth Artist ${i / 100}"
        val album = "Synth Album ${i / 12}"
        val genre = if (i % 5 == 0) "Café Rock" else "Synth Pop"
        val title = "Track ${i.toString().padStart(5, '0')}"
        val resident = (i % 3 == 0)
        val pathPrefix = if (resident) "resident" else "archive"
        val albumId = sha256Hex("$artist ${album.lowercase()}")
        val artistId = sha256Hex(artist.lowercase())
        val genreId = sha256Hex(genre.lowercase())
        val blob = foldForSearch("$title $artist $album $artist $genre")
        out += TrackEntity(
            id = sha256Hex("track-${i.toString().padStart(6, '0')}"),
            title = title,
            artist = listOf(artist),
            albumArtist = listOf(artist),
            album = album,
            albumId = albumId,
            albumArtistId = artistId,
            genre = listOf(genre),
            genreId = genreId,
            trackNumber = (i % 12) + 1,
            discNumber = 1,
            year = 2020 + (i % 6),
            durationMs = 200_000L + (i % 100) * 1000,
            sizeBytes = 30_000_000L + (i % 100) * 1000,
            format = "flac",
            mtimeNs = 1_700_000_000_000_000_000L + i,
            dateAdded = "2026-04-%02dT00:00:00Z".format((i % 27) + 1),
            path = "$pathPrefix/$artist/$album/$title.flac",
            isResident = resident,
            searchBlob = blob,
        )
    }
    return out
}

/** Hand-tuned rows for accent-fold + multi-token FTS assertions. */
internal fun knownAccentFixture(): List<TrackEntity> {
    fun mk(idSeed: String, title: String, artist: String, album: String, genre: String): TrackEntity {
        val albumId = sha256Hex("$artist ${album.lowercase()}")
        val artistId = sha256Hex(artist.lowercase())
        val genreId = sha256Hex(genre.lowercase())
        return TrackEntity(
            id = sha256Hex(idSeed),
            title = title,
            artist = listOf(artist),
            albumArtist = listOf(artist),
            album = album,
            albumId = albumId,
            albumArtistId = artistId,
            genre = listOf(genre),
            genreId = genreId,
            trackNumber = 1,
            discNumber = 1,
            year = 2024,
            durationMs = 240_000L,
            sizeBytes = 30_000_000L,
            format = "flac",
            mtimeNs = 0L,
            dateAdded = "2024-01-01T00:00:00Z",
            path = "resident/$artist/$album/$title.flac",
            isResident = true,
            searchBlob = foldForSearch("$title $artist $album $artist $genre"),
        )
    }
    return listOf(
        mk("known-cafe-orange", "café orange", "Sigur Rós", "Ágætis byrjun", "Post-Rock"),
        mk("known-etoile", "Étoile", "Daft Punk", "Discovery", "Electronic"),
        mk("known-plain", "Plain Song", "ASCII Artist", "ASCII Album", "ASCII Genre"),
        mk("known-cafe-bar", "Café Bar", "Café Rock Band", "Café Rock Album", "Café Rock"),
        mk("known-coffee", "Coffee", "Mainstream", "Mainstream Album", "Pop"),
    )
}
