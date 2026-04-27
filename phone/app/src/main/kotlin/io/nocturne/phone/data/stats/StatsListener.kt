package io.nocturne.phone.data.stats

import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * Phase 6 (STATS-01 / STATS-02 / D-10..D-14 / D-24): play-vs-skip emitter
 * hooked into Media3's `Player.Listener` callbacks.
 *
 * Lifecycle:
 *   - `onPlaybackStateChanged(STATE_READY)`: caches `player.contentDuration`
 *     into `preTransitionDurationMs`. This is the only place duration is
 *     guaranteed resolved — Pitfall 1 (`player.duration` is `C.TIME_UNSET`
 *     during transitions).
 *   - `onMediaItemTransition`: caches the new item's `mediaId` and resets the
 *     duration cache so the next `STATE_READY` re-fills it.
 *   - `onPositionDiscontinuity`: the actual classifier hook. Calls
 *     `decideEmission(...)` with the cached duration and the old position;
 *     if non-null, dispatches `StatsWriter.record` on `scope` (FGS-immune).
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
) : Player.Listener {

    // Cached pre-transition state. Updated on STATE_READY (duration) and
    // onMediaItemTransition (track id of the NEW item — used for the NEXT
    // discontinuity callback).
    private var preTransitionDurationMs: Long = C.TIME_UNSET
    private var preTransitionTrackId: String? = null

    override fun onPlaybackStateChanged(playbackState: Int) {
        if (playbackState == Player.STATE_READY) {
            preTransitionDurationMs = player.contentDuration  // Pitfall 1 mitigation
            if (preTransitionTrackId == null) {
                preTransitionTrackId = player.currentMediaItem?.mediaId
            }
        }
    }

    override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
        // After a transition, the NEW item's id becomes the candidate for the
        // next transition. The OLD id was used by the discontinuity callback,
        // which fires immediately BEFORE this one per Media3 1.10 docs.
        preTransitionTrackId = mediaItem?.mediaId
        // Force a re-cache on the next STATE_READY — the new item's duration
        // hasn't been resolved yet (Pitfall 1).
        preTransitionDurationMs = C.TIME_UNSET
    }

    override fun onPositionDiscontinuity(
        oldPosition: Player.PositionInfo,
        newPosition: Player.PositionInfo,
        reason: Int,
    ) {
        val decision = decideEmission(
            reason = reason,
            oldMediaItemIndex = oldPosition.mediaItemIndex,
            newMediaItemIndex = newPosition.mediaItemIndex,
            oldPositionMs = oldPosition.positionMs,
            cachedDurationMs = preTransitionDurationMs,
            cachedTrackId = preTransitionTrackId,
        ) ?: return
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

    internal data class Emission(
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
         * mid-song = skip). Intra-track seeks do not emit.
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
