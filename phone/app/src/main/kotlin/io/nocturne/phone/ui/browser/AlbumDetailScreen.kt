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
import androidx.lifecycle.compose.collectAsStateWithLifecycle
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
    requestPlay: (() -> Unit) -> Unit,
    onBack: () -> Unit,
    onPlayStarted: () -> Unit,
) {
    var album by remember { mutableStateOf<AlbumEntity?>(null) }
    LaunchedEffect(albumId) { album = vm.albumById(albumId) }
    val tracks = vm.tracksByAlbum(albumId).collectAsLazyPagingItems()
    val scope = rememberCoroutineScope()
    val ctx = androidx.compose.ui.platform.LocalContext.current

    // PLAY-10: collect pinnedIdSet once per screen; each TrackRow reads its
    // isPinned state from this shared set (more efficient than per-row flows).
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
                    isPinned = pinnedIds.contains(t.id),
                    onTap = {
                        if (t.isResident) {
                            // route through the AppRoot-hosted
                            // gate so the POST_NOTIFICATIONS rationale appears at most
                            // once per install. The gate may run this lambda
                            // immediately (already-shown / granted / pre-Android-13)
                            // or after the rationale's terminal state resolves.
                            requestPlay {
                                scope.launch {
                                    val full = vm.tracksByAlbumList(albumId)
                                    val start = full.firstOrNull { it.id == t.id } ?: full.firstOrNull()
                                    if (start != null) {
                                        playerVm.playAlbumFromTrack(full, start)
                                        onPlayStarted()
                                    }
                                }
                            }
                        }
                    },
                    onPinClick = { vm.togglePinTrack(t.id) },
                    onPlayNext = {
                        playerVm.playNextTrack(t)
                        android.widget.Toast.makeText(
                            ctx,
                            "Playing next",
                            android.widget.Toast.LENGTH_SHORT,
                        ).show()
                    },
                    onAddToQueue = {
                        playerVm.enqueueTrack(t)
                        android.widget.Toast.makeText(
                            ctx,
                            "Added to queue",
                            android.widget.Toast.LENGTH_SHORT,
                        ).show()
                    },
                    onUnsync = { vm.unsyncTrack(t.id) },
                    onDelete = { vm.deleteTrack(t.id) },
                )
            }
        }
    }
}
