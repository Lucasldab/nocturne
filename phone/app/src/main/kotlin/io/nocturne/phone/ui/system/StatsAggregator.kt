package io.nocturne.phone.ui.system

import io.nocturne.phone.data.stats.StatsEvent
import kotlinx.serialization.SerializationException
import kotlinx.serialization.json.Json
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

/**
 * Stats aggregator (pure JVM).
 *
 * Reads the local-only `stats/phone-<deviceid>.jsonl` produced by Phase 6's
 * StatsWriter and rolls it up into the shape the System / Stats screen
 * renders: hero counts, top-played list, and a 7d × 24h heatmap.
 *
 * Spec discipline (docs/jsonl-spec.md §8): malformed lines are silently
 * skipped; the screen never shows an error toast for a parse failure.
 *
 * The `zone` parameter exists so unit tests can pin to UTC and skip CI tz
 * variability; production callers omit it and get the device's local zone.
 */
data class TopPlayedRow(
    val trackId: String,
    val playCount: Int,
    val lastPlayedMs: Long,
)

data class StatsView(
    val playCount: Int,
    val skipCount: Int,
    val totalListenedMs: Long,
    val uniqueTrackCount: Int,
    val topPlayed: List<TopPlayedRow>,
    val perTrackPlays: Map<String, Int>,
    val perTrackListenedMs: Map<String, Long>,
    val heatmap: Array<LongArray>,        // [day 0..6][hour 0..23]; day 6 = today
    val heatmapNormalized: Array<FloatArray>,
    val windowStartMs: Long,
) {
    companion object {
        fun empty(windowStartMs: Long = 0L): StatsView = StatsView(
            playCount = 0,
            skipCount = 0,
            totalListenedMs = 0L,
            uniqueTrackCount = 0,
            topPlayed = emptyList(),
            perTrackPlays = emptyMap(),
            perTrackListenedMs = emptyMap(),
            heatmap = Array(7) { LongArray(24) },
            heatmapNormalized = Array(7) { FloatArray(24) },
            windowStartMs = windowStartMs,
        )
    }
}

object StatsAggregator {
    // Lenient parser — future spec additions (extra fields) must not crash
    // the screen. Contrast with JsonlFileWriter's strict default Json
    // instance (writer is the source of truth, reader is forgiving).
    private val json = Json { ignoreUnknownKeys = true }

    const val WEEK_MS = 7L * 24L * 60L * 60L * 1000L
    const val MONTH_MS = 30L * 24L * 60L * 60L * 1000L
    const val YEAR_MS = 365L * 24L * 60L * 60L * 1000L

    private const val FUTURE_TOLERANCE_MS = 86_400_000L  // 1 day

    fun aggregate(
        lines: Iterator<String>,
        nowMs: Long,
        zone: ZoneId = ZoneId.systemDefault(),
        windowMs: Long = WEEK_MS,
    ): StatsView {
        val windowStartMs = nowMs - windowMs
        // Anchor day 0 at the local-zone date corresponding to windowStart.
        val windowStartDate: LocalDate =
            Instant.ofEpochMilli(windowStartMs).atZone(zone).toLocalDate()

        val perTrack = HashMap<String, IntArray>()       // count
        val perTrackMs = HashMap<String, Long>()         // listened ms per track
        val perTrackLastTs = HashMap<String, Long>()     // last seen ts
        val unique = HashSet<String>()
        var playCount = 0
        var skipCount = 0
        var totalListenedMs = 0L
        val heatmap = Array(7) { LongArray(24) }

        val futureCutoff = nowMs + FUTURE_TOLERANCE_MS

        while (lines.hasNext()) {
            val raw = lines.next().trim()
            if (raw.isEmpty()) continue
            val ev: StatsEvent = try {
                json.decodeFromString(StatsEvent.serializer(), raw)
            } catch (_: SerializationException) {
                continue
            } catch (_: IllegalArgumentException) {
                // kotlinx.serialization can also throw IAE for malformed input.
                continue
            }
            if (ev.ts < windowStartMs || ev.ts > futureCutoff) continue
            when (ev.kind) {
                "play" -> {
                    playCount += 1
                    totalListenedMs += ev.playedMs
                    unique.add(ev.track)
                    val cnt = perTrack.getOrPut(ev.track) { IntArray(1) }
                    cnt[0] += 1
                    perTrackMs[ev.track] = (perTrackMs[ev.track] ?: 0L) + ev.playedMs
                    val prevTs = perTrackLastTs[ev.track] ?: Long.MIN_VALUE
                    if (ev.ts > prevTs) perTrackLastTs[ev.track] = ev.ts
                    // Heatmap: only "play" events contribute listened time.
                    // Heatmap is fixed 7d × 24h regardless of window — for
                    // longer windows we compress to the last 7 days.
                    val ldt = Instant.ofEpochMilli(ev.ts).atZone(zone).toLocalDateTime()
                    val playedDate = ldt.toLocalDate()
                    val today = Instant.ofEpochMilli(nowMs).atZone(zone).toLocalDate()
                    val daysFromNow = (today.toEpochDay() - playedDate.toEpochDay()).toInt()
                    if (daysFromNow in 0..6) {
                        val dayIdx = (6 - daysFromNow).coerceIn(0, 6)
                        val hourIdx = ldt.hour.coerceIn(0, 23)
                        heatmap[dayIdx][hourIdx] += ev.playedMs
                    }
                }
                "skip" -> {
                    skipCount += 1
                }
                else -> {
                    // ignore (per spec)
                }
            }
        }

        // Build topPlayed: sort by (count desc, lastTs desc), take 10.
        val topPlayed: List<TopPlayedRow> = perTrack.entries
            .map { (id, cnt) -> TopPlayedRow(id, cnt[0], perTrackLastTs[id] ?: 0L) }
            .sortedWith(
                compareByDescending<TopPlayedRow> { it.playCount }
                    .thenByDescending { it.lastPlayedMs },
            )
            .take(10)

        // Normalize heatmap.
        var maxCell = 0L
        for (d in 0..6) for (h in 0..23) if (heatmap[d][h] > maxCell) maxCell = heatmap[d][h]
        val heatmapNormalized = Array(7) { FloatArray(24) }
        if (maxCell > 0L) {
            val divisor = maxCell.toFloat()
            for (d in 0..6) for (h in 0..23) {
                heatmapNormalized[d][h] = heatmap[d][h].toFloat() / divisor
            }
        }

        val perTrackPlays: Map<String, Int> =
            perTrack.entries.associate { (id, cnt) -> id to cnt[0] }
        return StatsView(
            playCount = playCount,
            skipCount = skipCount,
            totalListenedMs = totalListenedMs,
            uniqueTrackCount = unique.size,
            topPlayed = topPlayed,
            perTrackPlays = perTrackPlays,
            perTrackListenedMs = perTrackMs,
            heatmap = heatmap,
            heatmapNormalized = heatmapNormalized,
            windowStartMs = windowStartMs,
        )
    }

    /** Convenience overload for production callers that don't override the zone. */
    fun aggregate(lines: Iterator<String>, nowMs: Long): StatsView =
        aggregate(lines, nowMs, ZoneId.systemDefault())
}
