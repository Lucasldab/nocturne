package io.nocturne.phone.ui.browser

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
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
import androidx.paging.compose.collectAsLazyPagingItems
import androidx.paging.compose.itemContentType
import androidx.paging.compose.itemKey
import io.nocturne.phone.data.db.entity.ArtistEntity
import io.nocturne.phone.ui.browser.components.AlbumRow
import io.nocturne.phone.ui.browser.components.TrackRow

@Composable
fun ArtistDetailScreen(
    artistId: String,
    vm: BrowserViewModel,
    onBack: () -> Unit,
    onAlbumTap: (String) -> Unit = {},
) {
    var artist by remember { mutableStateOf<ArtistEntity?>(null) }
    LaunchedEffect(artistId) { artist = vm.artistById(artistId) }

    val albums by vm.albumsByArtist(artistId).collectAsStateWithLifecycle(initialValue = emptyList())
    val tracks = vm.tracksByArtist(artistId).collectAsLazyPagingItems()

    // PLAY-10: collect pinnedIdSet once per screen for efficient pin state.
    val pinnedIds by vm.pinnedIdSet.collectAsStateWithLifecycle()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "<",
                style = MaterialTheme.typography.headlineMedium,
                color = MaterialTheme.colorScheme.onBackground,
                modifier = Modifier
                    .clickable(onClick = onBack)
                    .padding(end = 8.dp),
            )
            Spacer(Modifier.width(8.dp))
            Column {
                Text(
                    text = artist?.name ?: "…",
                    style = MaterialTheme.typography.headlineMedium,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                Text(
                    text = "${artist?.albumCount ?: 0} albums · ${artist?.trackCount ?: 0} tracks",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)

        LazyColumn(modifier = Modifier.fillMaxSize()) {
            if (albums.isNotEmpty()) {
                item {
                    Text(
                        text = "ALBUMS",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(start = 16.dp, top = 12.dp, bottom = 4.dp),
                    )
                }
                items(items = albums, key = { it.id }, contentType = { "album" }) { album ->
                    AlbumRow(album = album, onTap = { onAlbumTap(album.id) })
                }
                item { HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant) }
            }

            item {
                Text(
                    text = "TRACKS",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(start = 16.dp, top = 12.dp, bottom = 4.dp),
                )
            }
            items(
                count = tracks.itemCount,
                key = tracks.itemKey { it.id },
                contentType = tracks.itemContentType { "track" },
            ) { idx ->
                val t = tracks[idx] ?: return@items
                TrackRow(
                    track = t,
                    isPinned = pinnedIds.contains(t.id),
                    onTap = {},
                    onPinClick = { vm.pinTrack(t.id) },
                )
            }
        }
    }
}
