package io.nocturne.phone.data.catalog

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaMetadataRetriever
import android.provider.DocumentsContract
import androidx.core.net.toUri
import android.util.Log
import android.util.LruCache
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/**
 * Embedded album art reader. For each albumId, finds the first resident track,
 * opens its SAF URI, asks `MediaMetadataRetriever` for the embedded picture
 * bytes, and decodes them into a small bitmap. Results are memoised in an
 * `LruCache` keyed by albumId so a long Albums list doesn't re-read MMR per
 * scroll.
 *
 * Negative results (no first-resident-track / no embedded picture / decode
 * failure) are also cached as `null` so we don't keep re-trying the same album.
 *
 * No Coil, no external image library — single-purpose, single-format use case
 * (embedded APIC / METADATA_BLOCK_PICTURE / atom-format covers).
 */
class AlbumArtRepository(
    private val appContext: Context,
    private val db: NocturneDatabase,
    private val syncPrefs: SyncPrefs,
    private val targetSizePx: Int = 256,
) {

    /** ~50 MB ceiling on the cache (50 albums × ~1 MB peak per bitmap). */
    private val cache = object : LruCache<String, Cached>(50) {
        override fun sizeOf(key: String, value: Cached): Int = 1
    }

    /** Per-album single-flight: prevents concurrent MMR calls for the same id. */
    private val locks = mutableMapOf<String, Mutex>()
    private val locksGuard = Mutex()

    private suspend fun lockFor(albumId: String): Mutex = locksGuard.withLock {
        locks.getOrPut(albumId) { Mutex() }
    }

    /**
     * Returns a small bitmap for the album, or null if no embedded artwork is
     * available. Never throws — failures cache a null result.
     */
    suspend fun load(albumId: String): Bitmap? {
        cache.get(albumId)?.let { return it.bitmap }
        val lock = lockFor(albumId)
        lock.withLock {
            cache.get(albumId)?.let { return it.bitmap }
            val bmp = withContext(Dispatchers.IO) { readArt(albumId) }
            cache.put(albumId, Cached(bmp))
            return bmp
        }
    }

    private suspend fun readArt(albumId: String): Bitmap? {
        val track = db.trackDao().firstResidentByAlbum(albumId) ?: return null
        val musicTreeStr = syncPrefs.musicTreeUri.first() ?: return null
        val musicTreeUri = musicTreeStr.toUri()
        val phoneRelativePath = track.path
            .removePrefix("resident/")
            .removePrefix("archive/")
        val treeDocId = DocumentsContract.getTreeDocumentId(musicTreeUri)
        val audioFileDocId = "$treeDocId/$phoneRelativePath"
        val audioFileUri = DocumentsContract.buildDocumentUriUsingTree(musicTreeUri, audioFileDocId)

        // Path 1: embedded picture via MMR. FLAC + AAC carry covers natively;
        // Opus drops them at transcode time (METADATA_BLOCK_PICTURE Vorbis
        // comment isn't written by ffmpeg's single-pass encoder), which is
        // why we need the sidecar fallback below.
        val embedded = readEmbedded(audioFileUri, albumId)
        if (embedded != null) return embedded

        // Path 2: cover.jpg / folder.jpg sidecar in the album directory.
        // Daemon hardlinks these from archive/ into resident/ alongside the
        // transcoded audio (zero extra storage; same inode). Try common
        // filenames in order.
        val albumDirRelative = phoneRelativePath.substringBeforeLast('/', "")
        for (name in SIDECAR_NAMES) {
            val sidecarDocId = if (albumDirRelative.isEmpty()) {
                "$treeDocId/$name"
            } else {
                "$treeDocId/$albumDirRelative/$name"
            }
            val sidecarUri = DocumentsContract.buildDocumentUriUsingTree(musicTreeUri, sidecarDocId)
            val bytes = try {
                appContext.contentResolver.openInputStream(sidecarUri)?.use { it.readBytes() }
            } catch (e: Throwable) {
                null
            } ?: continue
            return decodeScaled(bytes)
        }
        return null
    }

    private fun readEmbedded(audioFileUri: android.net.Uri, albumId: String): Bitmap? {
        val pfd = try {
            appContext.contentResolver.openFileDescriptor(audioFileUri, "r") ?: return null
        } catch (e: Throwable) {
            Log.w(TAG, "open failed for $albumId: ${e.message}")
            return null
        }
        return pfd.use { fd ->
            val mmr = MediaMetadataRetriever()
            try {
                mmr.setDataSource(fd.fileDescriptor)
                val bytes = mmr.embeddedPicture ?: return@use null
                decodeScaled(bytes)
            } catch (e: Throwable) {
                Log.w(TAG, "MMR failed for $albumId: ${e.message}")
                null
            } finally {
                runCatching { mmr.release() }
            }
        }
    }

    private fun decodeScaled(bytes: ByteArray): Bitmap? {
        // Two-pass decode — bounds first to compute sample size, then decode.
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        val w = bounds.outWidth.coerceAtLeast(1)
        val h = bounds.outHeight.coerceAtLeast(1)
        var sample = 1
        while (w / (sample * 2) >= targetSizePx && h / (sample * 2) >= targetSizePx) sample *= 2
        val opts = BitmapFactory.Options().apply { inSampleSize = sample }
        return runCatching { BitmapFactory.decodeByteArray(bytes, 0, bytes.size, opts) }.getOrNull()
    }

    private data class Cached(val bitmap: Bitmap?)

    companion object {
        private const val TAG = "AlbumArtRepo"
        private val SIDECAR_NAMES = listOf("cover.jpg", "cover.png", "folder.jpg", "folder.png")
    }
}
