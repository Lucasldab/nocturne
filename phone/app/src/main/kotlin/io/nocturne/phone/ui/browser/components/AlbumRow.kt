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
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.nocturne.phone.data.db.entity.AlbumEntity
import io.nocturne.phone.ui.theme.NON_RESIDENT_ALPHA
import io.nocturne.phone.ui.theme.NocturneTheme

@Composable
fun AlbumRow(album: AlbumEntity, onTap: () -> Unit) {
    val rowAlpha = if (album.hasResident) 1f else NON_RESIDENT_ALPHA
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onTap)
            .padding(horizontal = 16.dp, vertical = 12.dp)
            .alpha(rowAlpha),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = album.title,
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = buildString {
                    append(album.albumArtist.firstOrNull().orEmpty())
                    if (album.year != null) {
                        if (isNotEmpty()) append(" · ")
                        append(album.year)
                    }
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Text(
            text = "${album.trackCount}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun AlbumRowPreview() {
    NocturneTheme {
        AlbumRow(
            album = AlbumEntity(
                id = "0".repeat(64),
                title = "Sample Album",
                albumArtist = listOf("Sample Artist"),
                albumArtistId = "1".repeat(64),
                year = 2024,
                trackCount = 12,
                totalSizeBytes = 60_000_000L,
                hasResident = true,
            ),
            onTap = {},
        )
    }
}
