package io.nocturne.phone.data.stats

import androidx.media3.common.C
import androidx.media3.common.Player

/**
 * Turns Media3 callbacks into play/skip emissions, with no Player of its own,
 * so the callback sequences can be replayed in plain JVM tests.
 *
 * Durations are remembered per track id as the player learns them (READY,
 * timeline updates, transitions) instead of being cached on "entered READY".
 * In gapless playback the player stays READY from one track to the next, so
 * that cache was empty for every track after the first auto-transition and
 * those plays were silently dropped (fixed 2026-11-08).
 *
 * One listen is open at a time: [onItemStarted] opens it and the first
 * emission for that track closes it, so a track that ended and is then removed
 * from the queue is not logged twice.
 */
class PlayTracker(private val maxRemembered: Int = 512) {

    private val durations = object : LinkedHashMap<String, Long>(16, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Long>?) =
            size > maxRemembered
    }

    var openTrackId: String? = null
        private set

    fun onDurationKnown(trackId: String?, durationMs: Long) {
        if (trackId.isNullOrEmpty() || durationMs == C.TIME_UNSET || durationMs <= 0L) return
        durations[trackId] = durationMs
    }

    fun onItemStarted(trackId: String?) {
        openTrackId = trackId?.takeIf { it.isNotEmpty() }
    }

    /** Auto-transition, a seek onto another item, or removal of the current item. */
    fun onDiscontinuity(
        reason: Int,
        oldMediaItemIndex: Int,
        newMediaItemIndex: Int,
        oldTrackId: String?,
        oldPositionMs: Long,
    ): StatsListener.Emission? {
        val track = oldTrackId ?: openTrackId ?: return null
        if (track != openTrackId) return null
        val emission = StatsListener.decideEmission(
            reason = reason,
            oldMediaItemIndex = oldMediaItemIndex,
            newMediaItemIndex = newMediaItemIndex,
            oldPositionMs = oldPositionMs,
            cachedDurationMs = durations[track] ?: C.TIME_UNSET,
            cachedTrackId = track,
        ) ?: return null
        openTrackId = null
        return emission
    }

    /** STATE_ENDED: the last item of the queue played out, with no transition after it. */
    fun onEnded(trackId: String?, positionMs: Long): StatsListener.Emission? =
        onDiscontinuity(
            reason = Player.DISCONTINUITY_REASON_AUTO_TRANSITION,
            oldMediaItemIndex = 0,
            newMediaItemIndex = 1,
            oldTrackId = trackId,
            oldPositionMs = positionMs,
        )
}
