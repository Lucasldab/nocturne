package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.nocturne.phone.ui.theme.NocturneTheme

/**
 * Visual-only chip rendered next to non-resident TrackRow / AlbumRow entries.
 * Phase 4: [onClick] is a no-op. Phase 6 wires the pin write through the
 * BrowserViewModel — this composable's contract stays the same; the caller
 * supplies a non-empty lambda then.
 *
 * Compose strong-skipping treats this as skippable since the only argument
 * is a stable `() -> Unit` and the body reads only theme + literals.
 */
@Composable
fun PinChip(onClick: () -> Unit = {}) {
    Surface(
        modifier = Modifier.padding(start = 8.dp),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.onSurfaceVariant),
        color = MaterialTheme.colorScheme.surfaceVariant,
    ) {
        Text(
            text = "PIN",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp),
        )
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun PinChipPreview() {
    NocturneTheme { PinChip() }
}
