package io.nocturne.phone.ui.gap

import io.nocturne.phone.data.stats.GapsWriter
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GapFormTest {

    @Test fun cannotSendBlank() {
        val f = GapForm { true }
        f.edit("  \n ")
        assertFalse(f.canSend)
    }

    @Test fun editBeyondCapIsRefused() {
        val f = GapForm { true }
        f.edit("x".repeat(GapsWriter.MAX_CHARS))
        f.edit("x".repeat(GapsWriter.MAX_CHARS + 1))
        assertEquals(GapsWriter.MAX_CHARS, f.text.length)
    }

    @Test fun successClearsText() = runTest {
        var sent: String? = null
        val f = GapForm { sent = it; true }
        f.edit("want a sleep timer")
        assertTrue(f.submit())
        assertEquals("want a sleep timer", sent)
        assertEquals("", f.text)
    }

    @Test fun failureKeepsText() = runTest {
        val f = GapForm { false }
        f.edit("want a sleep timer")
        assertFalse(f.submit())
        assertEquals("want a sleep timer", f.text)
    }

    @Test fun submitBlankDoesNotCallSend() = runTest {
        var called = false
        val f = GapForm { called = true; true }
        assertFalse(f.submit())
        assertFalse(called)
    }
}
