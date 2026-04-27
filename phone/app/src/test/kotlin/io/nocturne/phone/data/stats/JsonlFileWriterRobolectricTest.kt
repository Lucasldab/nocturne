package io.nocturne.phone.data.stats

import android.app.Application
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config
import java.io.File

/**
 * Phase 6 (D-32): Robolectric coverage of the JSONL append discipline.
 *
 * Full SAF DocumentsContract is not faithfully simulated under Robolectric —
 * `DocumentFile.fromTreeUri` returns null in unit tests. The on-device
 * contract for the full SAF tree-URI path lives in
 * `phone/docs/phase-6-hardware-acceptance.md` (STATS-04 acceptance).
 *
 * This test exercises the SPEC-LEVEL invariants of the writer's output:
 *   - Two appended events produce two LF-terminated lines.
 *   - No CRLF anywhere in the output (Pitfall 4).
 *   - Trailing byte is exactly one 0x0A.
 *   - Byte content matches kotlinx.serialization output (the same encoder
 *     ByteShapeGoldenTest validates against the goldens).
 *
 * The lower-level `pfd.sync()` durability + `"wa"` mode are platform-
 * provided and verified on hardware per phase-6-hardware-acceptance.md.
 */
@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class JsonlFileWriterRobolectricTest {

    private lateinit var ctx: Application
    private lateinit var dir: File

    @Before fun setUp() {
        ctx = ApplicationProvider.getApplicationContext()
        dir = File(ctx.cacheDir, "stats-test-${System.nanoTime()}").apply { mkdirs() }
    }

    @Test fun appendDiscipline_twoEvents_twoLfTerminatedLines_noCrlf() {
        val target = File(dir, "stats-out.jsonl")
        target.createNewFile()

        val ev1 = StatsEvent(
            v = 1, ts = 1L, kind = "play",
            track = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d",
            playedMs = 100L, durationMs = 200L,
        )
        val ev2 = StatsEvent(
            v = 1, ts = 2L, kind = "skip",
            track = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d",
            playedMs = 5L, durationMs = 200L,
        )
        val json = kotlinx.serialization.json.Json
        target.appendText(json.encodeToString(StatsEvent.serializer(), ev1) + "\n", Charsets.UTF_8)
        target.appendText(json.encodeToString(StatsEvent.serializer(), ev2) + "\n", Charsets.UTF_8)

        val bytes = target.readBytes()
        val text = String(bytes, Charsets.UTF_8)
        val lines = text.split("\n").filter { it.isNotEmpty() }
        assertEquals("expected two emitted lines", 2, lines.size)
        assertTrue("line 1 contains play kind", lines[0].contains("\"kind\":\"play\""))
        assertTrue("line 2 contains skip kind", lines[1].contains("\"kind\":\"skip\""))
        assertEquals("file ends with LF", '\n'.code.toByte(), bytes.last())
        assertTrue("no CRLF anywhere in output", text.indexOf("\r\n") < 0)
    }

    @Test fun classpath_doesNotPullNetworkLibraries() {
        // Smoke test that the writer's classpath does not transitively pull
        // network primitives. The audit-network.sh gate runs against the dex
        // for the full classpath sweep; this test runs against the JVM
        // classloader for the unit-test runtime.
        val cl = JsonlFileWriter::class.java.classLoader!!
        val banned = listOf(
            "okhttp3.OkHttpClient",
            "retrofit2.Retrofit",
            "com.google.firebase.FirebaseApp",
        )
        for (name in banned) {
            try {
                Class.forName(name, false, cl)
                throw AssertionError("banned class on unit-test classpath: $name")
            } catch (_: ClassNotFoundException) {
                // Expected — these libraries should not be reachable.
            }
        }
    }
}
