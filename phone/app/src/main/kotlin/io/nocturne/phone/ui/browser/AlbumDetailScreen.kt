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
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
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
    var fullTrackList by remember { mutableStateOf<List<io.nocturne.phone.data.db.entity.TrackEntity>>(emptyList()) }
    LaunchedEffect(albumId) {
        album = vm.albumById(albumId)
        // Header needs aggregates the AlbumEntity doesn't carry: count of
        // resident tracks + total duration. Pull the full track list once
        // (paging is fine for the LazyColumn below; we need a snapshot for
        // the header). Cheap — single album, ≤ ~20 rows typical.
        fullTrackList = vm.tracksByAlbumList(albumId)
    }
    val tracks by vm.tracksByAlbumState(albumId).collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val container = (ctx.applicationContext as io.nocturne.phone.NocturneApp).container

    // PLAY-10: collect pinnedIdSet once per screen; each TrackRow reads its
    // isPinned state from this shared set (more efficient than per-row flows).
    val pinnedIds by vm.pinnedIdSet.collectAsStateWithLifecycle()

    // SAF-stat-derived download progress for PinnedPulling rows. ViewModel
    // ticks every 5s while subscribed; map lookup is O(1) per row.
    val pullProgress by vm.pinnedDownloadProgress.collectAsStateWithLifecycle()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        // BackButton row — own row, padding 12/16/0 (top/sides/bottom).
        Row(
            modifier = Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "<",
                style = MaterialTheme.typography.headlineMedium,
                color = MaterialTheme.colorScheme.onBackground,
                modifier = Modifier.clickable(onClick = onBack),
            )
        }
        // Album Detail header block (spec).
        album?.let { a ->
            val residentCount = fullTrackList.count { it.isResident }
            val totalSec = fullTrackList.sumOf { (it.durationMs ?: 0L) / 1000L }
            val totalMin = ((totalSec + 30) / 60).toInt()
            io.nocturne.phone.ui.browser.components.AlbumDetailHeader(
                album = a,
                residentTrackCount = residentCount,
                totalDurationMin = totalMin,
                container = container,
            )
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
        LazyColumn(modifier = Modifier.fillMaxSize()) {
            items(
                items = tracks,
                key = { it.id },
                contentType = { "track" },
            ) { t ->
                TrackRow(
                    track = t,
                    isPinned = pinnedIds.contains(t.id),
                    pullProgress = pullProgress.perTrack[t.id],
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
