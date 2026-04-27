package io.nocturne.phone.player

import androidx.annotation.OptIn
import androidx.media3.common.MediaItem
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaSession
import io.nocturne.phone.data.db.dao.TrackDao

/**
 * Pure helper: converts a persisted SavedQueue into a
 * MediaSession.MediaItemsWithStartPosition suitable for returning from
 * MediaSession.Callback.onPlaybackResumption (PLAY-04).
 *
 * Resilient against missing tracks: if TrackDao.byId returns null for a
 * persisted mediaId, that slot is silently skipped. The startIndex is
 * clamped to 0 if the original track is no longer in the resolved list.
 *
 * Called from PlaybackService on the serviceScope (IO dispatcher).
 */
@OptIn(UnstableApi::class)
object PlaybackResumption {

    /**
     * Hydrate [saved] using [trackDao] lookups.
     *
     * @param saved The persisted queue state from QueueRepository.loadQueue().
     * @param trackDao Room DAO for track lookups by sha256 id.
     * @return A MediaItemsWithStartPosition ready for Media3 playback resumption.
     *         Returns an empty list at index 0 / position 0 if [saved] is empty
     *         or all tracks were deleted from the catalog.
     */
    suspend fun toMediaItemsWithStartPosition(
        saved: SavedQueue,
        trackDao: TrackDao,
    ): MediaSession.MediaItemsWithStartPosition {
        if (saved.mediaIds.isEmpty()) {
            return emptyResult()
        }

        // Hydrate each mediaId, tracking the original index alongside.
        val resolved = mutableListOf<Pair<Int, MediaItem>>()
        for ((originalIdx, mediaId) in saved.mediaIds.withIndex()) {
            val entity = trackDao.byId(mediaId) ?: continue // silently skip missing tracks
            resolved.add(Pair(originalIdx, entity.toMediaItem()))
        }

        if (resolved.isEmpty()) return emptyResult()

        // Find the new index corresponding to the persisted currentIndex.
        // The persisted index refers to a position in saved.mediaIds; after
        // skipping missing tracks the position may shift. We find the first
        // resolved slot whose original index is >= saved.currentIndex.
        val startIndex = resolved.indexOfFirst { (origIdx, _) -> origIdx >= saved.currentIndex }
            .coerceAtLeast(0)

        return MediaSession.MediaItemsWithStartPosition(
            /* mediaItems = */ resolved.map { (_, item) -> item },
            /* startIndex = */ startIndex,
            /* startPositionMs = */ saved.currentPositionMs,
        )
    }

    private fun emptyResult(): MediaSession.MediaItemsWithStartPosition =
        MediaSession.MediaItemsWithStartPosition(
            emptyList<MediaItem>(),
            /* startIndex = */ 0,
            /* startPositionMs = */ 0L,
        )
}
