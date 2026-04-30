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
 * The two map parameters default to empty so callers that don't have stats
 * (e.g. `Alphabetical` / `RecentlyDownloaded`) can elide the JSONL read.
 *
 * @param tracks unsorted list (typically the result of `TrackDao.listAll()`).
 * @param mode sort mode to apply.
 * @param perTrackPlays trackId -> play count. Missing ids default to 0.
 * @param perTrackLastTs trackId -> last-play epoch-ms. Missing ids sort to
 *   the bottom (Long.MIN_VALUE so never-played tracks are last).
 */
object TrackSorter {
    fun sort(
        tracks: List<TrackEntity>,
        mode: TrackSortMode,
        perTrackPlays: Map<String, Int> = emptyMap(),
        perTrackLastTs: Map<String, Long> = emptyMap(),
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
            TrackSortMode.RecentlyDownloaded -> tracks.sortedWith(
                compareByDescending<TrackEntity> { it.dateAdded }
                    .then(byTitle),
            )
        }
    }
}
