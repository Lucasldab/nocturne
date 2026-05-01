package io.nocturne.phone.ui.browser

import io.nocturne.phone.data.db.entity.TrackEntity

/**
 * Pure-JVM sorter for the Tracks browser tab. All four sort modes are
 * deterministic — secondary keys (title.lowercase) break every tie so unit
 * tests can assert exact orderings without relying on input iteration order.
 *
 * Performance is bounded by the library size (~1899 tracks at the time of
 * writing); a single Kotlin `sortedWith` is well under the frame budget.
 *
 * The map parameters default to empty so callers that don't have stats
 * (e.g. `Alphabetical`) can elide the JSONL read; `RecentlyDownloaded`
 * accepts an optional `perTrackPinnedAt` overlay so freshly-pinned tracks
 * outrank bulk-rescanned older catalog rows (quick task 260430-vtb Bug 2).
 *
 * @param tracks unsorted list (typically the result of `TrackDao.listAll()`).
 * @param mode sort mode to apply.
 * @param perTrackPlays trackId -> play count. Missing ids default to 0.
 * @param perTrackLastTs trackId -> last-play epoch-ms. Missing ids sort to
 *   the bottom (Long.MIN_VALUE so never-played tracks are last).
 * @param perTrackPinnedAt trackId -> pinnedAt epoch-ms (only consulted by
 *   [TrackSortMode.RecentlyDownloaded]). When present, the effective
 *   ordering key for a track is `max(parsedDateAdded, pinnedAt)` so a
 *   freshly pinned track outranks bulk-rescanned older catalog rows that
 *   share the same dateAdded. Missing ids contribute Long.MIN_VALUE.
 */
object TrackSorter {
    fun sort(
        tracks: List<TrackEntity>,
        mode: TrackSortMode,
        perTrackPlays: Map<String, Int> = emptyMap(),
        perTrackLastTs: Map<String, Long> = emptyMap(),
        perTrackPinnedAt: Map<String, Long> = emptyMap(),
    ): List<TrackEntity> {
        if (tracks.isEmpty()) return tracks
        val byTitle = compareBy<TrackEntity> { it.title.lowercase() }
        return when (mode) {
            TrackSortMode.Alphabetical -> tracks.sortedWith(byTitle)
            TrackSortMode.MostListened -> tracks.sortedWith(
                compareByDescending<TrackEntity> { perTrackPlays[it.id] ?: 0 }
                    .thenByDescending { perTrackLastTs[it.id] ?: Long.MIN_VALUE }
                    .then(byTitle),
            )
            TrackSortMode.RecentlyListened -> tracks.sortedWith(
                compareByDescending<TrackEntity> { perTrackLastTs[it.id] ?: Long.MIN_VALUE }
                    .then(byTitle),
            )
            TrackSortMode.RecentlyDownloaded -> {
                // Quick task 260430-vtb Bug 2: overlay pinnedAt onto dateAdded
                // so a freshly-pinned track outranks a bulk-rescanned older
                // catalog row. dateAdded is ISO-8601 UTC ("2026-04-30T12:34:56Z"
                // emitted by the daemon's scan.c). Parse to epoch ms; on parse
                // failure fall back to MIN so the pinnedAt contribution still
                // wins if the user pinned this track.
                fun keyOf(t: TrackEntity): Long {
                    val addedMs = runCatching {
                        java.time.Instant.parse(t.dateAdded).toEpochMilli()
                    }.getOrDefault(Long.MIN_VALUE)
                    val pinnedMs = perTrackPinnedAt[t.id] ?: Long.MIN_VALUE
                    return maxOf(addedMs, pinnedMs)
                }
                tracks.sortedWith(
                    compareByDescending<TrackEntity> { keyOf(it) }.then(byTitle),
                )
            }
        }
    }
}
