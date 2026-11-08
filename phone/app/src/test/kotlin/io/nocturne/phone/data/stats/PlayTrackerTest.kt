package io.nocturne.phone.data.stats

import androidx.media3.common.C
import androidx.media3.common.Player
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * PlayTracker sits between Media3's callbacks and the stats file. These tests
 * replay the callback sequences a real ExoPlayer produces.
 *
 * The bug this guards against (found 2026-11-08 from the phone's own log): in
 * gapless playback the player never leaves STATE_READY between tracks, so a
 * duration cached on "entered READY" was unknown for every track after the
 * first auto-transition, and those plays were dropped. Only 34 of 515
 * consecutive full plays were logged back to back.
 */
class PlayTrackerTest {

    private fun autoTransition(t: PlayTracker, from: String, fromIndex: Int, positionMs: Long) =
        t.onDiscontinuity(
            reason = Player.DISCONTINUITY_REASON_AUTO_TRANSITION,
            oldMediaItemIndex = fromIndex, newMediaItemIndex = fromIndex + 1,
            oldTrackId = from, oldPositionMs = positionMs,
        )

    @Test fun gapless_run_logs_every_track() {
        val t = PlayTracker()
        // Album queued: the timeline reports lengths as each item is prepared,
        // while the playback state stays READY throughout.
        t.onItemStarted("a")
        t.onDurationKnown("a", 200_000L)
        t.onDurationKnown("b", 180_000L)
        assertEquals("play", autoTransition(t, "a", 0, 200_000L)?.kind)
        t.onItemStarted("b")
        t.onDurationKnown("c", 240_000L)
        val b = autoTransition(t, "b", 1, 180_000L)
        assertEquals("b", b?.trackId)
        assertEquals("play", b?.kind)
        assertEquals(180_000L, b?.durationMs)
        t.onItemStarted("c")
        assertEquals("c", autoTransition(t, "c", 2, 240_000L)?.trackId)
    }

    @Test fun duration_learned_after_the_item_started_is_used() {
        val t = PlayTracker()
        t.onItemStarted("a")
        t.onDurationKnown("a", 200_000L)
        assertEquals("a", autoTransition(t, "a", 0, 200_000L)?.trackId)
    }

    @Test fun unknown_duration_still_drops() {
        val t = PlayTracker()
        t.onItemStarted("a")
        assertNull(autoTransition(t, "a", 0, 200_000L))
    }

    @Test fun manual_skip_is_a_skip() {
        val t = PlayTracker()
        t.onItemStarted("a")
        t.onDurationKnown("a", 200_000L)
        val r = t.onDiscontinuity(Player.DISCONTINUITY_REASON_SEEK, 0, 1, "a", 20_000L)
        assertEquals("skip", r?.kind)
    }

    @Test fun replacing_the_queue_logs_the_interrupted_track() {
        val t = PlayTracker()
        t.onItemStarted("a")
        t.onDurationKnown("a", 200_000L)
        // setMediaItems(newAlbum): Media3 reports the current item's removal.
        val r = t.onDiscontinuity(Player.DISCONTINUITY_REASON_REMOVE, 0, 0, "a", 150_000L)
        assertEquals("a", r?.trackId)
        assertEquals("play", r?.kind)
        assertEquals(150_000L, r?.playedMs)
    }

    @Test fun last_track_of_the_queue_ending_is_logged() {
        val t = PlayTracker()
        t.onItemStarted("z")
        t.onDurationKnown("z", 200_000L)
        val r = t.onEnded("z", 200_000L)
        assertEquals("z", r?.trackId)
        assertEquals("play", r?.kind)
    }

    @Test fun a_listen_is_logged_once() {
        val t = PlayTracker()
        t.onItemStarted("z")
        t.onDurationKnown("z", 200_000L)
        t.onEnded("z", 200_000L)
        // Picking a new album after the end removes the ended item.
        assertNull(t.onDiscontinuity(Player.DISCONTINUITY_REASON_REMOVE, 0, 0, "z", 200_000L))
        assertNull(t.onEnded("z", 200_000L))
    }

    @Test fun repeat_one_logs_each_loop() {
        val t = PlayTracker()
        t.onItemStarted("a")
        t.onDurationKnown("a", 200_000L)
        assertEquals("a", autoTransition(t, "a", 0, 200_000L)?.trackId.also { t.onItemStarted("a") })
        assertEquals("a", autoTransition(t, "a", 0, 200_000L)?.trackId)
    }

    @Test fun unset_duration_reports_are_ignored() {
        val t = PlayTracker()
        t.onItemStarted("a")
        t.onDurationKnown("a", 200_000L)
        t.onDurationKnown("a", C.TIME_UNSET)
        assertEquals("a", autoTransition(t, "a", 0, 200_000L)?.trackId)
    }

    @Test fun duration_memory_is_bounded() {
        val t = PlayTracker(maxRemembered = 3)
        for (i in 0 until 10) t.onDurationKnown("t$i", 100_000L)
        t.onItemStarted("t0")
        assertNull(autoTransition(t, "t0", 0, 100_000L))
        t.onItemStarted("t9")
        assertEquals("t9", autoTransition(t, "t9", 0, 100_000L)?.trackId)
    }
}
