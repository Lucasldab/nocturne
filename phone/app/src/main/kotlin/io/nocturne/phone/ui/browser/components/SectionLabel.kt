package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp

/**
 * Mono-lowercase letterspaced section header at the top of each browse screen.
 * Per design pass 2026-04-27 SectionLabel:
 *   font: monospace 11sp
 *   color: onSurfaceVariant (muted)
 *   letterSpacing: 1
 *   text: lowercase
 *   padding: 12dp top / 16dp horizontal / 8dp bottom
 *
 * Examples:
 *   "47 albums · 32 resident"
 *   "12 artists"
 *   "699 tracks"
 */
@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text.lowercase(),
        style = MaterialTheme.typography.labelSmall.copy(
            letterSpacing = androidx.compose.ui.unit.TextUnit.Unspecified,
        ),
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = TextAlign.Start,
        modifier = modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 8.dp),
    )
}
