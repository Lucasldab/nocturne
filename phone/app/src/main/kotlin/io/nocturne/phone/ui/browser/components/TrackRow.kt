package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.clickable
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
 * PIN chip on the right (Phase 4: chip is visual-only; Phase 6 wires it).
 * Tap is a no-op in Phase 4 — Phase 5 wires the player.
 */
@Composable
fun TrackRow(track: TrackEntity, onTap: () -> Unit) {
    val rowAlpha = if (track.isResident) 1f else NON_RESIDENT_ALPHA
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onTap)
            .padding(horizontal = 16.dp, vertical = 8.dp)
            .alpha(rowAlpha),
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
            Text(
                text = track.artist.firstOrNull().orEmpty(),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (!track.isResident) {
            PinChip(onClick = {})
        }
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
private fun TrackRowNonResidentPreview() {
    NocturneTheme {
        TrackRow(track = PREVIEW_TRACK.copy(isResident = false), onTap = {})
    }
}
