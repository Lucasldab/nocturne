package io.nocturne.phone.ui.browser

/**
 * String routes for Navigation Compose 2.9 (Navigation 3 is alpha — forbidden
 * by tech-stack lock). Detail routes encode the row id as a path parameter so
 * the destination handler can resolve the entity from Room without holding it
 * in the back stack.
 */
object Routes {
    const val ALBUMS = "albums"
    const val ARTISTS = "artists"
    const val TRACKS = "tracks"
    const val GENRES = "genres"

    const val ALBUM_DETAIL_PATTERN = "album/{albumId}"
    fun albumDetail(albumId: String) = "album/$albumId"

    const val ARTIST_DETAIL_PATTERN = "artist/{artistId}"
    fun artistDetail(artistId: String) = "artist/$artistId"

    const val NOW_PLAYING = "now-playing"

    const val SETTINGS = "settings"
}
