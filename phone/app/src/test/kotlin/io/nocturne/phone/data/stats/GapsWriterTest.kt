package io.nocturne.phone.data.stats

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Pure JVM: event building is a companion function so it needs no SAF. */
class GapsWriterTest {

    private val sha = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d"

    @Test fun normalize_collapsesNewlinesAndTrims() {
        assertEquals("a b c", GapsWriter.normalize("  a\nb\r\n\nc  "))
    }

    @Test fun buildEvent_carriesContext() {
        val e = GapsWriter.buildEvent("queue  ", "now-playing", sha, "0.4.49-dev", 1745679800000L)
        assertEquals(GapEvent(1, 1745679800000L, "queue", "now-playing", sha, "0.4.49-dev"), e)
    }

    @Test fun buildEvent_dropsTrackThatIsNotASha() {
        val e = GapsWriter.buildEvent("x", "now-playing", "content://not-a-sha", "0.4.49-dev", 1L)
        assertNull(e.track)
    }

    @Test(expected = IllegalArgumentException::class)
    fun buildEvent_rejectsBlank() {
        GapsWriter.buildEvent(" \n ", "now-playing", null, "0.4.49-dev", 1L)
    }

    @Test(expected = IllegalArgumentException::class)
    fun buildEvent_rejectsOverCap() {
        GapsWriter.buildEvent("x".repeat(GapsWriter.MAX_CHARS + 1), "now-playing", null, "0.4.49-dev", 1L)
    }

    @Test fun buildEvent_acceptsExactlyCap() {
        val e = GapsWriter.buildEvent("x".repeat(GapsWriter.MAX_CHARS), "now-playing", null, "0.4.49-dev", 1L)
        assertEquals(GapsWriter.MAX_CHARS, e.text.length)
    }
}
