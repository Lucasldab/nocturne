package io.nocturne.phone.ui.player

import android.graphics.BitmapFactory
import androidx.annotation.OptIn
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
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
    val scrollState = rememberScrollState()
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(horizontal = 16.dp, vertical = 32.dp)
            .verticalScroll(scrollState),
    ) {
        // 1. Back row (Phase 6: trailing-edge heart icon per UI-SPEC Surface 1).
        // Plain `if` show/hide and `if/else` icon swap — both are explicit
        // exemptions per UI-SPEC Animation Gate (instant swap; no fade/morph).
        Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = "<",
                style = MaterialTheme.typography.headlineMedium,
                color = MaterialTheme.colorScheme.onBackground,
                modifier = Modifier
                    .clickable(onClick = onBack)
                    .padding(end = 8.dp),
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

        Spacer(Modifier.height(24.dp))

        // 2. Album art block (300dp) -- UI-SPEC art square
        Box(
            modifier = Modifier
                .size(300.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant)
                .align(Alignment.CenterHorizontally),
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

        Spacer(Modifier.height(24.dp))

        // 3. Track title -- titleMedium, maxLines=2
        Text(
            text = title.ifEmpty { "no track playing" },
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
        // 4. Artist -- bodyMedium, maxLines=1
        Text(
            text = artist,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        // 4b. Album -- bodySmall, maxLines=1
        Text(
            text = album,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(top = 8.dp),
        )

        Spacer(Modifier.height(24.dp))

        // 5. ProgressSlider
        ProgressSlider(player = controller, modifier = Modifier.fillMaxWidth())

        // 6. Position / duration -- right-aligned
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
            PositionAndDurationText(player = controller)
        }

        Spacer(Modifier.height(24.dp))

        // 7. Transport controls -- SpaceEvenly
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            PreviousButton(player = controller)
            PlayPauseButton(player = controller)
            NextButton(player = controller)
        }

        // 8. Secondary controls (Shuffle / Repeat)
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ShuffleButton(player = controller)
            RepeatButton(player = controller)
        }

        Spacer(Modifier.height(24.dp))

        // 9. Queue section
        QueueSection(controller = controller, currentIndex = currentIndex)
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
