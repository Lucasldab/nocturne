package io.nocturne.phone.share

import org.junit.Assert.assertEquals
import org.junit.Test

class SharedLinkTest {
    private fun query(q: String) = SharedLink.Query(q)

    @Test fun plainTrackLink() {
        assertEquals(
            query("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
            parseSharedLink("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
        )
    }

    @Test fun trackingParamAndIntlPrefixAreDropped() {
        assertEquals(
            query("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
            parseSharedLink("https://open.spotify.com/intl-pt/track/4uLU6hMCjMI75M1A2tKUQC?si=abc123"),
        )
    }

    @Test fun linkInsideShareText() {
        assertEquals(
            query("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
            parseSharedLink("Check out this song!\nhttps://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC?si=x"),
        )
    }

    @Test fun spotifyUriBecomesWebLink() {
        assertEquals(
            query("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
            parseSharedLink("spotify:track:4uLU6hMCjMI75M1A2tKUQC"),
        )
    }

    @Test fun shortLinkPassesThrough() {
        assertEquals(
            query("https://spotify.link/AbC123xyz"),
            parseSharedLink("listen https://spotify.link/AbC123xyz"),
        )
    }

    @Test fun albumIsRejected() {
        assertEquals(
            SharedLink.Rejected("only Spotify tracks work, not albums"),
            parseSharedLink("https://open.spotify.com/album/1ATL5GLyefJaxhQzSPVrLX?si=1"),
        )
    }

    @Test fun playlistUriIsRejected() {
        assertEquals(
            SharedLink.Rejected("only Spotify tracks work, not playlists"),
            parseSharedLink("spotify:playlist:37i9dQZF1DXcBWIGoYBM5M"),
        )
    }

    @Test fun otherUrlIsRejected() {
        assertEquals(
            SharedLink.Rejected("not a Spotify link"),
            parseSharedLink("https://www.youtube.com/watch?v=dQw4w9WgXcQ"),
        )
    }

    @Test fun plainTextIsAQuery() {
        assertEquals(query("Chelsea Wolfe - Carrion Flowers"), parseSharedLink("  Chelsea Wolfe - Carrion Flowers "))
    }

    @Test fun blankIsRejected() {
        assertEquals(SharedLink.Rejected("nothing to download"), parseSharedLink("  "))
        assertEquals(SharedLink.Rejected("nothing to download"), parseSharedLink(null))
    }

    @Test fun multiLineTextWithoutLinkIsRejected() {
        assertEquals(
            SharedLink.Rejected("share a link or \"artist - title\""),
            parseSharedLink("some lyrics\nmore lyrics"),
        )
    }
}
