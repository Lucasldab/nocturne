package io.nocturne.phone.ui.browser

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.paging.compose.collectAsLazyPagingItems
import androidx.paging.compose.itemContentType
import androidx.paging.compose.itemKey
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.ui.browser.components.LetterScrollRail
import io.nocturne.phone.ui.browser.components.SectionLabel
import io.nocturne.phone.ui.browser.components.TrackRow
import io.nocturne.phone.ui.browser.components.TrackSortToggle

@Composable
fun TracksScreen(
    vm: BrowserViewModel,
    modifier: Modifier = Modifier,
    onTrackTap: (TrackEntity) -> Unit = {},
    onPlayNext: (TrackEntity) -> Unit = {},
    onAddToQueue: (TrackEntity) -> Unit = {},
    container: AppContainer? = null,
) {
    val sortMode by vm.trackSortMode.collectAsStateWithLifecycle()

    // PLAY-10: collect pinnedIdSet once per screen for efficient pin state.
    val pinnedIds by vm.pinnedIdSet.collectAsStateWithLifecycle()
    val pullProgress by vm.pinnedDownloadProgress.collectAsStateWithLifecycle()

    val listState = rememberLazyListState()

    if (sortMode == TrackSortMode.Alphabetical) {
        // -----------------------------------------------------------------
        // Alphabetical: existing paged path + LetterScrollRail.
        //
        // Sort-toggle is added as the first list item; section-label is
        // second. The letter rail's row-index map therefore needs a +2
        // offset (sort-toggle + section-label) to land on the first row of
        // each letter bucket — quick task 260430-po0 used +1 (section-label
        // only); this update preserves the rail's behavior.
        // -----------------------------------------------------------------
        val pagingItems = vm.tracks.collectAsLazyPagingItems()
        var letterMap by remember { mutableStateOf<Map<Char, Int>>(emptyMap()) }

        LaunchedEffect(pagingItems.itemCount, container) {
            if (container != null && pagingItems.itemCount > 0) {
                letterMap = container.db.trackDao().letterFirstIndex()
                    .associate { it.letter.first() to (it.rowIndex + 2) }
            }
        }

        Box(
            modifier = modifier
                .fillMaxSize()
                .background(MaterialTheme.colorScheme.background),
        ) {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize(),
            ) {
                item(key = "sort-toggle", contentType = "sort-toggle") {
                    TrackSortToggle(
                        current = sortMode,
                        onSelect = { vm.setTrackSortMode(it) },
                    )
                }
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
                        pullProgress = pullProgress.perTrack[track.id],
                        onTap = { onTrackTap(track) },
                        onPinClick = { vm.togglePinTrack(track.id) },
                        onPlayNext = { onPlayNext(track) },
                        onAddToQueue = { onAddToQueue(track) },
                        onUnsync = { vm.unsyncTrack(track.id) },
                        onDelete = { vm.deleteTrack(track.id) },
                    )
                }
            }
            LetterScrollRail(
                listState = listState,
                letterIndex = letterMap,
                modifier = Modifier.align(Alignment.CenterEnd),
            )
        }
    } else {
        // -----------------------------------------------------------------
        // MostListened / RecentlyListened / RecentlyDownloaded: non-paged
        // sorted list driven by BrowserViewModel.tracksSorted. Letter rail
        // is intentionally absent — the alpha-sorted row-index buckets do
        // not match these orderings (per scope_decision 7 / spec point 3).
        // -----------------------------------------------------------------
        val sorted by vm.tracksSorted.collectAsStateWithLifecycle()
        Box(
            modifier = modifier
                .fillMaxSize()
                .background(MaterialTheme.colorScheme.background),
        ) {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize(),
            ) {
                item(key = "sort-toggle", contentType = "sort-toggle") {
                    TrackSortToggle(
                        current = sortMode,
                        onSelect = { vm.setTrackSortMode(it) },
                    )
                }
                item(key = "section-label", contentType = "label") {
                    SectionLabel("${sorted.size} tracks")
                }
                items(
                    count = sorted.size,
                    key = { idx -> sorted[idx].id },
                    contentType = { "track" },
                ) { idx ->
                    val track = sorted[idx]
                    TrackRow(
                        track = track,
                        isPinned = pinnedIds.contains(track.id),
                        pullProgress = pullProgress.perTrack[track.id],
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
    }
}
