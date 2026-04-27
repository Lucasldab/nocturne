package io.nocturne.phone.player

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.io.ByteArrayOutputStream

/**
 * RESEARCH.md Pitfall 5 mitigation: scale embedded album art to a Binder-IPC-
 * safe size before exposing it via MediaMetadata.artworkData.
 *
 * 300x300 / JPEG q=85 fits well under the ~1 MB Binder transaction limit
 * (typical output ~30-60 KB) and matches the UI-SPEC art-square contract for
 * both NowPlayingScreen and MiniPlayer thumbnails.
 *
 * Pure utility: no lifecycle, no Android Context -- just BitmapFactory +
 * Bitmap + ByteArrayOutputStream.
 */
object ArtworkLoader {

    private const val TARGET_PX = 300
    private const val JPEG_QUALITY = 85

    /**
     * Decode raw album art bytes (any format BitmapFactory supports -- JPEG,
     * PNG, WebP), scale to 300x300, re-encode as JPEG q=85. Returns null on
     * unparseable input or empty bytes.
     */
    fun scaleTo300(rawBytes: ByteArray?): ByteArray? {
        if (rawBytes == null || rawBytes.isEmpty()) return null
        val source = BitmapFactory.decodeByteArray(rawBytes, 0, rawBytes.size) ?: return null
        return try {
            val scaled = Bitmap.createScaledBitmap(source, TARGET_PX, TARGET_PX, /* filter = */ true)
            val out = ByteArrayOutputStream()
            scaled.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, out)
            // Free the intermediate bitmap if it was a new allocation.
            if (scaled !== source) scaled.recycle()
            out.toByteArray()
        } finally {
            source.recycle()
        }
    }
}
