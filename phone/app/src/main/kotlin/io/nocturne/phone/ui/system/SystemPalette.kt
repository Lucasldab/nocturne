package io.nocturne.phone.ui.system

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Quick task 260428-7zc — shared chrome for the four System sub-screens.
 *
 * Mirrors the per-bucket palette from the canonical design source
 * /tmp/nocturne-design/nocturne/project/screens-system.jsx (BUCKETS array
 * colors). Index ordering follows RotationAggregator.CANONICAL_ORDER so the
 * Rotation bucket-bar and Storage by-bucket bars share one accent across
 * screens.
 */
object BucketColors {
    private val palette = listOf(
        Color(0xFF7E3AA0),  // primary purple — recent_adds
        Color(0xFF9E5FB8),  // lighter purple — top_played
        Color(0xFFC080D8),  // pale lilac — recent_plays
        Color(0xFFCF6679),  // error/red-pink — loved
        Color(0xFF6FB8B0),  // teal — exploration
        Color(0xFF8A8A8A),  // muted gray — manual_pins
    )
    fun forIndex(i: Int): Color = palette[((i % palette.size) + palette.size) % palette.size]
}

/**
 * `── header ──` divider rendered MONO 11sp letterSpacing 1.5sp,
 * onSurfaceVariant. Matches the JSX SectionHeader component.
 */
@Composable
fun SectionHeader(text: String, modifier: Modifier = Modifier) {
    Text(
        text = "── $text ──",
        style = TextStyle(
            fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
            fontSize = 11.sp,
            fontWeight = FontWeight.Normal,
            letterSpacing = 1.5.sp,
        ),
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = modifier.padding(top = 24.dp, bottom = 8.dp),
    )
}

/** `~/route` MONO 11sp letterSpacing 1sp onSurfaceVariant — terminal-prompt header. */
@Composable
fun TerminalPrompt(path: String, modifier: Modifier = Modifier) {
    Text(
        text = path,
        style = TextStyle(
            fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
            fontSize = 11.sp,
            letterSpacing = 1.sp,
        ),
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = modifier,
    )
}

/** Bold sans 22sp screen title — matches the SANS22 hero in the design. */
@Composable
fun ScreenHero(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleLarge.copy(
            fontSize = 22.sp,
            fontWeight = FontWeight.Bold,
        ),
        color = MaterialTheme.colorScheme.onBackground,
        modifier = modifier.padding(top = 4.dp),
    )
}

/**
 * Quick task 260428-ja8 — Utility-mode tab bar. Four horizontal tabs sit
 * directly below the BrowserRoot TopAppBar when the shell is in utility
 * mode (◇ → ◆). Mirrors phone-shell.jsx lines 50-66:
 *
 *   - 36dp tall row, surface bg
 *   - MONO 11sp letterSpacing 0.5sp tab labels
 *   - active tab: primary text + 2dp primary bottom-stripe
 *   - inactive tab: onSurfaceVariant text + transparent stripe
 *   - 1dp surfaceVariant hairline below the row so it reads as a ruled
 *     boundary above content
 */
@Composable
fun UtilityBar(active: String, onChange: (String) -> Unit, modifier: Modifier = Modifier) {
    val tabs = listOf("rotation", "sync", "storage", "stats")
    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(36.dp)
            .background(MaterialTheme.colorScheme.surface),
    ) {
        tabs.forEach { id ->
            val isActive = id == active
            Column(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight()
                    .clickable { onChange(id) },
                verticalArrangement = Arrangement.Center,
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.Center) {
                    Text(
                        text = id,
                        style = TextStyle(
                            fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
                            fontSize = 11.sp,
                            letterSpacing = 0.5.sp,
                        ),
                        color = if (isActive) MaterialTheme.colorScheme.primary
                                else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(2.dp)
                        .background(
                            if (isActive) MaterialTheme.colorScheme.primary
                            else Color.Transparent,
                        ),
                )
            }
        }
    }
    // 1dp bottom hairline (surfaceVariant) so the bar reads as a ruled boundary above content.
    Box(
        modifier = modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(MaterialTheme.colorScheme.surfaceVariant),
    )
}
