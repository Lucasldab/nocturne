package io.nocturne.phone.ui.player

import androidx.annotation.OptIn
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaController
import androidx.media3.ui.compose.material3.buttons.PlayPauseButton
import io.nocturne.phone.ui.theme.NocturneTheme

/**
 * Minimal mini-player per design pass2026-04-27 pass (ratified
 * `miniVariant: 'minimal'`). Single 44dp line above the NavigationBar:
 *
 *   • title · artist                                        ⏵
 *
 * - 6dp purple dot prefix (primary accent)
 * - JetBrains-style mono single line: `title · artist` ellipsised
 * - Trailing PlayPauseButton (36dp)
 * - 1dp top hairline (surfaceVariant) — replaces the bigger 56dp art thumb
 *
 * Visibility is gated by the caller:
 *   `if (controller != null && controller.currentMediaItem != null) { MiniPlayer(...) }`
 * — no AnimatedVisibility (UI-SPEC Animation Gate).
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
            .height(44.dp)
            .background(MaterialTheme.colorScheme.surface)
            .border(1.dp, MaterialTheme.colorScheme.surfaceVariant)
            .clickable(onClick = onTap)
            .padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // 6dp purple dot — accent indicator, replaces the 36dp art thumb.
        Box(
            modifier = Modifier
                .size(6.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.primary),
        )
        Spacer(Modifier.width(10.dp))
        // Single mono line: title · artist (artist muted)
        Text(
            text = buildAnnotatedString {
                withStyle(SpanStyle(color = MaterialTheme.colorScheme.onSurface)) {
                    append(metadata.title?.toString().orEmpty())
                }
                withStyle(SpanStyle(color = MaterialTheme.colorScheme.onSurfaceVariant)) {
                    append(" · ")
                    append(metadata.artist?.toString().orEmpty())
                }
            },
            style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
        PlayPauseButton(player = controller, modifier = Modifier.size(36.dp))
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
