package io.nocturne.phone.player

import android.net.Uri
import androidx.annotation.OptIn
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.util.UnstableApi
import io.nocturne.phone.data.db.entity.TrackEntity
import java.io.File

/**
 * Single source of truth for TrackEntity → MediaItem conversion.
 *
 * Used by:
 *   - PlayerViewModel.playAlbumFromTrack (plan 05-03)
 *   - AlbumUnitShuffle (plan 05-04)
 *   - QueueRepository.toMediaItems (plan 05-06)
 *   - NowPlayingScreen art update via Player.Listener.onMediaMetadataChanged (plan 05-05)
 *
 * Phase 5 URI strategy: build a file:// Uri from track.path. This works as
 * long as track.path is an absolute path the app can read (Phase 3 wrote the
 * library to /storage/emulated/0/<library>; Phase 4 imported it via SAF tree
 * URI which grants R/W). Hardware verification is required to confirm
 * file:// URIs actually play on a real GrapheneOS device — RESEARCH.md
 * Open Question 2. If file:// fails, plan 05-05 swaps to a content:// URI
 * built from the metaTreeUri's sibling music folder.
 *
 * Security: path is NOT included in MediaMetadata — only title/artist/album/
 * albumArtist/trackNumber/discNumber reach the lock-screen MediaSession surface
 * (T-05-03-02 mitigation).
 */
@OptIn(UnstableApi::class)
fun TrackEntity.toMediaItem(artworkBytes: ByteArray? = null): MediaItem {
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

    // TODO(plan 05-05 hw verify): if file:// playback fails on GrapheneOS SAF
    // mounts, switch to building a content:// Uri from SyncPrefs.musicTreeUri
    // (RESEARCH.md Open Question 2). Today we trust the path is absolute and
    // readable.
    val uri: Uri = Uri.fromFile(File(path))

    return MediaItem.Builder()
        .setMediaId(id)
        .setUri(uri)
        .setMediaMetadata(metadata)
        .build()
}
