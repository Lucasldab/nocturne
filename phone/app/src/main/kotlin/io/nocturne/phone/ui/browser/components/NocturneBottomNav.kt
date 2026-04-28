package io.nocturne.phone.ui.browser.components

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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.nocturne.phone.ui.browser.Routes
import io.nocturne.phone.ui.theme.JetBrainsMono

/**
 * Bottom navigation bar — custom layout per the locked design spec
 * (260428 storage-cap-slider message tail).
 *
 * - Container: 80dp height, surface (#121212) bg, 4 equal-flex tabs.
 * - Glyph pill (top row): JetBrains Mono 13sp / 500, padding 4dp×14dp,
 *   border-radius 12dp. Inactive transparent + #8A8A8A; active rgba(140,
 *   65, 180, 0.18) ≈ 0x2E8C41B4 + #8C41B4.
 * - Label (bottom row): JetBrains Mono 11sp / 500. Inactive #8A8A8A,
 *   active #E0E0E0.
 * - Layout: Column, gap 4dp, centered.
 *
 * Replaces the earlier Material3 NavigationBar + NavigationBarItem stack
 * because Material3's indicator shape is fixed-pill behind the icon — the
 * spec wants the pill to wrap ONLY the glyph, not the full label-row, and
 * to inflate from a 28dp glyph height rather than the 32dp Material default.
 */
@Composable
fun NocturneBottomNav(
    activeRoute: String?,
    onTab: (String) -> Unit,
) {
    val tabs = listOf(
        Tab(Routes.ALBUMS, "A", "Albums"),
        Tab(Routes.ARTISTS, "A", "Artists"),
        Tab(Routes.TRACKS, "T", "Tracks"),
        Tab(Routes.GENRES, "G", "Genres"),
    )
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(80.dp)
            .background(MaterialTheme.colorScheme.surface),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        tabs.forEach { tab ->
            BottomNavTab(
                tab = tab,
                active = activeRoute == tab.route,
                onClick = { onTab(tab.route) },
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight(),
            )
        }
    }
}

private data class Tab(val route: String, val glyph: String, val label: String)

private val GLYPH_INACTIVE = Color(0xFF8A8A8A)
private val GLYPH_ACTIVE = Color(0xFF8C41B4)
private val GLYPH_PILL_ACTIVE_BG = Color(0x2E8C41B4)
private val LABEL_ACTIVE = Color(0xFFE0E0E0)
private val LABEL_INACTIVE = Color(0xFF8A8A8A)

@Composable
private fun BottomNavTab(
    tab: Tab,
    active: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val glyphStyle = TextStyle(
        fontFamily = JetBrainsMono,
        fontSize = 13.sp,
        fontWeight = FontWeight.Medium,
    )
    val labelStyle = TextStyle(
        fontFamily = JetBrainsMono,
        fontSize = 11.sp,
        fontWeight = FontWeight.Medium,
    )
    Column(
        modifier = modifier.clickable(onClick = onClick),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterVertically),
    ) {
        // Glyph pill — wraps just the glyph. Padding 4dp vertical / 14dp
        // horizontal yields a visual pill of ~25dp tall × ~30dp wide.
        Box(
            modifier = Modifier
                .clip(RoundedCornerShape(12.dp))
                .background(if (active) GLYPH_PILL_ACTIVE_BG else Color.Transparent)
                .padding(horizontal = 14.dp, vertical = 4.dp),
        ) {
            Text(
                text = tab.glyph,
                style = glyphStyle,
                color = if (active) GLYPH_ACTIVE else GLYPH_INACTIVE,
            )
        }
        Text(
            text = tab.label,
            style = labelStyle,
            color = if (active) LABEL_ACTIVE else LABEL_INACTIVE,
        )
    }
}
