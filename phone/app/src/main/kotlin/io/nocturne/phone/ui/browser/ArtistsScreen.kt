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
import io.nocturne.phone.ui.browser.components.ArtistRow
import io.nocturne.phone.ui.browser.components.SectionLabel

@Composable
fun ArtistsScreen(
    vm: BrowserViewModel,
    onNavigate: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val pagingItems = vm.artists.collectAsLazyPagingItems()
    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
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
}
