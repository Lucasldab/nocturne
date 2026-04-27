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
import io.nocturne.phone.ui.browser.components.GenreRow

/**
 * Genre detail (per-genre track list) is deferred to v1.x — Phase 4 only
 * lists genres + their track counts. Tap is a no-op for now.
 */
@Composable
fun GenresScreen(vm: BrowserViewModel, modifier: Modifier = Modifier) {
    val pagingItems = vm.genres.collectAsLazyPagingItems()
    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        items(
            count = pagingItems.itemCount,
            key = pagingItems.itemKey { it.id },
            contentType = pagingItems.itemContentType { "genre" },
        ) { index ->
            val genre = pagingItems[index] ?: return@items
            GenreRow(genre = genre, onTap = {})
        }
    }
}
