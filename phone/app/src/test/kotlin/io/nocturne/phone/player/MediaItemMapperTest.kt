package io.nocturne.phone.player

import androidx.media3.common.MediaMetadata
import io.nocturne.phone.data.db.entity.TrackEntity
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

private fun trackFixture(
    id: String = "0".repeat(64),
    artistList: List<String> = listOf("Artist"),
    albumArtistList: List<String> = listOf("Artist"),
): TrackEntity = TrackEntity(
    id = id, title = "Title",
    artist = artistList, albumArtist = albumArtistList,
    album = "Album", albumId = "1".repeat(64), albumArtistId = "2".repeat(64),
    genre = listOf(), genreId = null,
    trackNumber = 4, discNumber = 1, year = 2024,
    durationMs = 234000L, sizeBytes = 5_000_000L, format = "flac",
    mtimeNs = 0L, dateAdded = "2026-04-27",
    path = "/storage/emulated/0/music/resident/sample.flac",
    isResident = true, searchBlob = "title",
)

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class MediaItemMapperTest {

    @Test fun toMediaItemSetsIdAndCoreMetadata() {
        val item = trackFixture(id = "id-1").toMediaItem()
        check(item.mediaId == "id-1") { "expected mediaId=id-1 got ${item.mediaId}" }
        check(item.mediaMetadata.title == "Title") { "expected title=Title got ${item.mediaMetadata.title}" }
        check(item.mediaMetadata.artist == "Artist") { "expected artist=Artist got ${item.mediaMetadata.artist}" }
        check(item.mediaMetadata.albumTitle == "Album") { "expected albumTitle=Album" }
        check(item.mediaMetadata.albumArtist == "Artist") { "expected albumArtist=Artist" }
        check(item.mediaMetadata.trackNumber == 4) { "expected trackNumber=4 got ${item.mediaMetadata.trackNumber}" }
        check(item.mediaMetadata.discNumber == 1) { "expected discNumber=1 got ${item.mediaMetadata.discNumber}" }
    }

    @Test fun toMediaItemHandlesEmptyArtistList() {
        val item = trackFixture(artistList = emptyList(), albumArtistList = emptyList())
            .toMediaItem()
        check(item.mediaMetadata.artist == null) { "expected null artist got ${item.mediaMetadata.artist}" }
        check(item.mediaMetadata.albumArtist == null) { "expected null albumArtist got ${item.mediaMetadata.albumArtist}" }
    }

    @Test fun toMediaItemUsesArtworkBytesAndPictureTypeFrontCover() {
        val bytes = ByteArray(8) { it.toByte() }
        val item = trackFixture().toMediaItem(artworkBytes = bytes)
        check(item.mediaMetadata.artworkData != null) { "expected artworkData to be set" }
        check(item.mediaMetadata.artworkData!!.contentEquals(bytes)) { "artworkData mismatch" }
        check(item.mediaMetadata.artworkDataType == MediaMetadata.PICTURE_TYPE_FRONT_COVER) {
            "expected PICTURE_TYPE_FRONT_COVER got ${item.mediaMetadata.artworkDataType}"
        }
    }

    @Test fun toMediaItemNullArtworkOmitsArtworkFields() {
        val item = trackFixture().toMediaItem(artworkBytes = null)
        check(item.mediaMetadata.artworkData == null) { "expected null artworkData" }
        // artworkDataType is unset (null) when artworkData is null
        check(item.mediaMetadata.artworkDataType == null) { "expected null artworkDataType" }
    }
}
