package io.nocturne.phone.ui.browser

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.paging.compose.collectAsLazyPagingItems
import androidx.paging.compose.itemContentType
import androidx.paging.compose.itemKey
import io.nocturne.phone.ui.browser.components.TrackRow

@Composable
fun TracksScreen(vm: BrowserViewModel, modifier: Modifier = Modifier) {
    val pagingItems = vm.tracks.collectAsLazyPagingItems()
    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        items(
            count = pagingItems.itemCount,
            key = pagingItems.itemKey { it.id },
            contentType = pagingItems.itemContentType { "track" },
        ) { index ->
            val track = pagingItems[index] ?: return@items
            TrackRow(track = track, onTap = { /* Phase 5 wires the player */ })
        }
    }
}
