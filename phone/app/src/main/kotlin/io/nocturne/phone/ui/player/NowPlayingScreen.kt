package io.nocturne.phone.ui.player

import android.graphics.BitmapFactory
import androidx.annotation.OptIn
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
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
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontFamily
import io.nocturne.phone.ui.theme.JetBrainsMono
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
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
    var playbackState by remember { mutableIntStateOf(controller.playbackState) }
    var isPlayingNow by remember { mutableStateOf(controller.isPlaying) }
    var hasItem by remember { mutableStateOf(controller.currentMediaItem != null) }

    DisposableEffect(controller, playerVm) {
        // Phase 6: publish the current track id eagerly so isLikedFlow has a
        // value before the first onMediaItemTransition fires.
        playerVm.publishCurrentTrackId(controller.currentMediaItem?.mediaId)
        val listener = object : Player.Listener {
            override fun onMediaMetadataChanged(m: MediaMetadata) { metadata = m }
            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
                currentIndex = controller.currentMediaItemIndex
                hasItem = mediaItem != null
                playerVm.publishCurrentTrackId(controller.currentMediaItem?.mediaId)
            }
            override fun onPlaybackStateChanged(state: Int) { playbackState = state }
            override fun onIsPlayingChanged(playing: Boolean) { isPlayingNow = playing }
        }
        controller.addListener(listener)
        onDispose { controller.removeListener(listener) }
    }

    // Eyebrow text reflects actual transport state. Was hardcoded to "playing"
    // which produced "▸ playing / no track playing" contradictions when the
    // queue exhausted or the service was killed mid-session.
    val eyebrow = when {
        !hasItem -> "idle"
        playbackState == Player.STATE_BUFFERING -> "buffering"
        playbackState == Player.STATE_ENDED -> "ended"
        playbackState == Player.STATE_IDLE -> "idle"
        isPlayingNow -> "playing"
        else -> "paused"
    }

    val isLiked by playerVm.isLikedFlow.collectAsStateWithLifecycle()
    val currentTrackId = controller.currentMediaItem?.mediaId

    // FileInfoCard needs format / size / duration from the DB row (the
    // MediaController metadata only carries title/artist/album).
    var trackEntity by remember(currentTrackId) {
        mutableStateOf<io.nocturne.phone.data.db.entity.TrackEntity?>(null)
    }
    androidx.compose.runtime.LaunchedEffect(currentTrackId) {
        trackEntity = currentTrackId?.let { playerVm.getTrack(it) }
    }

    NowPlayingBody(
        controller = controller,
        playerVm = playerVm,
        title = metadata.title?.toString().orEmpty(),
        artist = metadata.artist?.toString().orEmpty(),
        album = metadata.albumTitle?.toString().orEmpty(),
        artworkBytes = metadata.artworkData,
        currentIndex = currentIndex,
        currentTrackId = currentTrackId,
        currentTrack = trackEntity,
        isLiked = isLiked,
        eyebrow = eyebrow,
        onToggleLike = { playerVm.toggleLike() },
        onBack = onBack,
    )
}

@OptIn(UnstableApi::class)
@Composable
private fun NowPlayingBody(
    controller: MediaController,
    playerVm: PlayerViewModel,
    title: String,
    artist: String,
    album: String,
    artworkBytes: ByteArray?,
    currentIndex: Int,
    currentTrackId: String?,
    currentTrack: io.nocturne.phone.data.db.entity.TrackEntity?,
    isLiked: Boolean,
    eyebrow: String,
    onToggleLike: () -> Unit,
    onBack: () -> Unit,
) {
    // Terminal Now Playing per design pass 2026-04-27 ratified
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
                        .padding(end = 24.dp),
                )
                Text(
                    text = "~/now-playing",
                    style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.weight(1f))
                // Heart icon intentionally hidden 2026-04-28 — like-toggle UX
                // deferred until the design pass settles. Underlying toggleLike
                // wiring on PlayerViewModel is preserved for a future round.
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
                    // Transport-state eyebrow — primary accent, mono caps,
                    // letterspaced. Text comes from the controller listener so
                    // it tracks playing / paused / buffering / ended / idle.
                    Text(
                        text = "▸ $eyebrow",
                        style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
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
            FileInfoCard(controller = controller, currentIndex = currentIndex, track = currentTrack)

            Spacer(Modifier.height(16.dp))

            // 4. Queue — fills remaining space inside the scrollable upper region.
            QueueSection(
                controller = controller,
                currentIndex = currentIndex,
                playerVm = playerVm,
                modifier = Modifier.weight(1f),
            )
        }

        // 5. Sticky bottom transport block.
        //    Top hairline divider only — no side / bottom border, so the block
        //    bleeds into the system gesture inset cleanly. #837A6C matches the
        //    bottom-nav top hairline for a consistent ruled-boundary accent
        //    across the two transport surfaces (260428-ja8 polish pass).
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(androidx.compose.ui.graphics.Color(0xFF837A6C)),
        )
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 20.dp),
        ) {
            NowPlayingProgressBar(controller = controller)
            Spacer(Modifier.height(12.dp))
            // Inline transport — shuffle / prev / play / next / repeat in one row.
            // Center play/pause is wrapped in a 56dp circle so it reads as the
            // primary action, per design.
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                ShuffleButton(player = controller)
                PreviousButton(player = controller)
                Box(
                    modifier = Modifier
                        .size(56.dp)
                        .clip(CircleShape)
                        .background(androidx.compose.ui.graphics.Color(0xFF703490)),
                    contentAlignment = Alignment.Center,
                ) {
                    PlayPauseButton(player = controller)
                }
                NextButton(player = controller)
                RepeatButton(player = controller)
            }
        }
    }
}

/**
 * Custom progress bar for the terminal Now Playing layout.
 *
 *   ┄┄┄┄┄┄■━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┄┄┄┄┄┄┄┄┄┄┄┄
 *   00:34                                                  04:19
 *
 * - 2dp track on surfaceVariant.
 * - primary-colored fill from 0 to progress.
 * - 10dp circular thumb sitting on the seam.
 * - mono 11sp elapsed (left) and duration (right) below the bar.
 *
 * Seekable via drag — pointerInput-translates the local x offset to a position
 * fraction and calls controller.seekTo on release.
 */
@OptIn(UnstableApi::class)
@Composable
private fun NowPlayingProgressBar(controller: MediaController) {
    var positionMs by remember { mutableLongStateOf(controller.currentPosition) }
    var durationMs by remember { mutableLongStateOf(controller.duration) }
    androidx.compose.runtime.LaunchedEffect(controller) {
        while (true) {
            positionMs = controller.currentPosition
            durationMs = controller.duration
            kotlinx.coroutines.delay(500)
        }
    }
    val progress = if (durationMs > 0L) {
        (positionMs.toFloat() / durationMs.toFloat()).coerceIn(0f, 1f)
    } else 0f

    BoxWithConstraints(modifier = Modifier.fillMaxWidth()) {
        val barWidthPx = constraints.maxWidth.toFloat()
        val density = LocalDensity.current
        val thumbOffsetDp = with(density) {
            ((barWidthPx * progress) - 10f /* half thumb size in px is approx; px→dp via density */).coerceAtLeast(0f).toDp()
        }
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(20.dp)
                .pointerInput(durationMs) {
                    detectTapGestures(
                        onTap = { offset ->
                            if (durationMs > 0L) {
                                val frac = (offset.x / barWidthPx).coerceIn(0f, 1f)
                                controller.seekTo((frac * durationMs).toLong())
                            }
                        },
                    )
                },
            contentAlignment = Alignment.CenterStart,
        ) {
            // 2dp surfaceVariant track
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(2.dp)
                    .background(MaterialTheme.colorScheme.surfaceVariant),
            )
            // Primary fill 0..progress — literal #703490 to match the 56dp
            // PlayPauseButton circle exactly (NocturnePrimary 0xFF7E3AA0
            // resolves close but not identical; the design pins #703490 as
            // the load-bearing accent across the now-playing surface).
            Box(
                modifier = Modifier
                    .fillMaxWidth(progress)
                    .height(2.dp)
                    .background(androidx.compose.ui.graphics.Color(0xFF703490)),
            )
            // 10dp circular thumb sitting on the seam
            Box(
                modifier = Modifier
                    .offset(x = thumbOffsetDp)
                    .size(10.dp)
                    .clip(CircleShape)
                    .background(androidx.compose.ui.graphics.Color(0xFF703490)),
            )
        }
    }
    Row(
        modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            text = formatMs(positionMs),
            style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = formatMs(durationMs),
            style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private fun formatMs(ms: Long): String {
    if (ms <= 0L) return "—"
    val total = (ms / 1000L).toInt()
    val mm = total / 60
    val ss = total % 60
    return "%02d:%02d".format(mm, ss)
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
private fun FileInfoCard(
    controller: MediaController,
    currentIndex: Int,
    track: io.nocturne.phone.data.db.entity.TrackEntity?,
) {
    // FileInfoCard locked spec (design pass 2026-04-28 NowPlaying terminal):
    //   border 1px #1A1A1A · padding 10dp all sides · sharp corners
    //   font JetBrains Mono 11sp / lineHeight 1.7em · color #8A8A8A everywhere
    //   (NO label/value contrast — entire card is muted)
    //   5 rows in order: format · size · track · resident · genre
    //   all values lowercase
    val mono = androidx.compose.ui.text.TextStyle(
        fontFamily = JetBrainsMono,
        fontSize = 11.sp,
        lineHeight = (11 * 1.7f).sp,
    )
    val cardColor = androidx.compose.ui.graphics.Color(0xFF8A8A8A)
    val borderColor = androidx.compose.ui.graphics.Color(0xFF1A1A1A)

    // Poll the controller for fields that aren't State-backed: duration is
    // -1 until ExoPlayer prepares the timeline, then settles to the real ms
    // value. mediaItemCount can also lag a transition by a frame.
    var durationMs by remember(currentIndex) { mutableLongStateOf(controller.duration) }
    var mediaItemCount by remember(currentIndex) { mutableIntStateOf(controller.mediaItemCount) }
    androidx.compose.runtime.LaunchedEffect(controller, currentIndex) {
        // Tick every 500ms until duration becomes known (then stop polling).
        // Also refresh on first composition so the value isn't stale.
        repeat(20) {
            durationMs = controller.duration
            mediaItemCount = controller.mediaItemCount
            if (durationMs > 0L) return@LaunchedEffect
            kotlinx.coroutines.delay(500)
        }
    }

    val md = controller.mediaMetadata
    val trackNumber = md.trackNumber
    // Show "N" only when we have a track number but no full album track count
    // to compare against. Showing "9/1" (where 1 = mediaItemCount of a single-
    // item queue) is misleading — drop the denominator unless it's plausibly
    // the album track count.
    val trackLine = if (trackNumber != null && trackNumber > 0) {
        "$trackNumber"
    } else if (mediaItemCount > 1) {
        "${currentIndex + 1}/${mediaItemCount}"
    } else {
        "—"
    }

    // "size" line — human-readable file size from the Room row.
    val sizeLine = run {
        val bytes = track?.sizeBytes ?: 0L
        when {
            bytes <= 0L -> "—"
            bytes < 1_000_000L -> "%d kb".format(bytes / 1000L)
            bytes < 1_000_000_000L -> "%d mb".format(bytes / 1_000_000L)
            else -> "%.1f gb".format(bytes / 1e9)
        }
    }

    // "genre" line — first genre tag, lowercased.
    val genreLine = track?.genre?.firstOrNull()?.lowercase()?.takeIf { it.isNotBlank() } ?: "—"

    // "format" line — codec + computed bitrate. Codec source priority:
    //   1. track.format (Room — populated from catalog.json)
    //   2. file extension parsed off the controller's currentMediaItem URI
    //      (works even when Room lookup races composition or the row is missing)
    //   3. literal "audio" as the last fallback.
    // Bitrate prefers track.durationMs when known (catalog), else falls back to
    // controller.duration (live ExoPlayer reading) once it has settled past the
    // initial -1.
    val formatLine = run {
        val codecFromUri = run {
            val uri = controller.currentMediaItem?.localConfiguration?.uri?.toString()
            uri?.substringAfterLast('.', "")
                ?.substringBefore('?')
                ?.takeIf { it.isNotBlank() && it.length <= 5 }
                ?.lowercase()
        }
        val codec = track?.format?.lowercase() ?: codecFromUri
        val effDurationMs = (track?.durationMs ?: 0L).takeIf { it > 0L }
            ?: durationMs.takeIf { it > 0L }
            ?: 0L
        val effSizeBytes = track?.sizeBytes ?: 0L
        val kbps = if (effSizeBytes > 0L && effDurationMs > 0L) {
            (effSizeBytes * 8L / effDurationMs).toInt()
        } else 0
        when {
            codec != null && kbps > 0 -> "$codec $kbps kbps"
            codec != null -> codec
            kbps > 0 -> "$kbps kbps"
            else -> "audio"
        }
    }
    val residentLine = if (track != null) (if (track.isResident) "yes" else "no") else "yes"

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .border(1.dp, borderColor)
            .padding(10.dp),
    ) {
        FileInfoRow("format", formatLine, mono, cardColor, cardColor)
        FileInfoRow("size", sizeLine, mono, cardColor, cardColor)
        FileInfoRow("track", trackLine, mono, cardColor, cardColor)
        FileInfoRow("resident", residentLine, mono, cardColor, cardColor)
        FileInfoRow("genre", genreLine, mono, cardColor, cardColor)
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
