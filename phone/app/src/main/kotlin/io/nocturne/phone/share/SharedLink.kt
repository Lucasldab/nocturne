package io.nocturne.phone.share

/**
 * Turns whatever another app hands nocturne — a tapped Spotify link, a
 * `spotify:` URI from an "Add to Spotify" button, share-sheet text like
 * "Check out this song! https://open.spotify.com/track/…?si=…", or the
 * `instagram.com/reels/audio/<id>` link Instagram's share button gives for a
 * song — into a download query for flacget, which resolves links to
 * artist and title on hearth.
 *
 * Only track links are accepted: flacget refuses albums and playlists, so
 * rejecting them here gives the answer immediately instead of a round trip
 * through the daemon. Short links (`spotify.link/…`) are passed through
 * untouched — resolving them needs the network, which the phone does not
 * have (CROSS-01); flacget expands them on hearth.
 */
sealed interface SharedLink {
    data class Query(val query: String) : SharedLink
    data class Rejected(val reason: String) : SharedLink
}

private val TRACK_URL = Regex(
    """https?://open\.spotify\.com/(?:intl-[a-z]{2}/)?track/([A-Za-z0-9]+)""",
)
private val TRACK_URI = Regex("""spotify:track:([A-Za-z0-9]+)""")
private val SHORT_URL = Regex("""https?://(?:spotify\.link|spotify\.app\.link)/[A-Za-z0-9]+""")
private val OTHER_SPOTIFY = Regex(
    """(?:open\.spotify\.com/(?:intl-[a-z]{2}/)?|spotify:)(album|playlist|artist|episode|show|user)""",
)
private val INSTAGRAM_AUDIO = Regex("""https?://(?:www\.)?instagram\.com/reels?/audio/(\d+)""")
private val ANY_URL = Regex("""https?://\S+""")

/** Longest plain-text share treated as "artist - title" rather than junk. */
private const val MAX_PLAIN_QUERY = 200

fun parseSharedLink(raw: String?): SharedLink {
    val text = raw?.trim().orEmpty()
    if (text.isEmpty()) return SharedLink.Rejected("nothing to download")

    // Canonical form drops ?si= tracking and the intl- prefix, so the same
    // song shared twice is the same query.
    (TRACK_URL.find(text) ?: TRACK_URI.find(text))?.let {
        return SharedLink.Query("https://open.spotify.com/track/${it.groupValues[1]}")
    }
    SHORT_URL.find(text)?.let { return SharedLink.Query(it.value) }
    INSTAGRAM_AUDIO.find(text)?.let {
        return SharedLink.Query("https://www.instagram.com/reels/audio/${it.groupValues[1]}/")
    }
    OTHER_SPOTIFY.find(text)?.let {
        return SharedLink.Rejected("only Spotify tracks work, not ${it.groupValues[1]}s")
    }
    if (ANY_URL.containsMatchIn(text)) return SharedLink.Rejected("not a song link")
    if (text.length > MAX_PLAIN_QUERY || '\n' in text) {
        return SharedLink.Rejected("share a link or \"artist - title\"")
    }
    return SharedLink.Query(text)
}
