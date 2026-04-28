package io.nocturne.phone.ui.player

import android.graphics.BitmapFactory
import androidx.annotation.OptIn
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.outlined.FavoriteBorder
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaController
import androidx.media3.ui.compose.material3.buttons.NextButton
import androidx.media3.ui.compose.material3.buttons.PlayPauseButton
import androidx.media3.ui.compose.material3.buttons.PreviousButton
import androidx.media3.ui.compose.material3.buttons.RepeatButton
import androidx.media3.ui.compose.material3.buttons.ShuffleButton
import androidx.media3.ui.compose.material3.indicator.PositionAndDurationText
import androidx.media3.ui.compose.material3.indicator.ProgressSlider
import io.nocturne.phone.player.PlayerViewModel
import io.nocturne.phone.ui.theme.NocturneTheme

/**
 * UI-SPEC Surface 1. Full-screen now-playing surface.
 *
 * Uses the extracted-body pattern from SearchOverlay.kt: the public function
 * reads MediaController state via a Player.Listener, then delegates to the
 * private NowPlayingBody which takes plain data — making it preview-safe.
 *
 * Animation gate: no AnimatedVisibility / animateContentSize / Crossfade.
 * Library-internal motion (PlayPauseButton icon morph, ProgressSlider thumb)
 * is exempt per UI-SPEC Animation Gate.
 */
@OptIn(UnstableApi::class)
@Composable
fun NowPlayingScreen(
    controller: MediaController,
    playerVm: PlayerViewModel,
    onBack: () -> Unit,
) {
    var metadata by remember { mutableStateOf(controller.mediaMetadata) }
    var currentIndex by remember { mutableIntStateOf(controller.currentMediaItemIndex) }

    DisposableEffect(controller, playerVm) {
        // Phase 6: publish the current track id eagerly so isLikedFlow has a
        // value before the first onMediaItemTransition fires.
        playerVm.publishCurrentTrackId(controller.currentMediaItem?.mediaId)
        val listener = object : Player.Listener {
            override fun onMediaMetadataChanged(m: MediaMetadata) { metadata = m }
            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
                currentIndex = controller.currentMediaItemIndex
                playerVm.publishCurrentTrackId(controller.currentMediaItem?.mediaId)
            }
        }
        controller.addListener(listener)
        onDispose { controller.removeListener(listener) }
    }

    val isLiked by playerVm.isLikedFlow.collectAsStateWithLifecycle()
    val currentTrackId = controller.currentMediaItem?.mediaId

    NowPlayingBody(
        controller = controller,
        title = metadata.title?.toString().orEmpty(),
        artist = metadata.artist?.toString().orEmpty(),
        album = metadata.albumTitle?.toString().orEmpty(),
        artworkBytes = metadata.artworkData,
        currentIndex = currentIndex,
        currentTrackId = currentTrackId,
        isLiked = isLiked,
        onToggleLike = { playerVm.toggleLike() },
        onBack = onBack,
    )
}

@OptIn(UnstableApi::class)
@Composable
private fun NowPlayingBody(
    controller: MediaController,
    title: String,
    artist: String,
    album: String,
    artworkBytes: ByteArray?,
    currentIndex: Int,
    currentTrackId: String?,
    isLiked: Boolean,
    onToggleLike: () -> Unit,
    onBack: () -> Unit,
) {
    // Terminal Now Playing per design pass2026-04-27 ratified
    // `nowPlayingVariant: 'terminal'`. Metadata-led, file-info card, sticky
    // bottom transport. Top-row breadcrumb `~/now-playing` reinforces the
    // shell aesthetic. 88dp art (not 300dp) — leaves vertical room for the
    // queue below the file-info block.
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        // Scrollable upper section — back row, hero, file-info, queue all share weight(1f).
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(start = 16.dp, end = 16.dp, top = 20.dp, bottom = 8.dp),
        ) {
            // 1. Top row — back chevron + breadcrumb + heart.
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
                Text(
                    text = "<",
                    style = MaterialTheme.typography.headlineMedium,
                    color = MaterialTheme.colorScheme.onBackground,
                    modifier = Modifier
                        .clickable(onClick = onBack)
                        .padding(end = 6.dp),
                )
                Text(
                    text = "~/now-playing",
                    style = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.weight(1f))
                if (currentTrackId != null) {
                    IconButton(onClick = onToggleLike) {
                        Icon(
                            imageVector = if (isLiked) Icons.Filled.Favorite else Icons.Outlined.FavoriteBorder,
                            contentDescription = if (isLiked) "Unlike track" else "Like track",
                            tint = if (isLiked) {
                                MaterialTheme.colorScheme.primary
                            } else {
                                MaterialTheme.colorScheme.onSurface
                            },
                        )
                    }
                }
            }

            Spacer(Modifier.height(12.dp))

            // 2. Hero row — 88dp art on the left, metadata stack on the right.
            Row(verticalAlignment = Alignment.Top, modifier = Modifier.fillMaxWidth()) {
                Box(
                    modifier = Modifier
                        .size(88.dp)
                        .background(MaterialTheme.colorScheme.surfaceVariant),
                ) {
                    if (artworkBytes != null) {
                        val bitmap = remember(artworkBytes) {
                            BitmapFactory.decodeByteArray(artworkBytes, 0, artworkBytes.size)
                        }
                        if (bitmap != null) {
                            Image(
                                bitmap = bitmap.asImageBitmap(),
                                contentDescription = "Album art",
                                contentScale = ContentScale.Crop,
                                modifier = Modifier.fillMaxSize(),
                            )
                        }
                    }
                }
                Spacer(Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    // ▸ playing eyebrow — primary accent, mono caps, letterspaced
                    Text(
                        text = "▸ playing",
                        style = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace),
                        color = MaterialTheme.colorScheme.primary,
                    )
                    Spacer(Modifier.height(4.dp))
                    Text(
                        text = title.ifEmpty { "no track playing" },
                        style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.onBackground,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Spacer(Modifier.height(6.dp))
                    Text(
                        text = artist,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        text = album,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.padding(top = 2.dp),
                    )
                }
            }

            Spacer(Modifier.height(18.dp))

            // 3. File-info card — bordered metadata table (mono, muted).
            FileInfoCard(controller = controller, currentIndex = currentIndex)

            Spacer(Modifier.height(16.dp))

            // 4. Queue — fills remaining space inside the scrollable upper region.
            QueueSection(
                controller = controller,
                currentIndex = currentIndex,
                modifier = Modifier.weight(1f),
            )
        }

        // 5. Sticky bottom transport block — borderTop + slider + inline transport.
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .border(
                    width = 1.dp,
                    color = MaterialTheme.colorScheme.surfaceVariant,
                )
                .padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 20.dp),
        ) {
            ProgressSlider(player = controller, modifier = Modifier.fillMaxWidth())
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                PositionAndDurationText(player = controller)
            }
            Spacer(Modifier.height(8.dp))
            // Inline transport — shuffle / prev / play / next / repeat in one row.
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                ShuffleButton(player = controller)
                PreviousButton(player = controller)
                PlayPauseButton(player = controller)
                NextButton(player = controller)
                RepeatButton(player = controller)
            }
        }
    }
}

/**
 * File-info card for the terminal Now Playing layout — a bordered metadata
 * table styled like a tiny TUI block:
 *
 *   ┌─────────────────────────────────────┐
 *   │ format    audio                     │
 *   │ track     5/10                      │
 *   │ duration  03:42                     │
 *   │ resident  yes                       │
 *   └─────────────────────────────────────┘
 *
 * Mono throughout, two columns (label / value), 1dp surfaceVariant border.
 * Pulls metadata from the live MediaController — no DAO call needed for v1.
 */
@OptIn(UnstableApi::class)
@Composable
private fun FileInfoCard(controller: MediaController, currentIndex: Int) {
    val mono = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace)
    val mutedColor = MaterialTheme.colorScheme.onSurfaceVariant
    val onSurface = MaterialTheme.colorScheme.onSurface

    val md = controller.mediaMetadata
    val totalCount = controller.mediaItemCount
    val trackNumber = md.trackNumber
    val trackLine = if (trackNumber != null && trackNumber > 0) {
        if (totalCount > 0) "${trackNumber}/${totalCount}" else "${trackNumber}"
    } else if (totalCount > 0) {
        "${currentIndex + 1}/${totalCount}"
    } else {
        "—"
    }

    val durationMs = controller.duration
    val durationLine = if (durationMs > 0L) {
        val total = (durationMs / 1000L).toInt()
        val mm = total / 60
        val ss = total % 60
        "%02d:%02d".format(mm, ss)
    } else "—"

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .border(1.dp, MaterialTheme.colorScheme.surfaceVariant)
            .padding(horizontal = 12.dp, vertical = 10.dp),
    ) {
        FileInfoRow("format", "audio", mono, mutedColor, onSurface)
        FileInfoRow("track", trackLine, mono, mutedColor, onSurface)
        FileInfoRow("duration", durationLine, mono, mutedColor, onSurface)
        // resident: every queued track came from the resident set on this phone
        // (phone never receives archive/ content per Phase 3 design), so 'yes'
        // is the only honest value. Future: bind to TrackEntity.isResident
        // once a DAO query is plumbed.
        FileInfoRow("resident", "yes", mono, mutedColor, onSurface)
    }
}

@Composable
private fun FileInfoRow(
    label: String,
    value: String,
    style: androidx.compose.ui.text.TextStyle,
    labelColor: androidx.compose.ui.graphics.Color,
    valueColor: androidx.compose.ui.graphics.Color,
) {
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 1.dp)) {
        Text(text = label, style = style, color = labelColor, modifier = Modifier.width(80.dp))
        Text(text = value, style = style, color = valueColor)
    }
}

// ---------------------------------------------------------------------------
// Previews -- use the static body pattern so Android Studio can render them
// without a real MediaController. See SearchOverlay.kt for the established
// project convention.
// ---------------------------------------------------------------------------

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun NowPlayingBodyNoArtPreview() {
    NocturneTheme {
        Text(
            text = "NowPlayingScreen preview requires a real MediaController.\nBuild and sideload to preview on-device.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(16.dp),
        )
    }
}
