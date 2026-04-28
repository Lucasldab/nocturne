package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.ui.theme.NON_RESIDENT_ALPHA
import io.nocturne.phone.ui.theme.NocturneTheme

/**
 * Single track row. Non-resident tracks dim to NON_RESIDENT_ALPHA and gain a
 * PIN chip on the right.
 *
 * Phase 5 (plan 05-06):
 *  - [isPinned] drives PinChip's visual state (not-pinned vs pinned-awaiting-sync).
 *  - [onPinClick] is called when the user taps the PinChip on a non-resident row.
 *    For resident rows no chip is shown; for queue-context call sites the default
 *    no-op is acceptable.
 *  - [onTap] behaviour is unchanged: resident rows play via playAlbumFromTrack;
 *    non-resident rows do NOT play (no file present) — their tap routes to
 *    onPinClick so the user can pin the track.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun TrackRow(
    track: TrackEntity,
    onTap: () -> Unit,
    isCurrentlyPlaying: Boolean = false, // TODO 05-05: render leading 2dp accent border
    isPinned: Boolean = false,
    onPinClick: () -> Unit = {},
    onLongPress: () -> Unit = {},
) {
    val rowAlpha = if (track.isResident) 1f else NON_RESIDENT_ALPHA
    // Click hierarchy: the PinChip OWNS its own click target (via internal
    // `Surface(onClick = ...)`) and must always win over the row's tap. Earlier
    // versions wrapped the entire Row in `combinedClickable`; the chip's
    // onClick stopped firing under some gesture timings because Compose's
    // pointer dispatch sometimes hands the event to the parent click first.
    // Fix: only the LEFT region (track number + title + artist) is clickable
    // for play/long-press. The chip sits as a sibling at the row's edge with
    // its own dedicated tap target. No more click conflict.
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 8.dp)
            .alpha(rowAlpha),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(
            modifier = Modifier
                .weight(1f)
                .combinedClickable(
                    onClick = if (track.isResident) onTap else onPinClick,
                    onLongClick = if (track.isResident) onLongPress else null,
                ),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = track.trackNumber?.toString()?.padStart(2, '0') ?: "  ",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.width(28.dp),
            )
            Spacer(Modifier.width(8.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = track.title,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                // Subtitle = "artist · album" so the Tracks tab disambiguates
                // multi-album tracks (live/studio/compilation versions of the
                // same title). When album is blank, falls back to just artist.
                val subtitle = buildString {
                    append(track.artist.firstOrNull().orEmpty())
                    if (track.album.isNotBlank()) {
                        if (isNotEmpty()) append(" · ")
                        append(track.album)
                    }
                }
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        // PinChip — always visible, four states. Sibling of the clickable row
        // region above, NOT inside it. Tapping the chip fires the chip's own
        // Surface(onClick) cleanly without any nested-clickable conflict.
        val pinState = when {
            isPinned && !track.isResident -> PinState.PinnedPulling
            isPinned && track.isResident -> PinState.PinnedReady
            else -> PinState.NotPinned
        }
        PinChip(onClick = onPinClick, state = pinState)
    }
}

private val PREVIEW_TRACK = TrackEntity(
    id = "0".repeat(64),
    title = "Sample Track",
    artist = listOf("Sample Artist"),
    albumArtist = listOf("Sample Artist"),
    album = "Sample Album",
    albumId = "1".repeat(64),
    albumArtistId = "2".repeat(64),
    genre = listOf("Ambient"),
    genreId = "3".repeat(64),
    trackNumber = 4,
    discNumber = 1,
    year = 2024,
    durationMs = 234000L,
    sizeBytes = 5_000_000L,
    format = "flac",
    mtimeNs = 0L,
    dateAdded = "2026-04-27",
    path = "resident/sample.flac",
    isResident = true,
    searchBlob = "sample",
)

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun TrackRowResidentPreview() {
    NocturneTheme {
        TrackRow(track = PREVIEW_TRACK, onTap = {})
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun TrackRowNonResidentNotPinnedPreview() {
    NocturneTheme {
        TrackRow(track = PREVIEW_TRACK.copy(isResident = false), onTap = {}, isPinned = false)
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun TrackRowNonResidentPinnedPreview() {
    NocturneTheme {
        TrackRow(track = PREVIEW_TRACK.copy(isResident = false), onTap = {}, isPinned = true)
    }
}
