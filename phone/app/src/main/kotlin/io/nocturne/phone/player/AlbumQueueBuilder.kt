package io.nocturne.phone.player

import androidx.annotation.OptIn
import androidx.media3.common.MediaItem
import androidx.media3.common.util.UnstableApi
import io.nocturne.phone.data.db.entity.TrackEntity

/**
 * Pure function: given an album's track list (already ordered by trackNumber)
 * and a starting track, produce (List<MediaItem>, startIndex) ready to feed
 * MediaController.setMediaItems(items, startIndex, 0L).
 *
 * Implements PLAY-07: tap a track within an album → queue the entire album
 * from that track forward. We don't truncate the queue at startTrack;
 * Media3's setMediaItems(items, startIndex, ...) starts playback at the
 * given index and the rest plays in order.
 *
 * If startTrack is not in the list, defensively returns startIndex = 0
 * (caller's mistake, but we don't crash).
 */
@OptIn(UnstableApi::class)
object AlbumQueueBuilder {
    fun buildFromTrack(
        tracks: List<TrackEntity>,
        startTrack: TrackEntity,
    ): Pair<List<MediaItem>, Int> {
        val items = tracks.map { it.toMediaItem() }
        val startIndex = tracks.indexOfFirst { it.id == startTrack.id }.coerceAtLeast(0)
        return items to startIndex
    }
}
