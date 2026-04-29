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
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.ui.browser.components.AlbumRow
import io.nocturne.phone.ui.browser.components.SectionLabel

@Composable
fun AlbumsScreen(
    vm: BrowserViewModel,
    onNavigate: (String) -> Unit,
    modifier: Modifier = Modifier,
    container: AppContainer? = null,
) {
    val pagingItems = vm.albums.collectAsLazyPagingItems()
    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        item(key = "section-label", contentType = "label") {
            SectionLabel("${pagingItems.itemCount} albums")
        }
        items(
            count = pagingItems.itemCount,
            key = pagingItems.itemKey { it.id },
            contentType = pagingItems.itemContentType { "album" },
        ) { index ->
            val album = pagingItems[index] ?: return@items
            AlbumRow(album = album, onTap = { onNavigate(album.id) }, container = container)
        }
    }
}
