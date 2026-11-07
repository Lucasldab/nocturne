package io.nocturne.phone.ui.player

import androidx.annotation.OptIn
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SwipeToDismissBox
import androidx.compose.material3.SwipeToDismissBoxValue
import androidx.compose.material3.Text
import androidx.compose.material3.rememberSwipeToDismissBoxState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.zIndex
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.Timeline
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaController
import io.nocturne.phone.player.PlayerViewModel
import io.nocturne.phone.player.QueueOps
import io.nocturne.phone.player.entryId
import io.nocturne.phone.player.queuedBy
import io.nocturne.phone.ui.theme.JetBrainsMono
import io.nocturne.phone.ui.theme.NocturnePrimary

/** One read of the controller's queue, in play order. */
private data class QueueSnap(
    val items: List<MediaItem>,
    val keys: List<String>,
    val current: Int?,
    val history: List<Int>,
    val upcoming: List<Int>,
    val shuffle: Boolean,
) {
    fun indexOfKey(key: String): Int = keys.indexOf(key)

    companion object {
        val EMPTY = QueueSnap(emptyList(), emptyList(), null, emptyList(), emptyList(), false)
    }
}

private fun snapshotOf(controller: MediaController): QueueSnap {
    val count = controller.mediaItemCount
    if (count == 0) return QueueSnap.EMPTY
    val items = (0 until count).map { controller.getMediaItemAt(it) }
    val shuffle = controller.shuffleModeEnabled
    val timeline = controller.currentTimeline
    val order = if (timeline.windowCount == count) {
        QueueOps.playOrder(count, timeline.getFirstWindowIndex(shuffle)) { i ->
            timeline.getNextWindowIndex(i, Player.REPEAT_MODE_OFF, shuffle)
        }
    } else {
        (0 until count).toList()
    }
    val split = QueueOps.split(order, controller.currentMediaItemIndex)
    return QueueSnap(
        items = items,
        keys = QueueOps.entryKeys(items.map { it.mediaId }, items.map { it.entryId() }),
        current = split.current,
        history = split.history,
        upcoming = split.upcoming,
        shuffle = shuffle,
    )
}

/**
 * Now Playing queue, in play order (follows shuffle):
 *
 *   ↑ 1712 played            ← collapsed history; tap to show / hide
 *   ▸ Money Honey            ← now: accent bar + accent title
 *   ── up next · 3 ──
 *   + Halfway Right     ≡    ← "+" = queued by hand (play next / add to queue)
 *     Controllah        ≡    ← rest of the context the queue was built from
 *
 * Per row: tap → play it · long-press → [onLongPressTrack] (unpin / delete
 * sheet) · swipe → remove (not on the current row) · drag ≡ → reorder (up
 * next only, and only with shuffle off — with shuffle on the list order is
 * not the play order, so a move would not mean what it shows).
 */
@OptIn(UnstableApi::class)
@Composable
fun QueueSection(
    controller: MediaController,
    modifier: Modifier = Modifier,
    playerVm: PlayerViewModel? = null,
    onLongPressTrack: ((trackId: String, displayTitle: String) -> Unit)? = null,
) {
    var snap by remember { mutableStateOf(QueueSnap.EMPTY) }
    DisposableEffect(controller) {
        snap = snapshotOf(controller)
        val listener = object : Player.Listener {
            override fun onTimelineChanged(timeline: Timeline, reason: Int) { snap = snapshotOf(controller) }
            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) { snap = snapshotOf(controller) }
            override fun onShuffleModeEnabledChanged(shuffleModeEnabled: Boolean) { snap = snapshotOf(controller) }
        }
        controller.addListener(listener)
        onDispose { controller.removeListener(listener) }
    }
    val latest by rememberUpdatedState(snap)

    var showHistory by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()
    val drag = remember { QueueDrag(listState) }
    val canReorder = !snap.shuffle

    // Back to the top (= the current track) whenever the track changes.
    LaunchedEffect(snap.current) {
        if (!showHistory) listState.scrollToItem(0)
    }

    fun removeKey(key: String) {
        val idx = latest.indexOfKey(key)
        if (idx >= 0) controller.removeMediaItem(idx)
    }

    Column(modifier = modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(top = 24.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                "QUEUE",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
            )
            if (snap.shuffle) {
                Text(
                    "shuffled",
                    style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
                    color = MaterialTheme.colorScheme.primary,
                )
            }
        }
        val upcoming: List<Int> = drag.order ?: snap.upcoming
        LazyColumn(state = listState, modifier = Modifier.fillMaxSize()) {
            if (snap.history.isNotEmpty()) {
                item(key = "history-toggle") {
                    Text(
                        text = if (showHistory) "↓ hide played" else "↑ ${snap.history.size} played",
                        style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { showHistory = !showHistory }
                            .padding(horizontal = 16.dp, vertical = 10.dp),
                    )
                }
                if (showHistory) {
                    for (idx in snap.history) {
                        val key = snap.keys[idx]
                        val item = snap.items[idx]
                        item(key = key) {
                            SwipeRemovable(onRemove = { removeKey(key) }) {
                                QueueRow(
                                    item = item,
                                    playerVm = playerVm,
                                    kind = RowKind.Played,
                                    onTap = { controller.seekToDefaultPosition(idx); controller.play() },
                                    onLongPress = onLongPressTrack,
                                )
                            }
                        }
                    }
                }
            }
            snap.current?.let { idx ->
                val item = snap.items[idx]
                item(key = "now:" + snap.keys[idx]) {
                    // Never swipeable: removing the playing track is a skip+drop.
                    QueueRow(
                        item = item,
                        playerVm = playerVm,
                        kind = RowKind.Current,
                        onTap = { controller.play() },
                        onLongPress = onLongPressTrack,
                    )
                }
            }
            item(key = "next-label") {
                Text(
                    text = if (upcoming.isEmpty()) "── end of queue ──" else "── up next · ${upcoming.size} ──",
                    style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                )
            }
            for (idx in upcoming) {
                val key = snap.keys.getOrNull(idx) ?: continue
                val item = snap.items[idx]
                val dragging = drag.key == key
                item(key = key) {
                    Box(
                        modifier = if (dragging) {
                            Modifier
                                .zIndex(1f)
                                .graphicsLayer { translationY = drag.offset }
                                .background(MaterialTheme.colorScheme.surfaceVariant)
                        } else {
                            Modifier.animateItem()
                        },
                    ) {
                        SwipeRemovable(onRemove = { removeKey(key) }) {
                            QueueRow(
                                item = item,
                                playerVm = playerVm,
                                kind = RowKind.Upcoming,
                                onTap = {
                                    val live = latest.indexOfKey(key)
                                    if (live >= 0) { controller.seekToDefaultPosition(live); controller.play() }
                                },
                                onLongPress = onLongPressTrack,
                                reorder = if (canReorder) {
                                    RowDrag(
                                        key = key,
                                        onStart = { drag.start(key, latest) },
                                        onDrag = { dy -> drag.dragBy(dy, latest) },
                                        onEnd = { drag.finish(latest, controller) },
                                        onCancel = { drag.cancel() },
                                    )
                                } else null,
                            )
                        }
                    }
                }
            }
        }
    }
}

/**
 * Drag-to-reorder state for the up-next rows. While a drag runs, [order] is a
 * local copy of the up-next list indices that is permuted as the row crosses
 * its neighbours; the controller sees a single moveMediaItem on release.
 */
private class QueueDrag(private val listState: LazyListState) {
    var key by mutableStateOf<String?>(null)
        private set
    var offset by mutableFloatStateOf(0f)
        private set
    var order by mutableStateOf<List<Int>?>(null)
        private set
    private var fromIndex = -1

    fun start(key: String, snap: QueueSnap) {
        val idx = snap.indexOfKey(key)
        if (idx < 0) return
        this.key = key
        fromIndex = idx
        offset = 0f
        order = mutableStateListOf<Int>().apply { addAll(snap.upcoming) }
    }

    fun dragBy(dy: Float, snap: QueueSnap) {
        val k = key ?: return
        val list = order as? MutableList<Int> ?: return
        offset += dy
        val visible = listState.layoutInfo.visibleItemsInfo
        val cur = visible.firstOrNull { it.key == k } ?: return
        val mid = cur.offset + offset + cur.size / 2f
        val target = visible.firstOrNull { info ->
            info.key != k && info.key is String && mid >= info.offset && mid <= info.offset + info.size &&
                snap.indexOfKey(info.key as String).let { it >= 0 && it in list }
        } ?: return
        val from = list.indexOf(snap.indexOfKey(k))
        val to = list.indexOf(snap.indexOfKey(target.key as String))
        if (from < 0 || to < 0) return
        list.add(to, list.removeAt(from))
        offset += (cur.offset - target.offset).toFloat()
    }

    fun finish(snap: QueueSnap, controller: MediaController) {
        val list = order
        val k = key
        if (list != null && k != null && fromIndex >= 0) {
            val pos = list.indexOf(snap.indexOfKey(k))
            // Shuffle off: up next is the contiguous run after the current item.
            val first = snap.upcoming.minOrNull()
            if (pos >= 0 && first != null) {
                val newIndex = first + pos
                if (newIndex != fromIndex) controller.moveMediaItem(fromIndex, newIndex)
            }
        }
        cancel()
    }

    fun cancel() {
        key = null
        order = null
        offset = 0f
        fromIndex = -1
    }
}

@Composable
private fun SwipeRemovable(onRemove: () -> Unit, content: @Composable () -> Unit) {
    // confirmValueChange can fire more than once per swipe. With duplicate
    // tracks a second call would resolve the same row key to the *other*
    // copy and remove that one too, so remove at most once per row.
    var removed by remember { mutableStateOf(false) }
    val state = rememberSwipeToDismissBoxState(
        confirmValueChange = { value ->
            if (value != SwipeToDismissBoxValue.Settled) {
                if (!removed) {
                    removed = true
                    onRemove()
                }
                true
            } else false
        },
    )
    SwipeToDismissBox(
        state = state,
        backgroundContent = { SwipeBackground(state.dismissDirection) },
        content = { content() },
    )
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

private enum class RowKind { Played, Current, Upcoming }

/** Drag-handle callbacks for one up-next row ([key] restarts the gesture). */
private class RowDrag(
    val key: String,
    val onStart: () -> Unit,
    val onDrag: (dy: Float) -> Unit,
    val onEnd: () -> Unit,
    val onCancel: () -> Unit,
)

@OptIn(UnstableApi::class)
@Composable
private fun QueueRow(
    item: MediaItem,
    playerVm: PlayerViewModel?,
    kind: RowKind,
    onTap: () -> Unit,
    onLongPress: ((trackId: String, displayTitle: String) -> Unit)?,
    reorder: RowDrag? = null,
) {
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
    val title = dbTitle ?: item.mediaMetadata.title?.toString().orEmpty()
    val artist = dbArtist ?: item.mediaMetadata.artist?.toString().orEmpty()
    val isCurrent = kind == RowKind.Current
    val handQueued = item.queuedBy() != null

    val gesture: Modifier = if (onLongPress != null) {
        Modifier.pointerInput(item.mediaId, onTap) {
            detectTapGestures(
                onTap = { onTap() },
                onLongPress = { onLongPress(item.mediaId, title) },
            )
        }
    } else {
        Modifier.clickable(onClick = onTap)
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.background)
            .then(gesture)
            .then(
                if (isCurrent) Modifier.drawBehind {
                    drawRect(
                        color = NocturnePrimary,
                        topLeft = Offset.Zero,
                        size = Size(2.dp.toPx(), this.size.height),
                    )
                } else Modifier,
            )
            .padding(start = 16.dp, top = 8.dp, bottom = 8.dp)
            .alpha(if (kind == RowKind.Played) 0.55f else 1f),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Leading glyph column: ▸ now · + queued by hand · blank otherwise.
        Text(
            text = when {
                isCurrent -> "▸"
                handQueued -> "+"
                else -> ""
            },
            style = MaterialTheme.typography.bodySmall.copy(fontFamily = JetBrainsMono),
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.width(16.dp),
        )
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyMedium,
                color = if (isCurrent) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
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
        if (reorder != null) {
            DragHandle(
                Modifier.pointerInput(reorder.key) {
                    detectDragGestures(
                        onDragStart = { reorder.onStart() },
                        onDrag = { change, amount ->
                            change.consume()
                            reorder.onDrag(amount.y)
                        },
                        onDragEnd = { reorder.onEnd() },
                        onDragCancel = { reorder.onCancel() },
                    )
                },
            )
        } else {
            Spacer(Modifier.width(16.dp))
        }
    }
}

/** "≡" grip, 48dp touch target; [modifier] carries the drag gesture. */
@Composable
private fun DragHandle(modifier: Modifier = Modifier) {
    Box(
        modifier = modifier.size(48.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = "≡",
            style = MaterialTheme.typography.titleMedium.copy(fontFamily = JetBrainsMono),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
