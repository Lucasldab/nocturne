package io.nocturne.phone.data.stats

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.SecureRandom

/**
 * Phase 6 (D-21): deviceId is exactly 8 lowercase hex chars from 4 SecureRandom bytes.
 * NOT IMEI / Android ID / serial / hostname (per docs/jsonl-spec.md §3).
 *
 * This is a pure JVM test of the format helper. The actual production code
 * lives in SyncPrefs.deviceId() which is exercised under Robolectric in
 * later plans.
 */
class DeviceIdFormatTest {

    private fun mintDeviceId(rng: SecureRandom): String {
        val bytes = ByteArray(4)
        rng.nextBytes(bytes)
        return bytes.joinToString(separator = "") { "%02x".format(it.toInt() and 0xff) }
    }

    @Test fun mintedIdIsEightLowercaseHex() {
        val rng = SecureRandom()
        val pattern = Regex("^[0-9a-f]{8}\$")
        repeat(100) {
            val id = mintDeviceId(rng)
            assertEquals("expected 8 chars: '$id'", 8, id.length)
            assertTrue("expected match $pattern: '$id'", pattern.matches(id))
        }
    }
}
