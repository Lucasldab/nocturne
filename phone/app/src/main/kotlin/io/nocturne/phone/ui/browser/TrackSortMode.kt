package io.nocturne.phone.ui.browser

/**
 * Sort modes available on the Tracks browser tab. The selection is
 * persisted in DataStore via [io.nocturne.phone.data.prefs.SyncPrefs.trackSortMode].
 *
 * - `Alphabetical` — case-insensitive title order (mirrors the DAO's
 *   `ORDER BY title COLLATE NOCASE` paged query). Default on first launch.
 * - `MostListened` — descending all-time per-track play count from the
 *   local stats JSONL (`StatsAggregator.aggregate(.., allTime = true)`).
 * - `RecentlyListened` — descending per-track lastPlayedMs from the local
 *   stats JSONL.
 * - `RecentlyDownloaded` — descending `TrackEntity.dateAdded` (ISO-8601
 *   date string, lexicographic compare is correct).
 *
 * `persistedKey` is the stable string written to DataStore — DO NOT change
 * existing values without a migration path; new modes append a fresh key.
 *
 * `label` is the chip label shown in [components.TrackSortToggle].
 */
enum class TrackSortMode(val persistedKey: String, val label: String) {
    Alphabetical("alpha", "a-z"),
    MostListened("most-listened", "most"),
    RecentlyListened("recently-listened", "recent"),
    RecentlyDownloaded("recently-downloaded", "added"),
    ;

    companion object {
        val DEFAULT: TrackSortMode = Alphabetical

        /** Forward-compatible: unknown / null keys fall back to [DEFAULT]. */
        fun fromPersistedKey(key: String?): TrackSortMode =
            values().firstOrNull { it.persistedKey == key } ?: DEFAULT
    }
}
