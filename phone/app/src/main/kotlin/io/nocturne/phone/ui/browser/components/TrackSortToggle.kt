package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.nocturne.phone.ui.browser.TrackSortMode

/**
 * Mono-font helper duplicated from `RotationScreen.monoStyle` (which lives
 * in `io.nocturne.phone.ui.system` with `internal` visibility — outside
 * scope here). Four-line copy is the precedent over hoisting a shared
 * theme module (per quick task 260430-s5u plan, scope decision: option a).
 */
@Composable
private fun mono(sizeSp: Int): TextStyle = TextStyle(
    fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
    fontSize = sizeSp.sp,
)

/**
 * Four-chip horizontally-scrollable row: `[a-z]  most  recent  added`.
 * Active mode is bracketed in primary color; inactive modes are muted
 * onSurfaceVariant. Mirrors the StatsScreen window-toggle (lines 85-108)
 * — the visual precedent for lightweight terminal toggles in this app.
 *
 * Wrapped in [horizontalScroll] because four labels can overflow narrow
 * screens; the existing 3-tab Stats toggle does not need this.
 */
@Composable
fun TrackSortToggle(
    current: TrackSortMode,
    onSelect: (TrackSortMode) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(20.dp),
        modifier = modifier
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        TrackSortMode.values().forEach { m ->
            val active = m == current
            Text(
                text = if (active) "[${m.label}]" else " ${m.label} ",
                style = mono(12),
                color = if (active) {
                    MaterialTheme.colorScheme.primary
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
                modifier = Modifier
                    .padding(vertical = 4.dp)
                    .then(
                        if (!active) {
                            Modifier.clickable { onSelect(m) }
                        } else {
                            Modifier
                        },
                    ),
            )
        }
    }
}
