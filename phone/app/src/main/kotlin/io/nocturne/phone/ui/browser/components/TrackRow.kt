package io.nocturne.phone.ui.browser.components

import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.IntOffset
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.ui.theme.NON_RESIDENT_ALPHA
import io.nocturne.phone.ui.theme.NocturneTheme
import kotlin.math.roundToInt

/**
 * Single track row. Non-resident tracks dim to NON_RESIDENT_ALPHA and gain a
 * PIN chip on the right.
 *
 * Queue actions:
 *  - Tap (resident only) → onTap (typically: play album from this track)
 *  - Tap (non-resident)  → onPinClick (offer to pin)
 *  - Swipe right         → opens a DropdownMenu with "Play next" / "Add to queue"
 *
 * Swipe distance threshold is 80dp; the row visually shifts with the drag and
 * snaps back on release (or open-menu commit).
 */
@Composable
fun TrackRow(
    track: TrackEntity,
    onTap: () -> Unit,
    isCurrentlyPlaying: Boolean = false,
    isPinned: Boolean = false,
    onPinClick: () -> Unit = {},
    onPlayNext: () -> Unit = {},
    onAddToQueue: () -> Unit = {},
) {
    val rowAlpha = if (track.isResident) 1f else NON_RESIDENT_ALPHA
    val accentColor = MaterialTheme.colorScheme.primary
    val density = LocalDensity.current
    val swipeThresholdPx = with(density) { 80.dp.toPx() }
    var dragOffsetPx by remember { mutableFloatStateOf(0f) }
    var menuExpanded by remember { mutableStateOf(false) }
    val animatedOffset by animateDpAsState(
        targetValue = with(density) { dragOffsetPx.toDp() },
        animationSpec = tween(durationMillis = 120),
        label = "trackRowOffset",
    )

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .then(
                if (isCurrentlyPlaying) {
                    Modifier.drawBehind {
                        val w = 2.dp.toPx()
                        drawRect(
                            color = accentColor,
                            topLeft = Offset.Zero,
                            size = androidx.compose.ui.geometry.Size(w, size.height),
                        )
                    }
                } else Modifier,
            ),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .offset { IntOffset(with(density) { animatedOffset.roundToPx() }, 0) }
                .background(MaterialTheme.colorScheme.background)
                .pointerInput(track.isResident) {
                    if (!track.isResident) return@pointerInput
                    detectHorizontalDragGestures(
                        onDragEnd = {
                            if (dragOffsetPx >= swipeThresholdPx) {
                                menuExpanded = true
                            }
                            dragOffsetPx = 0f
                        },
                        onDragCancel = { dragOffsetPx = 0f },
                        onHorizontalDrag = { change, dragAmount ->
                            // Only allow positive drag (right). Negative
                            // movement clamps to 0 so left-flicks don't
                            // shove the row off-screen.
                            dragOffsetPx = (dragOffsetPx + dragAmount).coerceAtLeast(0f)
                            if (dragAmount > 0f) change.consume()
                        },
                    )
                }
                .padding(horizontal = 16.dp, vertical = 8.dp)
                .alpha(rowAlpha),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Row(
                modifier = Modifier
                    .weight(1f)
                    .clickable(onClick = if (track.isResident) onTap else onPinClick),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = track.trackNumber?.toString()?.padStart(2, '0') ?: "  ",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.width(28.dp),
                )
                Spacer(Modifier.width(8.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = track.title,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    val subtitle = buildString {
                        append(track.artist.firstOrNull().orEmpty())
                        if (track.album.isNotBlank()) {
                            if (isNotEmpty()) append(" · ")
                            append(track.album)
                        }
                    }
                    Text(
                        text = subtitle,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            val pinState = when {
                isPinned && !track.isResident -> PinState.PinnedPulling
                isPinned && track.isResident -> PinState.PinnedReady
                else -> PinState.NotPinned
            }
            PinChip(onClick = onPinClick, state = pinState)
        }
        DropdownMenu(
            expanded = menuExpanded,
            onDismissRequest = { menuExpanded = false },
        ) {
            DropdownMenuItem(
                text = { Text("Play next") },
                onClick = {
                    menuExpanded = false
                    onPlayNext()
                },
            )
            DropdownMenuItem(
                text = { Text("Add to queue") },
                onClick = {
                    menuExpanded = false
                    onAddToQueue()
                },
            )
        }
    }
}

private val PREVIEW_TRACK = TrackEntity(
    id = "0".repeat(64),
    title = "Sample Track",
    artist = listOf("Sample Artist"),
    albumArtist = listOf("Sample Artist"),
    album = "Sample Album",
    albumId = "1".repeat(64),
    albumArtistId = "2".repeat(64),
    genre = listOf("Ambient"),
    genreId = "3".repeat(64),
    trackNumber = 4,
    discNumber = 1,
    year = 2024,
    durationMs = 234000L,
    sizeBytes = 5_000_000L,
    format = "flac",
    mtimeNs = 0L,
    dateAdded = "2026-04-27",
    path = "resident/sample.flac",
    isResident = true,
    searchBlob = "sample",
)

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun TrackRowResidentPreview() {
    NocturneTheme {
        TrackRow(track = PREVIEW_TRACK, onTap = {})
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun TrackRowNonResidentNotPinnedPreview() {
    NocturneTheme {
        TrackRow(track = PREVIEW_TRACK.copy(isResident = false), onTap = {}, isPinned = false)
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun TrackRowNonResidentPinnedPreview() {
    NocturneTheme {
        TrackRow(track = PREVIEW_TRACK.copy(isResident = false), onTap = {}, isPinned = true)
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun TrackRowCurrentlyPlayingPreview() {
    NocturneTheme {
        TrackRow(track = PREVIEW_TRACK, onTap = {}, isCurrentlyPlaying = true)
    }
}
