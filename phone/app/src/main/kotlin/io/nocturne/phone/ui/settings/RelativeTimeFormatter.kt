package io.nocturne.phone.ui.settings

import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

/**
 * Phase 6 (STATS-06 / UI-SPEC Surface 3): pure JVM relative-time formatter
 * for the SettingsScreen "Last event logged: ..." row.
 *
 * Buckets (per UI-SPEC Copywriting Contract):
 *   < 60s             → "just now"
 *   < 60m             → "1 minute ago" / "N minutes ago"
 *   < 24h             → "1 hour ago" / "N hours ago"
 *   < 48h             → "yesterday"
 *   < 7d              → "N days ago"
 *   >= 7d             → "on YYYY-MM-DD" (ISO 8601, UTC)
 *
 * `nowMs` is parameterized so tests are deterministic. Production uses
 * `System.currentTimeMillis()` via the default value.
 *
 * Negative deltas (epochMs in the future) are clamped to "just now" so a
 * clock-skew event does not crash or render garbage.
 */
object RelativeTimeFormatter {
    private val ISO_DATE: DateTimeFormatter = DateTimeFormatter.ISO_LOCAL_DATE

    fun formatRelativeTime(epochMs: Long, nowMs: Long = System.currentTimeMillis()): String {
        val deltaMs = (nowMs - epochMs).coerceAtLeast(0L)
        val seconds = deltaMs / 1_000L
        val minutes = seconds / 60L
        val hours = minutes / 60L
        val days = hours / 24L
        return when {
            seconds < 60L -> "just now"
            minutes == 1L -> "1 minute ago"
            minutes < 60L -> "$minutes minutes ago"
            hours == 1L -> "1 hour ago"
            hours < 24L -> "$hours hours ago"
            days < 2L -> "yesterday"
            days < 7L -> "$days days ago"
            else -> "on " + ISO_DATE.format(
                Instant.ofEpochMilli(epochMs).atOffset(ZoneOffset.UTC).toLocalDate(),
            )
        }
    }
}
