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
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.ui.browser.components.AlbumRow
import io.nocturne.phone.ui.browser.components.LetterScrollRail
import io.nocturne.phone.ui.browser.components.SectionLabel

@Composable
fun AlbumsScreen(
    vm: BrowserViewModel,
    onNavigate: (String) -> Unit,
    modifier: Modifier = Modifier,
    container: AppContainer? = null,
) {
    // Quick task 260430-vtb (Bug 1): swapped from Pager → non-paged StateFlow.
    // Pager+scrollToItem(target) silently swallowed seeks past the loaded
    // window; non-paged List lets LetterScrollRail snap to any letter.
    val albumsList by vm.albumsAll.collectAsStateWithLifecycle()
    val listState = rememberLazyListState()
    var letterMap by remember { mutableStateOf<Map<Char, Int>>(emptyMap()) }

    // Re-query letter anchors when the list size changes (initial load +
    // post-import refresh). null container = preview/test render — skip the
    // DAO call. The +1 shift accounts for the SectionLabel header at index 0.
    LaunchedEffect(albumsList.size, container) {
        if (container != null && albumsList.isNotEmpty()) {
            letterMap = container.db.albumDao().letterFirstIndex()
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
                SectionLabel("${albumsList.size} albums")
            }
            items(
                count = albumsList.size,
                key = { idx -> albumsList[idx].id },
                contentType = { "album" },
            ) { idx ->
                val album = albumsList[idx]
                AlbumRow(
                    album = album,
                    onTap = { onNavigate(album.id) },
                    container = container,
                    onUnsync = { vm.unsyncAlbum(album.id) },
                    onDelete = { vm.deleteAlbum(album.id) },
                )
            }
        }
        LetterScrollRail(
            listState = listState,
            letterIndex = letterMap,
            modifier = Modifier.align(Alignment.CenterEnd),
        )
    }
}
