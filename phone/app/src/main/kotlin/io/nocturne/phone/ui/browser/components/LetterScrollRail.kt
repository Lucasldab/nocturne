package io.nocturne.phone.ui.browser.components

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import io.nocturne.phone.ui.theme.JetBrainsMono
import io.nocturne.phone.ui.theme.NocturneOnSurfaceMuted
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Pure-JVM helper for first-letter bucketing. The bucketing rule must match
 * the DAO's server-side `UPPER(SUBSTR(s, 1, 1))` + bucket-to-'#' so the
 * composable can index its [letterIndex] map with the same key the DAO emits.
 */
object LetterIndex {
    /** A..Z + '#' (catch-all for digits, accents, punctuation, empty). */
    val LETTERS: List<Char> = ('A'..'Z').toList() + '#'

    /** Bucket the first character of [s] to one of [LETTERS]. */
    fun letterOf(s: String): Char {
        val c0 = s.firstOrNull()?.uppercaseChar() ?: return '#'
        return if (c0 in 'A'..'Z') c0 else '#'
    }
}

/**
 * Right-edge A..Z + '#' fastscroller rail.
 *
 * Behavior:
 *  - Faded baseline (alpha 0.35) so it does not clutter the terminal aesthetic.
 *  - Any list scroll bumps the rail to full alpha (1.0); after ~1.5s of idle,
 *    it animates back down over 400ms.
 *  - Tap on a populated letter snaps the [listState] (no animation — instant
 *    snap matches a fastscroller's expectation and avoids re-triggering the
 *    isScrollInProgress fade-in).
 *  - Letters absent from [letterIndex] render at alpha 0.25 with the muted
 *    palette tone and have no click handler (tap is a no-op).
 *
 * Note: this composable intentionally avoids the high-level visibility /
 * crossfade animation primitives that are banned by the Phase 6 grep gate
 * in browser/system/player code. The single Animatable<Float> +
 * Modifier.alpha pattern is the project precedent.
 */
@Composable
fun LetterScrollRail(
    listState: LazyListState,
    letterIndex: Map<Char, Int>,
    modifier: Modifier = Modifier,
) {
    val scope = rememberCoroutineScope()
    val alpha = remember { Animatable(0.35f) }

    LaunchedEffect(listState.isScrollInProgress) {
        if (listState.isScrollInProgress) {
            alpha.snapTo(1f)
        } else {
            // 1500ms idle decay back to faded baseline.
            delay(1500L)
            alpha.animateTo(
                targetValue = 0.35f,
                animationSpec = tween(durationMillis = 400),
            )
        }
    }

    Column(
        modifier = modifier
            .width(16.dp)
            .fillMaxHeight()
            .alpha(alpha.value),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        for (char in LetterIndex.LETTERS) {
            val populated = char in letterIndex
            val glyphColor = if (populated) {
                MaterialTheme.colorScheme.onSurface
            } else {
                NocturneOnSurfaceMuted
            }
            val perGlyphAlpha = if (populated) 1f else 0.25f
            val tapModifier = if (populated) {
                Modifier.clickable {
                    val target = letterIndex.getValue(char)
                    scope.launch { listState.scrollToItem(target) }
                }
            } else {
                Modifier
            }
            Text(
                text = char.toString(),
                style = MaterialTheme.typography.labelSmall.copy(
                    fontFamily = JetBrainsMono,
                ),
                color = glyphColor,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .height(IntrinsicSize.Min)
                    .padding(horizontal = 4.dp, vertical = 1.dp)
                    .alpha(perGlyphAlpha)
                    .then(tapModifier),
            )
        }
    }
}
