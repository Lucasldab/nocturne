package io.nocturne.phone.ui.player

import androidx.annotation.OptIn
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SwipeToDismissBox
import androidx.compose.material3.SwipeToDismissBoxValue
import androidx.compose.material3.Text
import androidx.compose.material3.rememberSwipeToDismissBoxState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.Timeline
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaController
import io.nocturne.phone.player.PlayerViewModel
import io.nocturne.phone.ui.theme.NocturnePrimary

/**
 * UI-SPEC Surface 3. LazyColumn of queue items; the row at currentIndex
 * gets a 2dp leading border in primary color via Modifier.drawBehind. No
 * AnimatedVisibility, no animateContentSize.
 *
 * Per-row affordances:
 *   - tap         → seek to that index + play
 *   - long-press  → [onLongPressTrack] (host opens TrackActionsSheet)
 *   - swipe       → remove from queue via controller.removeMediaItem(idx).
 *                   Disabled on the currently-playing row to prevent an
 *                   accidental skip+drop.
 */
@OptIn(UnstableApi::class)
@Composable
fun QueueSection(
    controller: MediaController,
    currentIndex: Int,
    modifier: Modifier = Modifier,
    playerVm: PlayerViewModel? = null,
    onLongPressTrack: ((trackId: String, displayTitle: String) -> Unit)? = null,
) {
    val items = remember { mutableStateOf<List<MediaItem>>(emptyList()) }

    DisposableEffect(controller) {
        // Snapshot the current queue every time anything in the timeline changes.
        fun snapshot() {
            items.value = (0 until controller.mediaItemCount).map { controller.getMediaItemAt(it) }
        }
        snapshot()
        val listener = object : Player.Listener {
            override fun onTimelineChanged(timeline: Timeline, reason: Int) { snapshot() }
            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) { snapshot() }
        }
        controller.addListener(listener)
        onDispose { controller.removeListener(listener) }
    }

    Column(modifier = modifier.fillMaxWidth()) {
        Text(
            "QUEUE",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 24.dp, bottom = 8.dp),
        )
        LazyColumn(modifier = Modifier.fillMaxSize()) {
            items(
                items = items.value,
                key = { it.mediaId },
            ) { item ->
                val idx = items.value.indexOf(item)
                val isCurrent = idx == currentIndex
                // Resolve title/artist from the DB by mediaId — the live
                // MediaItem.mediaMetadata can lag or inherit from the previously
                // active item when ExoPlayer skips a missing-file row.
                var dbTitle by remember(item.mediaId) { mutableStateOf<String?>(null) }
                var dbArtist by remember(item.mediaId) { mutableStateOf<String?>(null) }
                LaunchedEffect(item.mediaId, playerVm) {
                    if (playerVm != null) {
                        val track = playerVm.getTrack(item.mediaId)
                        dbTitle = track?.title
                        dbArtist = track?.artist?.firstOrNull()
                    }
                }
                val displayTitle = dbTitle ?: item.mediaMetadata.title?.toString().orEmpty()
                val displayArtist = dbArtist ?: item.mediaMetadata.artist?.toString().orEmpty()

                val row: @Composable () -> Unit = {
                    QueueRow(
                        title = displayTitle,
                        artist = displayArtist,
                        isCurrent = isCurrent,
                        onTap = {
                            controller.seekToDefaultPosition(idx)
                            controller.play()
                        },
                        onLongPress = onLongPressTrack?.let { cb ->
                            { cb(item.mediaId, displayTitle) }
                        },
                    )
                }

                if (isCurrent) {
                    // Don't allow swiping the actively playing track —
                    // accidental skip+drop is a worse UX than "select another
                    // row first to swipe this one".
                    row()
                } else {
                    val dismissState = rememberSwipeToDismissBoxState(
                        confirmValueChange = { value ->
                            if (value == SwipeToDismissBoxValue.EndToStart ||
                                value == SwipeToDismissBoxValue.StartToEnd) {
                                // Re-read the live index from the controller —
                                // `idx` was captured at composition time and
                                // can be stale by a frame if a prior swipe
                                // already reshaped the queue.
                                val liveIdx = (0 until controller.mediaItemCount)
                                    .firstOrNull { controller.getMediaItemAt(it).mediaId == item.mediaId }
                                if (liveIdx != null) controller.removeMediaItem(liveIdx)
                                true
                            } else false
                        },
                    )
                    SwipeToDismissBox(
                        state = dismissState,
                        backgroundContent = { SwipeBackground(dismissState.dismissDirection) },
                        content = { row() },
                    )
                }
            }
        }
    }
}

@Composable
private fun SwipeBackground(direction: SwipeToDismissBoxValue) {
    val end = direction == SwipeToDismissBoxValue.EndToStart
    val start = direction == SwipeToDismissBoxValue.StartToEnd
    val showLabel = end || start
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(if (showLabel) MaterialTheme.colorScheme.errorContainer else Color.Transparent)
            .padding(horizontal = 16.dp),
        contentAlignment = if (end) Alignment.CenterEnd else Alignment.CenterStart,
    ) {
        if (showLabel) {
            Text(
                text = "$ remove",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onErrorContainer,
            )
        }
    }
}

@Composable
private fun QueueRow(
    title: String,
    artist: String,
    isCurrent: Boolean,
    onTap: () -> Unit,
    onLongPress: (() -> Unit)? = null,
) {
    val baseGesture: Modifier = if (onLongPress != null) {
        Modifier.pointerInput(onTap, onLongPress) {
            detectTapGestures(
                onTap = { onTap() },
                onLongPress = { onLongPress() },
            )
        }
    } else {
        Modifier.clickable(onClick = onTap)
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.background)
            .then(baseGesture)
            .then(
                if (isCurrent) Modifier.drawBehind {
                    drawRect(
                        color = NocturnePrimary,
                        topLeft = Offset.Zero,
                        size = Size(2.dp.toPx(), this.size.height),
                    )
                } else Modifier,
            )
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Spacer(Modifier.width(8.dp))
        Column(modifier = Modifier.fillMaxWidth()) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = artist,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}
