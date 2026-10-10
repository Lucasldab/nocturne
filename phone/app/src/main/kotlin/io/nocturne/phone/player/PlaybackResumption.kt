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
 * persisted mediaId, or the row is no longer resident, that slot is silently
 * skipped. The startIndex is clamped to 0 if the original track is no longer
 * in the resolved list.
 *
 * Called from PlaybackService on the serviceScope (IO dispatcher).
 */
@OptIn(UnstableApi::class)
object PlaybackResumption {

    /**
     * Everything a resumption needs beyond the items themselves: the player
     * modes, plus the shuffle play order remapped onto the hydrated list.
     *
     * @property shuffleOrder indices into [items].mediaItems in ExoPlayer
     *           shuffle play order, or null when the snapshot carried no
     *           usable order (old blob, corrupt) — ExoPlayer then keeps the
     *           fresh random order it builds on setMediaItems.
     */
    class Restored(
        val items: MediaSession.MediaItemsWithStartPosition,
        val mediaIds: List<String>,
        val shuffleMode: Boolean,
        val repeatMode: Int,
        val shuffleOrder: IntArray?,
    )

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
        musicTreeUri: android.net.Uri? = null,
    ): MediaSession.MediaItemsWithStartPosition = restore(saved, trackDao, musicTreeUri).items

    /**
     * Like [toMediaItemsWithStartPosition], but also carries shuffle mode,
     * repeat mode and the remapped shuffle order so the service can put the
     * player back exactly as the user left it.
     */
    suspend fun restore(
        saved: SavedQueue,
        trackDao: TrackDao,
        musicTreeUri: android.net.Uri? = null,
    ): Restored {
        // Hydrate each mediaId, tracking the original index alongside.
        val resolved = mutableListOf<Pair<Int, MediaItem>>()
        for ((originalIdx, mediaId) in saved.mediaIds.withIndex()) {
            val entity = trackDao.byId(mediaId) ?: continue // silently skip missing tracks
            // Rotation can demote a track out of resident/ while the queue is
            // parked. The catalog row survives, so a residency check is the
            // only thing standing between a stale snapshot and ExoPlayer
            // hitting a file that is no longer on the phone.
            if (!entity.isResident) continue
            resolved.add(Pair(originalIdx, entity.toMediaItem(musicTreeUri = musicTreeUri)))
        }

        if (resolved.isEmpty()) {
            return Restored(emptyResult(), emptyList(), saved.shuffleMode, saved.repeatMode, null)
        }

        // Find the new index corresponding to the persisted currentIndex.
        // The persisted index refers to a position in saved.mediaIds; after
        // skipping missing tracks the position may shift. We find the first
        // resolved slot whose original index is >= saved.currentIndex.
        val startIndex = resolved.indexOfFirst { (origIdx, _) -> origIdx >= saved.currentIndex }
            .coerceAtLeast(0)

        val items = MediaSession.MediaItemsWithStartPosition(
            /* mediaItems = */ resolved.map { (_, item) -> item },
            /* startIndex = */ startIndex,
            /* startPositionMs = */ saved.currentPositionMs,
        )
        return Restored(
            items = items,
            mediaIds = resolved.map { (_, item) -> item.mediaId },
            shuffleMode = saved.shuffleMode,
            repeatMode = saved.repeatMode,
            shuffleOrder = remapShuffleOrder(
                savedOrder = saved.shuffleOrder,
                savedSize = saved.mediaIds.size,
                keptOriginalIndices = resolved.map { (origIdx, _) -> origIdx },
            ),
        )
    }

    /**
     * Project a persisted shuffle order onto the hydrated queue.
     *
     * [savedOrder] indexes the persisted mediaIds; tracks dropped during
     * hydration (deleted / no longer resident) are removed from the order and
     * the survivors renumbered to their new positions, so the relative play
     * order of what remains is unchanged.
     *
     * Returns null if [savedOrder] isn't a permutation of 0 until [savedSize]
     * (absent in pre-field blobs, or corrupt) — the caller then leaves
     * ExoPlayer's own order in place.
     */
    fun remapShuffleOrder(
        savedOrder: List<Int>,
        savedSize: Int,
        keptOriginalIndices: List<Int>,
    ): IntArray? {
        if (savedOrder.size != savedSize || savedSize == 0) return null
        val seen = BooleanArray(savedSize)
        for (i in savedOrder) {
            if (i !in 0 until savedSize || seen[i]) return null
            seen[i] = true
        }
        val newIndexOf = IntArray(savedSize) { -1 }
        keptOriginalIndices.forEachIndexed { newIdx, origIdx -> newIndexOf[origIdx] = newIdx }
        return savedOrder.map { newIndexOf[it] }.filter { it >= 0 }.toIntArray()
    }

    private fun emptyResult(): MediaSession.MediaItemsWithStartPosition =
        MediaSession.MediaItemsWithStartPosition(
            emptyList<MediaItem>(),
            /* startIndex = */ 0,
            /* startPositionMs = */ 0L,
        )
}
