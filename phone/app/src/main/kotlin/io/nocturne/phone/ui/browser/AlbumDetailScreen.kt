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
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.player.PlayerViewModel
import io.nocturne.phone.ui.browser.components.TrackRow
import io.nocturne.phone.ui.player.rememberNotificationPermissionPrompt
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

    // PLAY-10: collect pinnedIdSet once per screen; each TrackRow reads its
    // isPinned state from this shared set (more efficient than per-row flows).
    val pinnedIds by vm.pinnedIdSet.collectAsStateWithLifecycle()

    // UI-SPEC Surface 4: POST_NOTIFICATIONS prompt on FIRST PLAY only.
    // pendingPlay holds the tapped TrackEntity until the permission dialog
    // resolves; then the onProceed lambda queues and starts playback.
    var pendingPlay by remember { mutableStateOf<TrackEntity?>(null) }
    val requestNotifThenPlay = rememberNotificationPermissionPrompt(onProceed = {
        val t = pendingPlay ?: return@rememberNotificationPermissionPrompt
        scope.launch {
            val full = vm.tracksByAlbumList(albumId)
            val start = full.firstOrNull { it.id == t.id } ?: full.firstOrNull()
            if (start != null) {
                playerVm.playAlbumFromTrack(full, start)
                onPlayStarted()
            }
            pendingPlay = null
        }
    })

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
                            // Route through the POST_NOTIFICATIONS prompt on first play;
                            // subsequent calls either skip the prompt (already granted) or
                            // show the rationale dialog before proceeding (UI-SPEC Surface 4).
                            pendingPlay = t
                            requestNotifThenPlay()
                        }
                    },
                    onPinClick = { vm.pinTrack(t.id) },
                )
            }
        }
    }
}
