package io.nocturne.phone.ui.search

import io.nocturne.phone.data.catalog.accentFold
import io.nocturne.phone.data.db.entity.TrackEntity

/**
 * Order FTS hits so the thing you typed comes first.
 *
 * FTS4 MATCH returns rows in rowid order, so "tool" used to list whatever
 * mentioned "tool" anywhere (a genre, a word in a title) ahead of the band,
 * and the 50-row cap could cut the band off entirely.
 *
 * Tiers, best first:
 *   0  title or an artist is exactly the query
 *   1  title or an artist starts with it
 *   2  album is or starts with it
 *   3  anything else FTS matched (genre, mid-field word, multi-field query)
 * Within a tier: on-phone tracks first, then artist, album, disc, track — so a
 * band's tracks read in album order.
 */
fun rankSearchResults(rawInput: String, rows: List<TrackEntity>): List<TrackEntity> {
    val q = normalizeSearchInput(rawInput) ?: return rows
    fun tier(t: TrackEntity): Int {
        val title = accentFold(t.title)
        val artists = (t.artist + t.albumArtist).map(::accentFold)
        val album = accentFold(t.album)
        return when {
            title == q || artists.any { it == q } -> 0
            title.startsWith(q) || artists.any { it.startsWith(q) } -> 1
            album.startsWith(q) -> 2
            else -> 3
        }
    }
    return rows
        .map { it to tier(it) }
        .sortedWith(
            compareBy<Pair<TrackEntity, Int>> { it.second }
                .thenBy { !it.first.isResident }
                .thenBy { accentFold(it.first.albumArtist.firstOrNull() ?: it.first.artist.firstOrNull().orEmpty()) }
                .thenBy { accentFold(it.first.album) }
                .thenBy { it.first.discNumber ?: 0 }
                .thenBy { it.first.trackNumber ?: 0 },
        )
        .map { it.first }
}
