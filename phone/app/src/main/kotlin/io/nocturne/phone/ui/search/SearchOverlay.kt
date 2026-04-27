package io.nocturne.phone.ui.search

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.ui.browser.components.TrackRow
import io.nocturne.phone.ui.theme.NocturneTheme
import kotlinx.collections.immutable.ImmutableList
import kotlinx.collections.immutable.persistentListOf

/**
 * Modal-style search surface.
 *
 * Plan 04-07 wires the trigger from BrowserRoot.
 * Plan 05-06 adds pin wiring:
 *   - [pinnedIds] is the shared Set<String> from BrowserViewModel.pinnedIdSet.
 *   - [onPinTrack] routes to BrowserViewModel.pinTrack(trackId) on non-resident tap.
 */
@Composable
fun SearchOverlay(
    container: AppContainer,
    onDismiss: () -> Unit,
    pinnedIds: Set<String> = emptySet(),
    onPinTrack: (String) -> Unit = {},
) {
    val vm: SearchViewModel = viewModel(factory = SearchVMFactory(container))
    val query by vm.query.collectAsStateWithLifecycle()
    val state by vm.results.collectAsStateWithLifecycle()
    SearchOverlayBody(
        query = query,
        state = state,
        onChange = vm::onQueryChange,
        onClear = vm::clear,
        onDismiss = onDismiss,
        pinnedIds = pinnedIds,
        onPinTrack = onPinTrack,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SearchOverlayBody(
    query: String,
    state: SearchResult,
    onChange: (String) -> Unit,
    onClear: () -> Unit,
    onDismiss: () -> Unit,
    pinnedIds: Set<String> = emptySet(),
    onPinTrack: (String) -> Unit = {},
) {
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }

    Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .background(MaterialTheme.colorScheme.background),
        ) {
            TopAppBar(
                title = {
                    Text("Search", style = MaterialTheme.typography.titleMedium)
                },
                navigationIcon = {
                    IconButton(onClick = onDismiss) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = "Close",
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface,
                    titleContentColor = MaterialTheme.colorScheme.onSurface,
                    navigationIconContentColor = MaterialTheme.colorScheme.onSurface,
                ),
            )
            OutlinedTextField(
                value = query,
                onValueChange = onChange,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 8.dp)
                    .focusRequester(focus),
                placeholder = {
                    Text(
                        text = "type to search…",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                textStyle = MaterialTheme.typography.bodyMedium,
                trailingIcon = {
                    if (query.isNotEmpty()) {
                        IconButton(onClick = onClear) {
                            Icon(
                                imageVector = Icons.Filled.Clear,
                                contentDescription = "Clear",
                            )
                        }
                    }
                },
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
            ResultsBody(state, pinnedIds = pinnedIds, onPinTrack = onPinTrack)
        }
    }
}

@Composable
private fun ResultsBody(
    state: SearchResult,
    pinnedIds: Set<String> = emptySet(),
    onPinTrack: (String) -> Unit = {},
) {
    when (state) {
        SearchResult.Idle -> EmptyHint("type to search by title, artist, album, genre")
        SearchResult.Loading -> Box(Modifier.padding(16.dp)) {
            LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        }
        SearchResult.Empty -> EmptyHint("no matches")
        is SearchResult.Results -> LazyColumn(modifier = Modifier.fillMaxSize()) {
            items(items = state.items, key = { it.id }, contentType = { "track" }) { track ->
                TrackRow(
                    track = track,
                    isPinned = pinnedIds.contains(track.id),
                    onTap = { /* P5 wires player for resident tracks */ },
                    onPinClick = { onPinTrack(track.id) },
                )
            }
        }
        is SearchResult.Error -> Text(
            text = state.message,
            color = MaterialTheme.colorScheme.error,
            modifier = Modifier.padding(16.dp),
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun EmptyHint(text: String) {
    Box(modifier = Modifier.fillMaxSize().padding(24.dp)) {
        Text(
            text = text,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private val PREVIEW_TRACKS: ImmutableList<TrackEntity> = persistentListOf(
    TrackEntity(
        id = "0".repeat(64),
        title = "Café Orange",
        artist = listOf("Sigur Rós"),
        albumArtist = listOf("Sigur Rós"),
        album = "Ágætis byrjun",
        albumId = "1".repeat(64),
        albumArtistId = "2".repeat(64),
        genre = listOf("Post-Rock"),
        genreId = "3".repeat(64),
        trackNumber = 4,
        discNumber = 1,
        year = 1999,
        durationMs = 240000L,
        sizeBytes = 5_000_000L,
        format = "flac",
        mtimeNs = 0L,
        dateAdded = "2026-04-27",
        path = "resident/sample.flac",
        isResident = true,
        searchBlob = "cafe orange sigur ros agaetis byrjun post-rock",
    ),
    TrackEntity(
        id = "4".repeat(64),
        title = "Étoile",
        artist = listOf("Daft Punk"),
        albumArtist = listOf("Daft Punk"),
        album = "Discovery",
        albumId = "5".repeat(64),
        albumArtistId = "6".repeat(64),
        genre = listOf("Electronic"),
        genreId = "7".repeat(64),
        trackNumber = 7,
        discNumber = 1,
        year = 2001,
        durationMs = 220000L,
        sizeBytes = 4_000_000L,
        format = "flac",
        mtimeNs = 0L,
        dateAdded = "2026-04-27",
        path = "archive/sample.flac",
        isResident = false,
        searchBlob = "etoile daft punk discovery electronic",
    ),
)

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun SearchOverlayIdlePreview() {
    NocturneTheme {
        SearchOverlayBody(query = "", state = SearchResult.Idle, onChange = {}, onClear = {}, onDismiss = {})
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun SearchOverlayLoadingPreview() {
    NocturneTheme {
        SearchOverlayBody(query = "cafe", state = SearchResult.Loading, onChange = {}, onClear = {}, onDismiss = {})
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun SearchOverlayResultsPreview() {
    NocturneTheme {
        SearchOverlayBody(
            query = "cafe",
            state = SearchResult.Results(PREVIEW_TRACKS),
            onChange = {}, onClear = {}, onDismiss = {},
        )
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun SearchOverlayEmptyPreview() {
    NocturneTheme {
        SearchOverlayBody(query = "qzzz", state = SearchResult.Empty, onChange = {}, onClear = {}, onDismiss = {})
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun SearchOverlayErrorPreview() {
    NocturneTheme {
        SearchOverlayBody(
            query = "boom",
            state = SearchResult.Error("SQLiteException: malformed MATCH expression"),
            onChange = {}, onClear = {}, onDismiss = {},
        )
    }
}
