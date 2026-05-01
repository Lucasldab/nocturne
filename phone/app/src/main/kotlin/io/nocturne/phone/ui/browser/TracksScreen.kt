package io.nocturne.phone.ui.browser

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
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
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
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
        // Alphabetical: non-paged tracksAlphabetical StateFlow + LetterScrollRail.
        //
        // Quick task 260430-vtb (Bug 1 + Bug 3): swapped from Pager so
        // LetterScrollRail.scrollToItem(target) lands on a real row even
        // when the target index is past the would-have-been-loaded paging
        // window, and so cold-start render no longer waits on Pager warm-up.
        //
        // Sort-toggle is the first list item; section-label is second.
        // Letter rail row-index map keeps the +2 offset (sort-toggle +
        // section-label).
        // -----------------------------------------------------------------
        val tracksList by vm.tracksAlphabetical.collectAsStateWithLifecycle()
        var letterMap by remember { mutableStateOf<Map<Char, Int>>(emptyMap()) }

        LaunchedEffect(tracksList.size, container) {
            if (container != null && tracksList.isNotEmpty()) {
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
                // Reserve LetterScrollRail width on the right (alpha branch
                // shows the rail) so trailing PinChip / track count don't
                // render under it.
                contentPadding = PaddingValues(end = 40.dp),
            ) {
                item(key = "sort-toggle", contentType = "sort-toggle") {
                    TrackSortToggle(
                        current = sortMode,
                        onSelect = { vm.setTrackSortMode(it) },
                    )
                }
                item(key = "section-label", contentType = "label") {
                    SectionLabel("${tracksList.size} tracks")
                }
                items(
                    count = tracksList.size,
                    key = { idx -> tracksList[idx].id },
                    contentType = { "track" },
                ) { idx ->
                    val track = tracksList[idx]
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
