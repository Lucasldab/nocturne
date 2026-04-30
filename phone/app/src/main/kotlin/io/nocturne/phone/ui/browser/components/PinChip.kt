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
 * Pin chip rendered next to TrackRow / AlbumRow entries that the user
 * has pinned, OR next to non-resident rows so the user can pin them.
 *
 * Three visual states (pin-as-download contract):
 *
 *   [PinState.NotPinned]            offer-to-pin (only shown on non-resident rows):
 *     border  = onSurfaceVariant
 *     fill    = surfaceVariant
 *     label   = onSurface
 *
 *   [PinState.PinnedPulling]        pinned, file not on phone yet (Syncthing en-route):
 *     border  = primary
 *     fill    = surfaceVariant
 *     label   = primary
 *
 *   [PinState.PinnedReady]          pinned, file resident on phone (terminal state):
 *     border  = primary
 *     fill    = primary
 *     label   = onPrimary
 *
 * State derivation lives at the call-site:
 *   isPinned && !isResident → PinnedPulling
 *   isPinned && isResident  → PinnedReady
 *  !isPinned && !isResident → NotPinned (offer-to-pin)
 *  !isPinned && isResident  → don't render the chip at all
 *
 * Tap toggles pin state via the call-site's `onClick` lambda which routes
 * through `BrowserViewModel.togglePinTrack` / `togglePinAlbum`. On a
 * pinned row, tap-again unpins (emits a `pinned: false` JSONL tombstone).
 *
 * Compose strong-skipping: this composable is skippable because all params
 * are stable primitives + `() -> Unit`.
 */
enum class PinState { NotPinned, PinnedPulling, PinnedReady }

@Composable
fun PinChip(
    onClick: () -> Unit = {},
    state: PinState = PinState.NotPinned,
    progress: Float? = null,
) {
    val borderColor = when (state) {
        PinState.NotPinned -> MaterialTheme.colorScheme.onSurfaceVariant
        PinState.PinnedPulling, PinState.PinnedReady -> MaterialTheme.colorScheme.primary
    }
    val fillColor = when (state) {
        PinState.NotPinned, PinState.PinnedPulling -> MaterialTheme.colorScheme.surfaceVariant
        PinState.PinnedReady -> MaterialTheme.colorScheme.primary
    }
    val labelColor = when (state) {
        PinState.NotPinned -> MaterialTheme.colorScheme.onSurface
        PinState.PinnedPulling -> MaterialTheme.colorScheme.primary
        PinState.PinnedReady -> MaterialTheme.colorScheme.onPrimary
    }

    val label = when (state) {
        PinState.PinnedPulling -> {
            val pct = progress?.let { (it * 100f).toInt().coerceIn(0, 99) }
            if (pct != null) "PIN $pct%" else "PIN ⋯"
        }
        else -> "PIN"
    }

    Surface(
        modifier = Modifier.padding(start = 8.dp),
        border = BorderStroke(1.dp, borderColor),
        color = fillColor,
        onClick = onClick,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = labelColor,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp),
        )
    }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun PinChipNotPinnedPreview() {
    NocturneTheme { PinChip(state = PinState.NotPinned) }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun PinChipPullingPreview() {
    NocturneTheme { PinChip(state = PinState.PinnedPulling) }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun PinChipPullingWithProgressPreview() {
    NocturneTheme { PinChip(state = PinState.PinnedPulling, progress = 0.42f) }
}

@Preview(showBackground = true, backgroundColor = 0xFF0A0A0A)
@Composable
private fun PinChipReadyPreview() {
    NocturneTheme { PinChip(state = PinState.PinnedReady) }
}
