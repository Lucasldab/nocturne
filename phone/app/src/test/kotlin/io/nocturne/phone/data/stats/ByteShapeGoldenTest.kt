package io.nocturne.phone.data.stats

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Phase 6 byte-shape gate (D-09).
 *
 * Asserts kotlinx.serialization 1.8.1 produces output byte-for-byte identical
 * to the committed goldens for every event type. If this test fails, the
 * Phase 7 desktop ingester will not parse the phone's output — the entire
 * stats loop is broken at the wire level.
 *
 * Pure JVM (no Robolectric) — runs as part of testDebugUnitTest, fails fast
 * before any device-bound test.
 */
class ByteShapeGoldenTest {

    private val json = Json

    private fun loadGolden(name: String): List<String> =
        requireNotNull(javaClass.classLoader)
            .getResourceAsStream("jsonl-goldens/$name")
            .use { stream ->
                requireNotNull(stream) { "missing test resource jsonl-goldens/$name" }
                    .bufferedReader(Charsets.UTF_8)
                    .readLines()
                    .filter { it.isNotEmpty() }
            }

    @Test fun statsGoldenLine1_play() {
        val expected = loadGolden("stats-golden.jsonl")[0]
        val event = StatsEvent(
            v = 1,
            ts = 1745678910123L,
            kind = "play",
            track = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d",
            playedMs = 231400L,
            durationMs = 237000L,
        )
        assertEquals(expected, json.encodeToString(StatsEvent.serializer(), event))
    }

    @Test fun statsGoldenLine2_skip() {
        val expected = loadGolden("stats-golden.jsonl")[1]
        val event = StatsEvent(
            v = 1,
            ts = 1745679200000L,
            kind = "skip",
            track = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d",
            playedMs = 4200L,
            durationMs = 237000L,
        )
        assertEquals(expected, json.encodeToString(StatsEvent.serializer(), event))
    }

    @Test fun statsGoldenLine3_play_fullDuration() {
        val expected = loadGolden("stats-golden.jsonl")[2]
        val event = StatsEvent(
            v = 1,
            ts = 1745679300000L,
            kind = "play",
            track = "b1c2d3e4f50617283940a1b2c3d4e5f60718293a4b5c6d7e8f900112233445dd",
            playedMs = 180000L,
            durationMs = 180000L,
        )
        assertEquals(expected, json.encodeToString(StatsEvent.serializer(), event))
    }

    @Test fun likesGoldenLine1_track_liked() {
        val expected = loadGolden("likes-golden.jsonl")[0]
        val event = LikeEvent(
            v = 1,
            ts = 1745679200000L,
            unit = "track",
            id = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d",
            liked = true,
        )
        assertEquals(expected, json.encodeToString(LikeEvent.serializer(), event))
    }

    @Test fun likesGoldenLine2_track_unliked() {
        val expected = loadGolden("likes-golden.jsonl")[1]
        val event = LikeEvent(
            v = 1,
            ts = 1745679400000L,
            unit = "track",
            id = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d",
            liked = false,
        )
        assertEquals(expected, json.encodeToString(LikeEvent.serializer(), event))
    }

    @Test fun likesGoldenLine3_album_liked() {
        val expected = loadGolden("likes-golden.jsonl")[2]
        val event = LikeEvent(
            v = 1,
            ts = 1745679500000L,
            unit = "album",
            id = "alb_abcd1234",
            liked = true,
        )
        assertEquals(expected, json.encodeToString(LikeEvent.serializer(), event))
    }

    @Test fun pinsGoldenLine1_album_pinned() {
        val expected = loadGolden("pins-golden.jsonl")[0]
        val event = PinEvent(
            v = 1,
            ts = 1745679500000L,
            unit = "album",
            id = "alb_abcd1234",
            pinned = true,
        )
        assertEquals(expected, json.encodeToString(PinEvent.serializer(), event))
    }

    @Test fun pinsGoldenLine2_track_pinned() {
        val expected = loadGolden("pins-golden.jsonl")[1]
        val event = PinEvent(
            v = 1,
            ts = 1745679600000L,
            unit = "track",
            id = "b1c2d3e4f50617283940a1b2c3d4e5f60718293a4b5c6d7e8f900112233445dd",
            pinned = true,
        )
        assertEquals(expected, json.encodeToString(PinEvent.serializer(), event))
    }

    @Test fun pinsGoldenLine3_album_unpinned() {
        val expected = loadGolden("pins-golden.jsonl")[2]
        val event = PinEvent(
            v = 1,
            ts = 1745679700000L,
            unit = "album",
            id = "alb_abcd1234",
            pinned = false,
        )
        assertEquals(expected, json.encodeToString(PinEvent.serializer(), event))
    }
}
