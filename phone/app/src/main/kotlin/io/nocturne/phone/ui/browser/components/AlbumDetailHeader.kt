package io.nocturne.phone.ui.browser.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.entity.AlbumEntity

/**
 * Album Detail header block (spec: screens-browse.jsx → AlbumDetail header).
 *
 * Outer wrapper: padding 8/16/16 (top/sides/bottom), flex row, gap 14dp.
 * Two children: 96dp AlbumArt and a metadata column.
 *
 * Static block — no taps. Lives between the BackButton row and the 1dp
 * surfaceVariant divider that separates it from the track list.
 */
@Composable
fun AlbumDetailHeader(
    album: AlbumEntity,
    residentTrackCount: Int,
    totalDurationMin: Int,
    container: AppContainer? = null,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        // Subelement 1: 96dp art tile. AlbumArt already paints the
        // deterministic gradient under the bitmap; the spec's pattern
        // overlay + initials caption render only when no real cover is
        // present, so the production case (cover.jpg sidecars in place)
        // shows a clean image and the fallback shows the synthetic art.
        AlbumArt(seed = album.id, size = 96.dp, container = container)

        // Subelement 2: metadata column (4dp top pad to optically align
        // with the art's top edge, accounting for cap height).
        Column(
            modifier = Modifier.padding(top = 4.dp),
        ) {
            Text(
                text = album.title,
                style = TextStyle(
                    // SANS — Material's titleLarge uses Inter/system sans by default.
                    fontFamily = MaterialTheme.typography.titleLarge.fontFamily,
                    fontSize = 20.sp,
                    fontWeight = FontWeight.Bold,
                    lineHeight = 24.sp,  // 1.2× of 20
                ),
                color = MaterialTheme.colorScheme.onBackground,
            )
            Text(
                text = album.albumArtist.firstOrNull().orEmpty(),
                style = MonoText(13),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 6.dp),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )

            // Two-line secondary block (mono, lh ~1.6).
            val genreLower = album.year?.let { "$it · " } ?: ""
            val line1 = buildString {
                if (album.year != null) append(album.year).append(" · ")
                // AlbumEntity has no genre column; pull from album_artist as a
                // weak proxy is wrong — leave blank if no genre available.
                // (genre lives on tracks; aggregate would require a query.)
            }.trimEnd(' ', '·').ifBlank { "—" }
            val totalSizeStr = fmtBytes(album.totalSizeBytes)
            val line2 = "${album.trackCount} tracks · $totalDurationMin min · $totalSizeStr"

            Text(
                text = line1,
                style = MonoText(11).copy(lineHeight = 18.sp),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp),
            )
            Text(
                text = line2,
                style = MonoText(11).copy(lineHeight = 18.sp),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            // Pills row.
            Row(
                modifier = Modifier.padding(top = 10.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Pill(
                    text = "$residentTrackCount/${album.trackCount} resident",
                    active = true,
                )
                if (residentTrackCount == 0) {
                    Pill(text = "not synced", active = false)
                }
            }
        }
    }
}

@Composable
private fun Pill(text: String, active: Boolean) {
    val accent = if (active) MaterialTheme.colorScheme.primary
                 else MaterialTheme.colorScheme.onSurfaceVariant
    Text(
        text = text,
        style = MonoText(11).copy(letterSpacing = 0.5.sp),
        color = accent,
        modifier = Modifier
            .border(width = 1.dp, color = accent)
            .padding(horizontal = 8.dp, vertical = 3.dp),
    )
}

@Composable
private fun MonoText(sizeSp: Int): TextStyle = TextStyle(
    fontFamily = MaterialTheme.typography.bodySmall.fontFamily,
    fontSize = sizeSp.sp,
)

/** "412 MB", "1.2 GB" — matches the JSX fmtBytes contract. */
internal fun fmtBytes(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val kb = bytes / 1024.0
    if (kb < 1024) return "${kb.toInt()} KB"
    val mb = kb / 1024.0
    if (mb < 1024) return "${mb.toInt()} MB"
    val gb = mb / 1024.0
    return "%.1f GB".format(gb)
}
