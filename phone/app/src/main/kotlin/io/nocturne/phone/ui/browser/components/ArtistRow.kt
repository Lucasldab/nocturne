package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.nocturne.phone.data.db.entity.ArtistEntity
import io.nocturne.phone.ui.theme.NocturneTheme

@Composable
fun ArtistRow(artist: ArtistEntity, onTap: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onTap)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = artist.name,
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = "${artist.albumCount} album${if (artist.albumCount == 1) "" else "s"} · ${artist.trackCount} tracks",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        // design pass2026-04-27: artist row trailing pill shows resident
        // / total track ratio. Empty here at the row level (we don't know
        // residency without an extra DAO query); the overall trackCount is
        // still useful as a tertiary at-a-glance metric.
        Text(
            text = "${artist.trackCount}",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun ArtistRowPreview() {
    NocturneTheme {
        ArtistRow(
            artist = ArtistEntity(
                id = "0".repeat(64),
                name = "Sample Artist",
                albumCount = 4,
                trackCount = 53,
            ),
            onTap = {},
        )
    }
}
