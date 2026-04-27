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
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.paging.compose.collectAsLazyPagingItems
import androidx.paging.compose.itemContentType
import androidx.paging.compose.itemKey
import io.nocturne.phone.data.db.entity.AlbumEntity
import io.nocturne.phone.player.PlayerViewModel
import io.nocturne.phone.ui.browser.components.TrackRow
import kotlinx.coroutines.launch

@Composable
fun AlbumDetailScreen(
    albumId: String,
    vm: BrowserViewModel,
    playerVm: PlayerViewModel,
    onBack: () -> Unit,
    onPlayStarted: () -> Unit,
) {
    var album by remember { mutableStateOf<AlbumEntity?>(null) }
    LaunchedEffect(albumId) { album = vm.albumById(albumId) }
    val tracks = vm.tracksByAlbum(albumId).collectAsLazyPagingItems()
    val scope = rememberCoroutineScope()

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
                    text = album?.title ?: "…",
                    style = MaterialTheme.typography.headlineMedium,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                Text(
                    text = buildString {
                        append(album?.albumArtist?.firstOrNull().orEmpty())
                        if (album?.year != null) {
                            if (isNotEmpty()) append(" · ")
                            append(album!!.year)
                        }
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
        LazyColumn(modifier = Modifier.fillMaxSize()) {
            items(
                count = tracks.itemCount,
                key = tracks.itemKey { it.id },
                contentType = tracks.itemContentType { "track" },
            ) { idx ->
                val t = tracks[idx] ?: return@items
                TrackRow(
                    track = t,
                    onTap = {
                        if (t.isResident) {
                            // Materialise the full album in a coroutine, then play.
                            // rememberCoroutineScope() is lifecycle-safe (cancelled on
                            // recomposition exit — see AppRoot.kt line 86 pattern).
                            scope.launch {
                                val full = vm.tracksByAlbumList(albumId)
                                val start = full.firstOrNull { it.id == t.id } ?: full.firstOrNull()
                                if (start != null) {
                                    playerVm.playAlbumFromTrack(full, start)
                                    onPlayStarted()
                                }
                            }
                        }
                        // Non-resident tap: plan 05-06 wires the pin path. For now no-op.
                    },
                )
            }
        }
    }
}
