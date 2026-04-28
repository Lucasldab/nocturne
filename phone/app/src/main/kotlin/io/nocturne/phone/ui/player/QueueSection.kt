package io.nocturne.phone.ui.player

import androidx.annotation.OptIn
import androidx.compose.foundation.clickable
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
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.Timeline
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaController
import io.nocturne.phone.ui.theme.NocturnePrimary

/**
 * UI-SPEC Surface 3. LazyColumn of queue items; the row at currentIndex
 * gets a 2dp leading border in primary color via Modifier.drawBehind. No
 * AnimatedVisibility, no animateContentSize.
 */
@OptIn(UnstableApi::class)
@Composable
fun QueueSection(
    controller: MediaController,
    currentIndex: Int,
    modifier: Modifier = Modifier,
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
                QueueRow(
                    title = item.mediaMetadata.title?.toString().orEmpty(),
                    artist = item.mediaMetadata.artist?.toString().orEmpty(),
                    isCurrent = idx == currentIndex,
                    onTap = {
                        controller.seekToDefaultPosition(idx)
                        controller.play()
                    },
                )
            }
        }
    }
}

@Composable
private fun QueueRow(
    title: String,
    artist: String,
    isCurrent: Boolean,
    onTap: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onTap)
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
