package io.nocturne.phone.ui.browser

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.paging.compose.collectAsLazyPagingItems
import androidx.paging.compose.itemContentType
import androidx.paging.compose.itemKey
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.ui.browser.components.SectionLabel
import io.nocturne.phone.ui.browser.components.TrackRow

@Composable
fun TracksScreen(
    vm: BrowserViewModel,
    modifier: Modifier = Modifier,
    onTrackTap: (TrackEntity) -> Unit = {},
    onPlayNext: (TrackEntity) -> Unit = {},
    onAddToQueue: (TrackEntity) -> Unit = {},
) {
    val pagingItems = vm.tracks.collectAsLazyPagingItems()

    // PLAY-10: collect pinnedIdSet once per screen for efficient pin state.
    val pinnedIds by vm.pinnedIdSet.collectAsStateWithLifecycle()

    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        item(key = "section-label", contentType = "label") {
            SectionLabel("${pagingItems.itemCount} tracks")
        }
        items(
            count = pagingItems.itemCount,
            key = pagingItems.itemKey { it.id },
            contentType = pagingItems.itemContentType { "track" },
        ) { index ->
            val track = pagingItems[index] ?: return@items
            TrackRow(
                track = track,
                isPinned = pinnedIds.contains(track.id),
                onTap = { onTrackTap(track) },
                onPinClick = { vm.togglePinTrack(track.id) },
                onPlayNext = { onPlayNext(track) },
                onAddToQueue = { onAddToQueue(track) },
                onUnsync = { vm.unsyncTrack(track.id) },
                onDelete = { vm.deleteTrack(track.id) },
            )
        }
    }
}
