package io.nocturne.phone.ui.browser.components

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
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
    /** '#' first (catch-all for digits, accents, punctuation, empty), then A..Z.
     *  '#'-first matches the user's mental model: titles starting with digits
     *  / symbols (e.g. "111", "4up") are listed BEFORE alphabetical entries
     *  in the data ordering, so the rail's first cell jumps to that bucket. */
    val LETTERS: List<Char> = listOf('#') + ('A'..'Z').toList()

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
 *
 * Each letter is wrapped in a full-rail-width Box so the tappable target is
 * (rail-width × row-height) rather than the glyph's intrinsic bounds. On a
 * 16dp-wide rail with 27 letters, the glyph-only hit area was ~6×10dp with
 * 1dp dead-zones that swallowed real-device taps (quick task 260430-wt0 Bug 2).
 */
@Composable
fun LetterScrollRail(
    listState: LazyListState,
    letterIndex: Map<Char, Int>,
    modifier: Modifier = Modifier,
) {
    val scope = rememberCoroutineScope()
    val alpha = remember { Animatable(0.35f) }
    val railHeightPx = remember { mutableIntStateOf(0) }
    val lastTargetIdx = remember { mutableIntStateOf(-1) }

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

    // Resolve a finger-Y position to a letter index, then scroll the list to
    // the first row of that letter's bucket. Coalesces consecutive snaps to
    // the same letter (drag through a 200dp rail at 30 fps would otherwise
    // fire 30 redundant scrollToItem calls per second).
    fun handleY(y: Float) {
        val total = railHeightPx.intValue
        if (total <= 0) return
        val letters = LetterIndex.LETTERS
        val per = total.toFloat() / letters.size
        val raw = (y / per).toInt().coerceIn(0, letters.size - 1)
        if (raw == lastTargetIdx.intValue) return
        // Update visual feedback regardless of whether the bucket has rows —
        // user sees their finger position track even when dragging through
        // empty letters (e.g. Q, X, Z in a small library).
        lastTargetIdx.intValue = raw
        val char = letters[raw]
        val target = letterIndex[char] ?: return
        scope.launch {
            alpha.snapTo(1f)
            listState.scrollToItem(target)
        }
    }

    Column(
        modifier = modifier
            // 40dp (was 28dp) — round-2 ship at 28dp still produced "great
            // difficulty hitting the right letter" on real device. Going
            // wider trades a thicker rail for actually-tappable letters.
            .width(40.dp)
            .fillMaxHeight()
            .alpha(alpha.value)
            .onSizeChanged { railHeightPx.intValue = it.height }
            .pointerInput(letterIndex) {
                detectVerticalDragGestures(
                    onDragStart = { offset -> handleY(offset.y) },
                    onDragEnd = { lastTargetIdx.intValue = -1 },
                    onDragCancel = { lastTargetIdx.intValue = -1 },
                    onVerticalDrag = { change, _ ->
                        change.consume()
                        handleY(change.position.y)
                    },
                )
            }
            .pointerInput(letterIndex) {
                detectTapGestures(onTap = { offset -> handleY(offset.y) })
            },
        // SpaceEvenly distributes letters across the FULL rail height so the
        // Y / 27 math in handleY actually maps to the visible letter under
        // the finger. Arrangement.Center previously clustered letters in the
        // middle of the rail with empty space above/below — finger-Y was
        // computed against the full rail height but glyphs were only in the
        // middle, so taps near the top targeted '#' (computed at y≈0) but
        // visually '#' was halfway down the rail. The mismatch was the
        // user-reported "considers the # button way above the actual UI #".
        verticalArrangement = Arrangement.SpaceEvenly,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        for ((idx, char) in LetterIndex.LETTERS.withIndex()) {
            val populated = char in letterIndex
            val active = idx == lastTargetIdx.intValue
            val glyphColor = when {
                active -> MaterialTheme.colorScheme.primary
                populated -> MaterialTheme.colorScheme.onSurface
                else -> NocturneOnSurfaceMuted
            }
            val perGlyphAlpha = if (populated) 1f else 0.25f
            // Modifier.weight(1f) ensures every letter Box gets an equal
            // share of the rail height, regardless of glyph metrics. This
            // makes the per = total/27 math in handleY exact: the box at
            // index N occupies y in [N*per, (N+1)*per). No per-letter
            // clickable — the parent Column's pointerInput owns tap+drag.
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .alpha(perGlyphAlpha),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = char.toString(),
                    style = MaterialTheme.typography.labelSmall.copy(
                        fontFamily = JetBrainsMono,
                    ),
                    color = glyphColor,
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}
