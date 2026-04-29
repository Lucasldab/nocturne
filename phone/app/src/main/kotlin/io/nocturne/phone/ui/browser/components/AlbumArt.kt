package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import io.nocturne.phone.data.AppContainer
import kotlin.math.cos
import kotlin.math.sin

/**
 * Album-art tile.
 *
 * If [container] is provided AND the album's first resident track has embedded
 * artwork, decodes it via [io.nocturne.phone.data.catalog.AlbumArtRepository]
 * and renders the bitmap. Otherwise falls back to the deterministic gradient
 * placeholder (design pass 2026-04-27 albumGradient).
 *
 * The gradient is ALWAYS painted under the bitmap so a partially-loaded row
 * never flashes a blank box. `produceState` runs on the default dispatcher;
 * AlbumArtRepository moves the actual MMR call onto Dispatchers.IO.
 */
@Composable
fun AlbumArt(
    seed: String,
    modifier: Modifier = Modifier,
    size: Dp = 48.dp,
    container: AppContainer? = null,
) {
    val brush = remember(seed, size) { albumGradientBrush(seed) }
    val bitmap: ImageBitmap? = if (container == null) null else {
        val state = produceState<ImageBitmap?>(initialValue = null, key1 = seed) {
            value = container.albumArt.load(seed)?.asImageBitmap()
        }
        state.value
    }
    Box(
        modifier = modifier
            .size(size)
            .background(brush),
    ) {
        if (bitmap != null) {
            Image(
                bitmap = bitmap,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.fillMaxSize(),
            )
        }
    }
}

internal fun albumGradientBrush(seed: String): Brush {
    var h = 0u
    for (ch in seed) {
        h = (h * 31u + ch.code.toUInt())
    }
    val h1 = (h % 360u).toFloat()
    val h2 = ((h1.toInt() + 30 + ((h shr 8) % 60u).toInt()) % 360).toFloat()
    val s1 = 18 + ((h shr 4) % 12u).toInt()
    val s2 = 22 + ((h shr 12) % 14u).toInt()
    val l1 = 14 + ((h shr 6) % 8u).toInt()
    val l2 = 26 + ((h shr 10) % 10u).toInt()
    val angleDeg = ((h shr 2) % 360u).toInt()

    val c1 = hslToColor(h1, s1 / 100f, l1 / 100f)
    val c2 = hslToColor(h2, s2 / 100f, l2 / 100f)

    val rad = Math.toRadians(angleDeg.toDouble())
    val dx = cos(rad).toFloat()
    val dy = sin(rad).toFloat()
    return Brush.linearGradient(
        colors = listOf(c1, c2),
        start = androidx.compose.ui.geometry.Offset(0.5f - dx, 0.5f - dy),
        end = androidx.compose.ui.geometry.Offset(0.5f + dx, 0.5f + dy),
    )
}

private fun hslToColor(hueDeg: Float, sat: Float, light: Float): Color {
    val h = hueDeg / 360f
    val q = if (light < 0.5f) light * (1f + sat) else light + sat - light * sat
    val p = 2f * light - q
    val r = hueToRgb(p, q, h + 1f / 3f)
    val g = hueToRgb(p, q, h)
    val b = hueToRgb(p, q, h - 1f / 3f)
    return Color(r, g, b)
}

private fun hueToRgb(p: Float, q: Float, t: Float): Float {
    var x = t
    if (x < 0f) x += 1f
    if (x > 1f) x -= 1f
    return when {
        x < 1f / 6f -> p + (q - p) * 6f * x
        x < 1f / 2f -> q
        x < 2f / 3f -> p + (q - p) * (2f / 3f - x) * 6f
        else -> p
    }
}
