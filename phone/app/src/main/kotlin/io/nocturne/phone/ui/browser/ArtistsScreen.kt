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
    // Quick task 260430-vtb (Bug 1): swapped from Pager → non-paged StateFlow.
    // Pager+scrollToItem silently swallowed seeks past the loaded window;
    // non-paged List lets LetterScrollRail snap to any letter.
    val artistsList by vm.artistsAll.collectAsStateWithLifecycle()
    val listState = rememberLazyListState()
    var letterMap by remember { mutableStateOf<Map<Char, Int>>(emptyMap()) }

    LaunchedEffect(artistsList.size, container) {
        if (container != null && artistsList.isNotEmpty()) {
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
                SectionLabel("${artistsList.size} artists")
            }
            items(
                count = artistsList.size,
                key = { idx -> artistsList[idx].id },
                contentType = { "artist" },
            ) { idx ->
                val artist = artistsList[idx]
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
