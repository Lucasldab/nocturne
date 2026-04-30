package io.nocturne.phone.ui.system

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.LocalDateTime
import java.time.ZoneOffset

/**
 * Quick task 260428-7zc — StatsAggregator unit tests.
 *
 * Pure JVM. Hand-written JSONL strings (NOT roundtripped through
 * encodeToString) so the test asserts against a literal byte shape and
 * therefore catches unintentional schema drift.
 *
 * `now` is fixed to 2026-04-28 12:00:00 UTC for reproducibility. Tests pass
 * `zone = ZoneOffset.UTC` so heatmap day/hour math does not depend on the
 * CI machine's default time zone.
 */
class StatsAggregatorTest {
    private val nowMs = 1745842800000L  // 2026-04-28 11:00:00 UTC (placeholder; recomputed below)
    // Use a deterministic value that maps cleanly: 2026-04-28T12:00:00Z.
    private val now = LocalDateTime.of(2026, 4, 28, 12, 0, 0)
        .toInstant(ZoneOffset.UTC).toEpochMilli()

    @Test
    fun empty_input_yields_zero_view() {
        val v = StatsAggregator.aggregate(emptyList<String>().iterator(), now, ZoneOffset.UTC)
        assertEquals(0, v.playCount)
        assertEquals(0, v.skipCount)
        assertEquals(0L, v.totalListenedMs)
        assertEquals(0, v.uniqueTrackCount)
        assertTrue(v.topPlayed.isEmpty())
        // Heatmap dimensions present, all zero.
        assertEquals(7, v.heatmap.size)
        v.heatmap.forEach { row -> assertEquals(24, row.size); row.forEach { assertEquals(0L, it) } }
        assertEquals(7, v.heatmapNormalized.size)
        v.heatmapNormalized.forEach { row -> assertEquals(24, row.size); row.forEach { assertEquals(0f, it, 0f) } }
    }

    @Test
    fun three_plays_and_one_skip_in_window_aggregate_correctly() {
        // All in window. Two distinct tracks.
        // Track A played twice (60_000 + 60_000 ms), Track B once (90_000 ms), B skipped once (10_000 ms).
        val lines = listOf(
            "{\"v\":1,\"ts\":${now - 1000},\"kind\":\"play\",\"track\":\"a\",\"played_ms\":60000,\"duration_ms\":120000}",
            "{\"v\":1,\"ts\":${now - 2000},\"kind\":\"play\",\"track\":\"b\",\"played_ms\":90000,\"duration_ms\":180000}",
            "{\"v\":1,\"ts\":${now - 3000},\"kind\":\"play\",\"track\":\"a\",\"played_ms\":60000,\"duration_ms\":120000}",
            "{\"v\":1,\"ts\":${now - 4000},\"kind\":\"skip\",\"track\":\"b\",\"played_ms\":10000,\"duration_ms\":180000}",
        )
        val v = StatsAggregator.aggregate(lines.iterator(), now, ZoneOffset.UTC)
        assertEquals(3, v.playCount)
        assertEquals(1, v.skipCount)
        assertEquals(60_000L + 90_000L + 60_000L, v.totalListenedMs)
        assertEquals(2, v.uniqueTrackCount)
        // topPlayed: A (2 plays) ahead of B (1 play).
        assertEquals(2, v.topPlayed.size)
        assertEquals("a", v.topPlayed[0].trackId)
        assertEquals(2, v.topPlayed[0].playCount)
        assertEquals("b", v.topPlayed[1].trackId)
        assertEquals(1, v.topPlayed[1].playCount)
    }

    @Test
    fun line_before_window_start_is_ignored() {
        val before = now - 8L * 24L * 60L * 60L * 1000L  // 8 days back
        val lines = listOf(
            "{\"v\":1,\"ts\":$before,\"kind\":\"play\",\"track\":\"old\",\"played_ms\":60000,\"duration_ms\":120000}",
            "{\"v\":1,\"ts\":${now - 1000},\"kind\":\"play\",\"track\":\"new\",\"played_ms\":60000,\"duration_ms\":120000}",
        )
        val v = StatsAggregator.aggregate(lines.iterator(), now, ZoneOffset.UTC)
        assertEquals(1, v.playCount)
        assertEquals(1, v.uniqueTrackCount)
        assertEquals("new", v.topPlayed.single().trackId)
    }

    @Test
    fun malformed_line_is_skipped_silently() {
        val lines = listOf(
            "not-json",
            "",
            "{\"v\":1,\"ts\":${now - 1000},\"kind\":\"play\",\"track\":\"a\",\"played_ms\":60000,\"duration_ms\":120000}",
            "{partial",
        )
        val v = StatsAggregator.aggregate(lines.iterator(), now, ZoneOffset.UTC)
        assertEquals(1, v.playCount)
        assertEquals(60_000L, v.totalListenedMs)
    }

    @Test
    fun all_time_mode_includes_old_events_outside_default_window() {
        // ts = 0 (epoch) is far older than now - WEEK_MS, so the default
        // window would reject it; allTime = true must keep it.
        val line =
            "{\"v\":1,\"ts\":0,\"kind\":\"play\",\"track\":\"old-track\",\"played_ms\":60000,\"duration_ms\":120000}"
        val v = StatsAggregator.aggregate(
            lines = listOf(line).iterator(),
            nowMs = now,
            zone = ZoneOffset.UTC,
            allTime = true,
        )
        assertEquals(1, v.playCount)
        assertEquals(1, v.uniqueTrackCount)
        assertEquals(1, v.perTrackPlays["old-track"])
    }

    @Test
    fun per_track_last_ts_exposed_on_view_and_uses_max_observed_ts() {
        // Three plays for "a" at ts 100, 200, 50 — max is 200.
        val nowFar = now  // far in the future, so window includes 50/100/200.
        // But the default window is 7 days; ts values 50/100/200 are far older
        // than now - WEEK_MS, so we use allTime = true.
        val lines = listOf(
            "{\"v\":1,\"ts\":100,\"kind\":\"play\",\"track\":\"a\",\"played_ms\":1000,\"duration_ms\":2000}",
            "{\"v\":1,\"ts\":200,\"kind\":\"play\",\"track\":\"a\",\"played_ms\":1000,\"duration_ms\":2000}",
            "{\"v\":1,\"ts\":50,\"kind\":\"play\",\"track\":\"a\",\"played_ms\":1000,\"duration_ms\":2000}",
        )
        val v = StatsAggregator.aggregate(
            lines = lines.iterator(),
            nowMs = nowFar,
            zone = ZoneOffset.UTC,
            allTime = true,
        )
        assertEquals(3, v.playCount)
        assertEquals(200L, v.perTrackLastTs["a"])
    }

    @Test
    fun heatmap_cell_math_30min_play_at_local_14_00_lights_today_hour14() {
        // Play at 2026-04-28 14:00 in the test zone (UTC). Today (last day in 7d window) → dayIdx 6, hourIdx 14.
        val ts = LocalDateTime.of(2026, 4, 28, 14, 0, 0)
            .toInstant(ZoneOffset.UTC).toEpochMilli()
        val playedMs = 30L * 60L * 1000L  // 30 minutes
        // Make sure `now` is later than the play so the 7d window includes it.
        val nowLater = LocalDateTime.of(2026, 4, 28, 23, 59, 0)
            .toInstant(ZoneOffset.UTC).toEpochMilli()
        val line =
            "{\"v\":1,\"ts\":$ts,\"kind\":\"play\",\"track\":\"x\",\"played_ms\":$playedMs,\"duration_ms\":1800000}"
        val v = StatsAggregator.aggregate(listOf(line).iterator(), nowLater, ZoneOffset.UTC)
        assertEquals(1, v.playCount)
        // Today is dayIdx 6 (last row); 14:00 → hourIdx 14.
        assertEquals(playedMs, v.heatmap[6][14])
        assertEquals(1.0f, v.heatmapNormalized[6][14], 0.0001f)
        // All other cells must be zero.
        for (d in 0..6) {
            for (h in 0..23) {
                if (d == 6 && h == 14) continue
                assertEquals(0L, v.heatmap[d][h])
                assertEquals(0f, v.heatmapNormalized[d][h], 0f)
            }
        }
        assertNotNull(v.windowStartMs)
    }
}
