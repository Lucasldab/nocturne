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
import androidx.paging.compose.collectAsLazyPagingItems
import androidx.paging.compose.itemContentType
import androidx.paging.compose.itemKey
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.ui.browser.components.ArtistRow
import io.nocturne.phone.ui.browser.components.LetterScrollRail
import io.nocturne.phone.ui.browser.components.SectionLabel

@Composable
fun ArtistsScreen(
    vm: BrowserViewModel,
    onNavigate: (String) -> Unit,
    modifier: Modifier = Modifier,
    container: AppContainer? = null,
) {
    val pagingItems = vm.artists.collectAsLazyPagingItems()
    val listState = rememberLazyListState()
    var letterMap by remember { mutableStateOf<Map<Char, Int>>(emptyMap()) }

    LaunchedEffect(pagingItems.itemCount, container) {
        if (container != null && pagingItems.itemCount > 0) {
            letterMap = container.db.artistDao().letterFirstIndex()
                .associate { it.letter.first() to (it.rowIndex + 1) }
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
            item(key = "section-label", contentType = "label") {
                SectionLabel("${pagingItems.itemCount} artists")
            }
            items(
                count = pagingItems.itemCount,
                key = pagingItems.itemKey { it.id },
                contentType = pagingItems.itemContentType { "artist" },
            ) { index ->
                val artist = pagingItems[index] ?: return@items
                ArtistRow(artist = artist, onTap = { onNavigate(artist.id) })
            }
        }
        LetterScrollRail(
            listState = listState,
            letterIndex = letterMap,
            modifier = Modifier.align(Alignment.CenterEnd),
        )
    }
}
