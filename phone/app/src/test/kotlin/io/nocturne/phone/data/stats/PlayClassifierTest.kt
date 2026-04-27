package io.nocturne.phone.data.stats

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Phase 6 (STATS-01 / STATS-02 / D-12): exhaustive table for the play-vs-skip
 * threshold. Pure JVM — no Android, no Robolectric.
 *
 * Threshold = `min(durationMs / 2, 240_000)`. Tracks under 8 minutes use the
 * 50% rule; tracks ≥ 8 minutes use the flat 4-minute cap.
 */
class PlayClassifierTest {
    @Test fun zero_played_is_not_play() = assertFalse(PlayClassifier.isPlay(0L, 100_000L))
    @Test fun just_under_half_short_track_is_skip() = assertFalse(PlayClassifier.isPlay(49_999L, 100_000L))
    @Test fun exactly_half_short_track_is_play() = assertTrue(PlayClassifier.isPlay(50_000L, 100_000L))
    @Test fun just_under_four_min_long_track_is_skip() = assertFalse(PlayClassifier.isPlay(239_999L, 600_000L))
    @Test fun exactly_four_min_long_track_is_play() = assertTrue(PlayClassifier.isPlay(240_000L, 600_000L))
    @Test fun over_four_min_long_track_is_play() = assertTrue(PlayClassifier.isPlay(241_000L, 600_000L))
    @Test fun half_of_four_min_track_is_play() = assertTrue(PlayClassifier.isPlay(120_000L, 240_000L))
    @Test fun zero_duration_track_is_not_play() = assertFalse(PlayClassifier.isPlay(0L, 0L))
    @Test fun very_short_track_half_is_play() = assertTrue(PlayClassifier.isPlay(50L, 100L))
    @Test fun negative_played_is_coerced_to_zero() = assertFalse(PlayClassifier.isPlay(-100L, 100_000L))
}
