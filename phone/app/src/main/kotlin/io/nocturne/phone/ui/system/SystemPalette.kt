package io.nocturne.phone.ui.system

import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
