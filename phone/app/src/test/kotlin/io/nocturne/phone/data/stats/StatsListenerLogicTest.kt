package io.nocturne.phone.data.stats

import androidx.media3.common.C
import androidx.media3.common.Player
import io.nocturne.phone.data.stats.StatsListener.Companion.decideEmission
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Phase 6 (STATS-01 / STATS-02 / D-10..D-14): exhaustive table for
 * `StatsListener.decideEmission`. Covers AUTO_TRANSITION above/below
 * threshold, SEEK across vs intra-item, gapless tail-trim clamp (Pitfall 2),
 * unresolved duration drop (Pitfall 1), missing track-id drop, and
 * unrelated discontinuity reasons.
 *
 * Uses `Player`/`C` constants from media3 — these are JVM constants and
 * don't require Robolectric or a running player.
 */
class StatsListenerLogicTest {

    @Test fun auto_transition_above_threshold_emits_play() {
        val r = decideEmission(
            reason = Player.DISCONTINUITY_REASON_AUTO_TRANSITION,
            oldMediaItemIndex = 0, newMediaItemIndex = 1,
            oldPositionMs = 100_000L,
            cachedDurationMs = 200_000L,
            cachedTrackId = "abc",
        )
        assertEquals("play", r?.kind)
        assertEquals(100_000L, r?.playedMs)
        assertEquals(200_000L, r?.durationMs)
        assertEquals("abc", r?.trackId)
    }

    @Test fun auto_transition_below_threshold_emits_skip() {
        val r = decideEmission(
            reason = Player.DISCONTINUITY_REASON_AUTO_TRANSITION,
            oldMediaItemIndex = 0, newMediaItemIndex = 1,
            oldPositionMs = 30_000L,
            cachedDurationMs = 200_000L,
            cachedTrackId = "abc",
        )
        assertEquals("skip", r?.kind)
    }

    @Test fun seek_across_items_below_threshold_emits_skip() {
        val r = decideEmission(
            reason = Player.DISCONTINUITY_REASON_SEEK,
            oldMediaItemIndex = 0, newMediaItemIndex = 1,
            oldPositionMs = 30_000L,
            cachedDurationMs = 200_000L,
            cachedTrackId = "abc",
        )
        assertEquals("skip", r?.kind)
    }

    @Test fun seek_intra_item_does_not_emit() {
        val r = decideEmission(
            reason = Player.DISCONTINUITY_REASON_SEEK,
            oldMediaItemIndex = 0, newMediaItemIndex = 0,
            oldPositionMs = 30_000L,
            cachedDurationMs = 200_000L,
            cachedTrackId = "abc",
        )
        assertNull(r)
    }

    @Test fun gapless_tail_trim_clamps_played_to_duration() {
        val r = decideEmission(
            reason = Player.DISCONTINUITY_REASON_AUTO_TRANSITION,
            oldMediaItemIndex = 0, newMediaItemIndex = 1,
            oldPositionMs = 300_000L,                 // beyond duration (decoder tail)
            cachedDurationMs = 200_000L,
            cachedTrackId = "abc",
        )
        assertEquals(200_000L, r?.playedMs)           // clamped (Pitfall 2)
        assertEquals("play", r?.kind)
    }

    @Test fun unresolved_duration_does_not_emit() {
        val r = decideEmission(
            reason = Player.DISCONTINUITY_REASON_AUTO_TRANSITION,
            oldMediaItemIndex = 0, newMediaItemIndex = 1,
            oldPositionMs = 100_000L,
            cachedDurationMs = C.TIME_UNSET,
            cachedTrackId = "abc",
        )
        assertNull(r)
    }

    @Test fun zero_duration_does_not_emit() {
        val r = decideEmission(
            reason = Player.DISCONTINUITY_REASON_AUTO_TRANSITION,
            oldMediaItemIndex = 0, newMediaItemIndex = 1,
            oldPositionMs = 100_000L,
            cachedDurationMs = 0L,
            cachedTrackId = "abc",
        )
        assertNull(r)
    }

    @Test fun missing_track_id_does_not_emit() {
        val r = decideEmission(
            reason = Player.DISCONTINUITY_REASON_AUTO_TRANSITION,
            oldMediaItemIndex = 0, newMediaItemIndex = 1,
            oldPositionMs = 100_000L,
            cachedDurationMs = 200_000L,
            cachedTrackId = null,
        )
        assertNull(r)
    }

    @Test fun other_discontinuity_reasons_do_not_emit() {
        val r = decideEmission(
            reason = Player.DISCONTINUITY_REASON_REMOVE,
            oldMediaItemIndex = 0, newMediaItemIndex = 1,
            oldPositionMs = 100_000L,
            cachedDurationMs = 200_000L,
            cachedTrackId = "abc",
        )
        assertNull(r)
    }
}
