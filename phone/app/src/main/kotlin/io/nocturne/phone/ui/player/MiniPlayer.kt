package io.nocturne.phone.ui.player

import android.graphics.BitmapFactory
import androidx.annotation.OptIn
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
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
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaController
import androidx.media3.ui.compose.material3.buttons.PlayPauseButton
import io.nocturne.phone.ui.theme.NocturneTheme

/**
 * UI-SPEC Surface 2. Slim 56dp bar mounted above NavigationBar in BrowserRoot.
 *
 * Visibility is gated by the caller:
 *   `if (controller != null && controller.currentMediaItem != null) { MiniPlayer(...) }`
 * -- no AnimatedVisibility (UI-SPEC Animation Gate).
 *
 * PlayPauseButton size exception: 40dp (documented in UI-SPEC Spacing Scale
 * "Mini-player bar height" note -- tight chrome, caller's intent).
 */
@OptIn(UnstableApi::class)
@Composable
fun MiniPlayer(
    controller: MediaController,
    onTap: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var metadata by remember { mutableStateOf(controller.mediaMetadata) }
    DisposableEffect(controller) {
        val listener = object : Player.Listener {
            override fun onMediaMetadataChanged(m: MediaMetadata) { metadata = m }
        }
        controller.addListener(listener)
        onDispose { controller.removeListener(listener) }
    }

    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(56.dp)
            .background(MaterialTheme.colorScheme.surface)
            .clickable(onClick = onTap)
            .padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Art thumbnail 36dp -- surfaceVariant placeholder when art is null
        Box(
            modifier = Modifier
                .size(36.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant),
        ) {
            val artworkBytes = metadata.artworkData
            if (artworkBytes != null) {
                val bitmap = remember(artworkBytes) {
                    BitmapFactory.decodeByteArray(artworkBytes, 0, artworkBytes.size)
                }
                if (bitmap != null) {
                    Image(
                        bitmap = bitmap.asImageBitmap(),
                        contentDescription = null,
                        contentScale = ContentScale.Crop,
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            }
        }
        Spacer(Modifier.width(8.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = metadata.title?.toString().orEmpty(),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = metadata.artist?.toString().orEmpty(),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        // PlayPauseButton: 40dp per UI-SPEC documented exception
        PlayPauseButton(player = controller, modifier = Modifier.size(40.dp))
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF121212)
@Composable
private fun MiniPlayerPreview() {
    NocturneTheme {
        Text(
            text = "MiniPlayer preview requires a real MediaController.\nBuild and sideload to preview on-device.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(16.dp),
        )
    }
}
