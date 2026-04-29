package io.nocturne.phone.ui.system

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.ui.browser.components.SwipeQueueActions
import io.nocturne.phone.ui.settings.RelativeTimeFormatter
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/**
 * Stats / listening dashboard, now an inline
 * utility-mode content slot (no Scaffold / TopAppBar / back-button — the
 * BrowserRoot shell owns chrome). Mirrors
 * /tmp/nocturne-design/nocturne/project/screens-system.jsx lines 268-308.
 *
 * Hero counts (listened / plays / unique) + 7d × 24h heatmap + top-played
 * list. All data is aggregated by StatsAggregator from the local
 * phone-<deviceid>.jsonl; absent file → all-zero render.
 */
@Composable
fun StatsScreen(
    container: AppContainer,
    onPlayNext: (TrackEntity) -> Unit = {},
    onAddToQueue: (TrackEntity) -> Unit = {},
) {
    val vm: SystemViewModel = viewModel(factory = SystemViewModel.Factory(container))
    LaunchedEffect(Unit) { vm.refreshStats() }
    val view by vm.stats.collectAsStateWithLifecycle()
    val tracks by vm.topPlayedTracks.collectAsStateWithLifecycle()
    val topArtists by vm.topArtists.collectAsStateWithLifecycle()
    val topGenres by vm.topGenres.collectAsStateWithLifecycle()
    val window by vm.statsWindow.collectAsStateWithLifecycle()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(horizontal = 16.dp)
            .padding(bottom = 80.dp)
            .verticalScroll(rememberScrollState()),
    ) {
        TerminalPrompt("~/stats", modifier = Modifier.padding(top = 16.dp))
        ScreenHero(
            when (window) {
                StatsWindow.Week -> "last 7 days"
                StatsWindow.Month -> "last 30 days"
                StatsWindow.Year -> "last 12 months"
            },
        )

        // Window selector — 7d / 30d / 1y. Active tab uses primary color +
        // bracket markers; inactive uses muted text. Mono font matches the
        // shell aesthetic.
        Row(
            horizontalArrangement = Arrangement.spacedBy(20.dp),
            modifier = Modifier.padding(top = 8.dp),
        ) {
            StatsWindow.values().forEach { w ->
                val active = w == window
                Text(
                    text = if (active) "[${w.label}]" else " ${w.label} ",
                    style = monoStyle(12),
                    color = if (active) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                    modifier = Modifier
                        .padding(vertical = 4.dp)
                        .then(
                            if (!active) {
                                Modifier.clickable { vm.setStatsWindow(w) }
                            } else Modifier,
                        ),
                )
            }
        }

        // Hero row: listened / plays / unique
        Row(
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 18.dp),
        ) {
            StatCard(label = "listened", value = formatListened(view.totalListenedMs), modifier = Modifier.weight(1f))
            StatCard(label = "plays",    value = view.playCount.toString(),             modifier = Modifier.weight(1f))
            StatCard(label = "unique",   value = view.uniqueTrackCount.toString(),      modifier = Modifier.weight(1f))
        }

        SectionHeader("heatmap · hour × day")
        Heatmap(view = view)

        SectionHeader("top played")
        if (view.topPlayed.isEmpty()) {
            Text(
                text = "no plays yet",
                style = monoStyle(12),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp),
            )
        } else {
            val maxPlays = view.topPlayed.maxOf { it.playCount }.coerceAtLeast(1)
            view.topPlayed.forEachIndexed { i, row ->
                val track = tracks[row.trackId]
                if (track != null) {
                    SwipeQueueActions(
                        onPlayNext = { onPlayNext(track) },
                        onAddToQueue = { onAddToQueue(track) },
                        enabled = track.isResident,
                    ) {
                        TopPlayedRowView(
                            index = i,
                            row = row,
                            maxPlays = maxPlays,
                            title = track.title,
                            artist = track.artist.firstOrNull() ?: "",
                        )
                    }
                } else {
                    TopPlayedRowView(
                        index = i,
                        row = row,
                        maxPlays = maxPlays,
                        title = row.trackId.take(8) + "…",
                        artist = "",
                    )
                }
            }
        }

        SectionHeader("top artists")
        TopGroupSection(rows = topArtists, emptyText = "no plays yet")

        SectionHeader("top genres")
        TopGroupSection(rows = topGenres, emptyText = "no plays yet")
    }
}

private fun formatListened(totalMs: Long): String {
    if (totalMs <= 0L) return "0m"
    val totalMins = totalMs / 60_000L
    val hours = totalMins / 60L
    val mins = totalMins % 60L
    return when {
        hours == 0L -> "${mins}m"
        hours < 24L -> "${hours}h ${mins}m"
        else -> {
            val days = hours / 24L
            val rem = hours % 24L
            "${days}d ${rem}h"
        }
    }
}

@Composable
private fun TopGroupSection(rows: List<TopGroupRow>, emptyText: String) {
    if (rows.isEmpty()) {
        Text(
            text = emptyText,
            style = monoStyle(12),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 4.dp),
        )
        return
    }
    val maxMs = rows.maxOf { it.listenedMs }.coerceAtLeast(1L)
    rows.forEachIndexed { i, row ->
        Column {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 8.dp),
            ) {
                Text(
                    text = "%02d".format(i + 1),
                    style = monoStyle(11),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.width(22.dp),
                )
                Column(modifier = Modifier.weight(1f).padding(start = 4.dp)) {
                    Text(
                        text = row.label,
                        style = monoStyle(13),
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 1,
                    )
                    Text(
                        text = "${formatListened(row.listenedMs)} · ${row.plays} plays",
                        style = monoStyle(11),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    val frac = (row.listenedMs.toFloat() / maxMs.toFloat()).coerceIn(0f, 1f)
                    Box(
                        modifier = Modifier
                            .width(50.dp)
                            .height(4.dp)
                            .background(MaterialTheme.colorScheme.surfaceVariant),
                    ) {
                        Box(
                            modifier = Modifier
                                .fillMaxWidth(frac)
                                .fillMaxHeight()
                                .background(MaterialTheme.colorScheme.primary),
                        )
                    }
                }
            }
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(1.dp)
                    .background(MaterialTheme.colorScheme.surfaceVariant),
            )
        }
    }
}

@Composable
private fun StatCard(label: String, value: String, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .padding(0.dp)
            .border(1.dp, MaterialTheme.colorScheme.surfaceVariant)
            .padding(10.dp),
    ) {
        Text(
            text = value,
            style = MaterialTheme.typography.titleLarge.copy(
                fontWeight = FontWeight.Bold,
                fontSize = 18.sp,
            ),
            color = MaterialTheme.colorScheme.onBackground,
        )
        Text(
            text = label,
            style = monoStyle(11),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 2.dp),
        )
    }
}

@Composable
private fun Heatmap(view: StatsView) {
    val zone = ZoneId.systemDefault()
    val windowStartDate = Instant.ofEpochMilli(view.windowStartMs).atZone(zone).toLocalDate()
    // LocalConfiguration is observable; survives runtime locale changes without
    // calling Locale.getDefault() inside the composable (NonObservableLocale lint).
    val locale = LocalConfiguration.current.locales[0]
    val weekdayFmt = remember(locale) { DateTimeFormatter.ofPattern("EEE", locale) }
    Column {
        for (d in 0..6) {
            val dayLabel = windowStartDate.plusDays(d.toLong()).format(weekdayFmt)
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 1.dp),
            ) {
                Text(
                    text = dayLabel,
                    style = monoStyle(10),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.width(28.dp),
                )
                Row(
                    horizontalArrangement = Arrangement.spacedBy(1.dp),
                    modifier = Modifier
                        .weight(1f)
                        .padding(start = 4.dp),
                ) {
                    for (h in 0..23) {
                        val v = view.heatmapNormalized[d][h]
                        Box(
                            modifier = Modifier
                                .weight(1f)
                                .height(16.dp)
                                .background(cellColor(v)),
                        )
                    }
                }
            }
        }
        // 24-cell hour label row, every 6th hour shown.
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 6.dp, start = 32.dp),
            horizontalArrangement = Arrangement.spacedBy(1.dp),
        ) {
            for (h in 0..23) {
                Text(
                    text = if (h % 6 == 0) "%02d".format(h) else "",
                    style = monoStyle(9),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}

@Composable
private fun cellColor(v: Float): androidx.compose.ui.graphics.Color {
    val primary = MaterialTheme.colorScheme.primary
    val muted = MaterialTheme.colorScheme.surfaceVariant
    return when {
        v < 0.05f -> muted
        v < 0.25f -> primary.copy(alpha = 0.20f)
        v < 0.50f -> primary.copy(alpha = 0.45f)
        v < 0.75f -> primary.copy(alpha = 0.70f)
        else      -> primary
    }
}

@Composable
private fun TopPlayedRowView(
    index: Int,
    row: TopPlayedRow,
    maxPlays: Int,
    title: String,
    artist: String,
) {
    Column {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 8.dp),
        ) {
            Text(
                text = "%02d".format(index + 1),
                style = monoStyle(11),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.width(22.dp),
            )
            Column(modifier = Modifier.weight(1f).padding(start = 4.dp)) {
                Text(
                    text = title,
                    style = monoStyle(13),
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                )
                Text(
                    text = if (artist.isEmpty()) {
                        RelativeTimeFormatter.formatRelativeTime(row.lastPlayedMs)
                    } else {
                        "$artist · ${RelativeTimeFormatter.formatRelativeTime(row.lastPlayedMs)}"
                    },
                    style = monoStyle(11),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            // Trailing: 50dp x 4dp progress + plays count.
            Row(verticalAlignment = Alignment.CenterVertically) {
                val frac = (row.playCount.toFloat() / maxPlays.toFloat()).coerceIn(0f, 1f)
                Box(
                    modifier = Modifier
                        .width(50.dp)
                        .height(4.dp)
                        .background(MaterialTheme.colorScheme.surfaceVariant),
                ) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth(frac)
                            .fillMaxHeight()
                            .background(MaterialTheme.colorScheme.primary),
                    )
                }
                Text(
                    text = row.playCount.toString(),
                    style = monoStyle(11),
                    color = MaterialTheme.colorScheme.onSurface,
                    textAlign = TextAlign.End,
                    modifier = Modifier.width(22.dp).padding(start = 6.dp),
                )
            }
        }
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant),
        )
    }
}
