package io.nocturne.phone.data.stats

import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.Timeline
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * Phase 6 (STATS-01 / STATS-02 / D-10..D-14 / D-24): play-vs-skip emitter
 * hooked into Media3's `Player.Listener` callbacks.
 *
 * The callback bookkeeping lives in [PlayTracker] (durations remembered per
 * track id, one open listen at a time); this class only adapts Media3:
 *   - READY, timeline changes and transitions report durations of the
 *     current and next items. Gapless playback stays READY between tracks,
 *     so READY alone is not enough (the bug fixed 2026-11-08).
 *   - `onMediaItemTransition` opens the new item's listen.
 *   - `onPositionDiscontinuity` (auto-transition, seek onto another item,
 *     removal of the current item) and STATE_ENDED (last item played out)
 *     close it and emit via `StatsWriter.record` on `scope` (FGS-immune).
 *
 * `decideEmission` is factored out as an `internal companion fun` so the
 * decision table can be exercised by a pure-JVM unit test without a real
 * Media3 Player.
 */
class StatsListener(
    private val player: Player,
    private val writer: StatsWriter,
    private val scope: CoroutineScope,
    private val nowMs: () -> Long = { System.currentTimeMillis() },
    private val tracker: PlayTracker = PlayTracker(),
) : Player.Listener {

    private val window = Timeline.Window()

    /** Record the duration of the timeline item at [index], if the player knows it. */
    private fun rememberWindow(index: Int) {
        val timeline = player.currentTimeline
        if (index == C.INDEX_UNSET || index < 0 || index >= timeline.windowCount) return
        timeline.getWindow(index, window)
        tracker.onDurationKnown(window.mediaItem.mediaId, window.durationMs)
    }

    private fun rememberCurrentAndNext() {
        rememberWindow(player.currentMediaItemIndex)
        rememberWindow(player.nextMediaItemIndex)
    }

    override fun onPlaybackStateChanged(playbackState: Int) {
        val current = player.currentMediaItem?.mediaId
        when (playbackState) {
            Player.STATE_READY -> {
                tracker.onDurationKnown(current, player.contentDuration)
                rememberCurrentAndNext()
                // Restored queue, or a replay after STATE_ENDED: no transition
                // announces the item, so open its listen here.
                if (tracker.openTrackId == null) tracker.onItemStarted(current)
            }
            Player.STATE_ENDED -> emit(tracker.onEnded(current, player.currentPosition))
        }
    }

    override fun onTimelineChanged(timeline: Timeline, reason: Int) {
        // Gapless: the next item's duration arrives here (SOURCE_UPDATE) while
        // the playback state stays READY.
        rememberCurrentAndNext()
    }

    override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
        // Media3 calls onPositionDiscontinuity for the OLD item first, so the
        // previous listen is already closed when this opens the next one.
        tracker.onItemStarted(mediaItem?.mediaId)
        rememberCurrentAndNext()
    }

    override fun onPositionDiscontinuity(
        oldPosition: Player.PositionInfo,
        newPosition: Player.PositionInfo,
        reason: Int,
    ) {
        emit(
            tracker.onDiscontinuity(
                reason = reason,
                oldMediaItemIndex = oldPosition.mediaItemIndex,
                newMediaItemIndex = newPosition.mediaItemIndex,
                oldTrackId = oldPosition.mediaItem?.mediaId,
                oldPositionMs = oldPosition.positionMs,
            ),
        )
    }

    private fun emit(decision: Emission?) {
        decision ?: return
        scope.launch {
            writer.record(
                track = decision.trackId,
                playedMs = decision.playedMs,
                durationMs = decision.durationMs,
                kind = decision.kind,
                tsMs = nowMs(),
            )
        }
    }

    data class Emission(
        val trackId: String,
        val playedMs: Long,
        val durationMs: Long,
        val kind: String,
    )

    internal companion object {
        /**
         * Decide whether to emit a StatsEvent. Returns null when no emission
         * should be made: intra-track seek (user scrubbing within the same
         * item), unresolved duration, missing track id, or an unrelated
         * discontinuity reason.
         *
         * Pitfall 2 mitigation: `oldPositionMs > cachedDurationMs` is clamped
         * to `cachedDurationMs` (gapless decoder tail-trim can report a
         * position slightly past the mathematical end).
         *
         * Per D-13: emits for AUTO_TRANSITION (track played to natural end)
         * AND for SEEK that crosses a media-item boundary (user pressed Next
         * mid-song = skip), and for REMOVE (the current item left the queue
         * mid-song). Intra-track seeks do not emit.
         */
        fun decideEmission(
            reason: Int,
            oldMediaItemIndex: Int,
            newMediaItemIndex: Int,
            oldPositionMs: Long,
            cachedDurationMs: Long,
            cachedTrackId: String?,
        ): Emission? {
            val crossesItem = oldMediaItemIndex != newMediaItemIndex
            val emitting = when (reason) {
                Player.DISCONTINUITY_REASON_AUTO_TRANSITION -> true
                Player.DISCONTINUITY_REASON_SEEK -> crossesItem
                // The current item left the queue (setMediaItems for another
                // album, or a swipe-remove): the interrupted listen counts.
                Player.DISCONTINUITY_REASON_REMOVE -> true
                else -> false
            }
            if (!emitting) return null
            if (cachedDurationMs == C.TIME_UNSET || cachedDurationMs <= 0L) return null
            val trackId = cachedTrackId ?: return null
            val clampedPlayed = oldPositionMs
                .coerceAtLeast(0L)
                .coerceAtMost(cachedDurationMs)  // Pitfall 2 — gapless tail-trim
            val kind = if (PlayClassifier.isPlay(clampedPlayed, cachedDurationMs)) "play" else "skip"
            return Emission(
                trackId = trackId,
                playedMs = clampedPlayed,
                durationMs = cachedDurationMs,
                kind = kind,
            )
        }
    }
}
