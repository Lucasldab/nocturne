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
 *
 * Two visual states (UI-SPEC Surface 2):
 *
 *   [isPinned = false]            not-pinned:
 *     border  = onSurfaceVariant
 *     fill    = surfaceVariant
 *     label   = onSurface
 *
 *   [isPinned = true]             pinned-awaiting-sync:
 *     border  = primary
 *     fill    = surfaceVariant
 *     label   = primary
 *
 * Phase 6 introduces toggle behavior: tap-once when isPinned=false pins,
 * tap-again when isPinned=true unpins (emits a `pinned: false` JSONL
 * tombstone). The PinChip API is unchanged — call sites route their
 * onClick lambda through `BrowserViewModel.togglePinTrack` /
 * `togglePinAlbum` (the toggle dispatcher).
 *
 * The `pinned-resident` third state from Phase 5's docstring (fill = primary,
 * label = onPrimary) remains a future feature — Phase 6 explicitly defers it.
 *
 * Compose strong-skipping: this composable is skippable because all params
 * are stable primitives + `() -> Unit`.
 */
@Composable
fun PinChip(
    onClick: () -> Unit = {},
    isPinned: Boolean = false,
) {
    val borderColor = if (isPinned) {
        MaterialTheme.colorScheme.primary
    } else {
        MaterialTheme.colorScheme.onSurfaceVariant
    }
    val labelColor = if (isPinned) {
        MaterialTheme.colorScheme.primary
    } else {
        MaterialTheme.colorScheme.onSurface
    }

    Surface(
        modifier = Modifier.padding(start = 8.dp),
        border = BorderStroke(1.dp, borderColor),
        color = MaterialTheme.colorScheme.surfaceVariant,
        onClick = onClick,
    ) {
        Text(
            text = "PIN",
            style = MaterialTheme.typography.labelSmall,
            color = labelColor,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp),
        )
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun PinChipNotPinnedPreview() {
    NocturneTheme { PinChip(isPinned = false) }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun PinChipPinnedAwaitingSyncPreview() {
    NocturneTheme { PinChip(isPinned = true) }
}
