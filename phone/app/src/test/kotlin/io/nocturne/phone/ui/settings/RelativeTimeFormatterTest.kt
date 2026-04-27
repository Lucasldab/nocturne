package io.nocturne.phone.ui.settings

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Phase 6 (STATS-06 / UI-SPEC Surface 3): bucket coverage for the
 * SettingsScreen "Last event logged: ..." formatter.
 *
 * NOW is a fixed reference (`1_750_000_000_000L`) so every case is
 * deterministic regardless of test wall-clock. The formatter itself
 * is pure JVM (no Android imports), so this runs without Robolectric.
 */
class RelativeTimeFormatterTest {

    private val NOW = 1_750_000_000_000L  // arbitrary fixed reference

    private fun fmt(deltaMs: Long): String =
        RelativeTimeFormatter.formatRelativeTime(epochMs = NOW - deltaMs, nowMs = NOW)

    @Test fun zero_delta_is_just_now() = assertEquals("just now", fmt(0L))
    @Test fun thirty_seconds_is_just_now() = assertEquals("just now", fmt(30_000L))
    @Test fun fifty_nine_seconds_is_just_now() = assertEquals("just now", fmt(59_000L))
    @Test fun sixty_seconds_is_one_minute() = assertEquals("1 minute ago", fmt(60_000L))
    @Test fun three_minutes() = assertEquals("3 minutes ago", fmt(3L * 60_000L))
    @Test fun fifty_nine_minutes() = assertEquals("59 minutes ago", fmt(59L * 60_000L))
    @Test fun sixty_minutes_is_one_hour() = assertEquals("1 hour ago", fmt(60L * 60_000L))
    @Test fun five_hours() = assertEquals("5 hours ago", fmt(5L * 60L * 60_000L))
    @Test fun twenty_three_hours() = assertEquals("23 hours ago", fmt(23L * 60L * 60_000L))
    @Test fun twenty_four_hours_is_yesterday() = assertEquals("yesterday", fmt(24L * 60L * 60_000L))
    @Test fun forty_seven_hours_is_yesterday() = assertEquals("yesterday", fmt(47L * 60L * 60_000L))
    @Test fun forty_eight_hours_is_two_days() = assertEquals("2 days ago", fmt(48L * 60L * 60_000L))
    @Test fun six_days() = assertEquals("6 days ago", fmt(6L * 24L * 60L * 60_000L))

    @Test fun seven_days_falls_back_to_iso() {
        val sevenDaysAgo = NOW - 7L * 24L * 60L * 60_000L
        val result = RelativeTimeFormatter.formatRelativeTime(epochMs = sevenDaysAgo, nowMs = NOW)
        check(result.startsWith("on ")) { "expected ISO format starting with 'on ', got: $result" }
        check(Regex("^on \\d{4}-\\d{2}-\\d{2}$").matches(result)) {
            "expected 'on YYYY-MM-DD', got: $result"
        }
    }

    @Test fun negative_delta_is_just_now() = assertEquals("just now", fmt(-1_000L))
}
