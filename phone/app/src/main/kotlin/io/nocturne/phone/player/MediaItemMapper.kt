package io.nocturne.phone.player

import android.net.Uri
import android.provider.DocumentsContract
import androidx.annotation.OptIn
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.util.UnstableApi
import io.nocturne.phone.data.db.entity.TrackEntity
import java.io.File

/**
 * Single source of truth for TrackEntity → MediaItem conversion.
 *
 * URI strategy (resolved during Phase 8 hardware acceptance — RESEARCH Open
 * Question 2 closed): on real GrapheneOS + targetSdk 35, raw file:// access to
 * Syncthing-Fork-managed paths outside the app-private dir is blocked by
 * scoped storage. The app must use SAF content:// URIs constructed from a
 * user-granted tree URI plus the relative path stored in `track.path`.
 *
 *   - If `musicTreeUri` is non-null: build a `content://` URI via
 *     `DocumentsContract.buildDocumentUriUsingTree`. Uses the tree's
 *     document-id as the prefix and appends the relative path.
 *   - If `musicTreeUri` is null: fall back to `file://` for tests / preview
 *     compose / dev builds where the path is host-absolute.
 *
 * Security: path is NOT included in MediaMetadata — only title/artist/album/
 * albumArtist/trackNumber/discNumber reach the lock-screen MediaSession surface
 * (T-05-03-02 mitigation).
 */
@OptIn(UnstableApi::class)
fun TrackEntity.toMediaItem(
    musicTreeUri: Uri? = null,
    artworkBytes: ByteArray? = null,
): MediaItem {
    val metadata = MediaMetadata.Builder()
        .setTitle(title)
        .setArtist(artist.firstOrNull())
        .setAlbumTitle(album)
        .setAlbumArtist(albumArtist.firstOrNull())
        .setTrackNumber(trackNumber)
        .setDiscNumber(discNumber)
        .apply {
            if (artworkBytes != null) {
                setArtworkData(artworkBytes, MediaMetadata.PICTURE_TYPE_FRONT_COVER)
            }
        }
        .build()

    // Catalog paths are LIBRARY-ROOT relative (`resident/...` or `archive/...`)
    // because the desktop daemon owns the path-layout subtrees. But desktop
    // Syncthing's sync-files folder is mapped at `<library>/resident/` and
    // sends only the contents — so on the phone, files arrive WITHOUT the
    // `resident/` prefix. Strip the leading subtree segment before composing
    // the SAF URI. (Phase 3 architecture: the phone never receives `archive/`
    // content, so the `archive/` branch should never fire in practice — kept
    // for robustness in case a track flips during a rotation.)
    val phoneRelativePath = path
        .removePrefix("resident/")
        .removePrefix("archive/")

    val uri: Uri = if (musicTreeUri != null) {
        val treeDocId = DocumentsContract.getTreeDocumentId(musicTreeUri)
        val childDocId = "$treeDocId/$phoneRelativePath"
        DocumentsContract.buildDocumentUriUsingTree(musicTreeUri, childDocId)
    } else {
        Uri.fromFile(File(path))
    }

    return MediaItem.Builder()
        .setMediaId(id)
        .setUri(uri)
        .setMediaMetadata(metadata)
        .build()
}
