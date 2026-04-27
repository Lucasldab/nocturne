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
 * Three visual states (UI-SPEC Surface 2 / PinChip state machine):
 *
 *   [isPinned = false]            not-pinned:
 *     border  = onSurfaceVariant
 *     fill    = surfaceVariant
 *     label   = onSurface
 *
 *   [isPinned = true]             pinned-awaiting-sync (Phase 5 only emits this state):
 *     border  = primary
 *     fill    = surfaceVariant
 *     label   = primary
 *
 *   pinned-resident               (Phase 6 toggles synced=true → daemon has pulled the file)
 *     border  = primary
 *     fill    = primary
 *     label   = onPrimary
 *     (Phase 6 introduces a `isResident: Boolean` param; Phase 5 leaves this for later)
 *
 * Compose strong-skipping: this composable is skippable because all params are
 * stable primitives + `() -> Unit`.
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
