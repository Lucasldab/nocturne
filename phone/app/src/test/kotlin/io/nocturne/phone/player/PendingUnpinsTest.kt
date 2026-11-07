package io.nocturne.phone.player

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PendingUnpinsTest {
    private val committed = mutableListOf<String>()
    private val p = PendingUnpins { committed.add(it) }

    @Test fun undoWritesNothing() {
        val t = p.stage("a")
        assertTrue(p.undo(t))
        p.commit(t) // late commit (snackbar finally) is a no-op
        assertEquals(emptyList<String>(), committed)
    }

    @Test fun commitOnce() {
        val t = p.stage("a")
        p.commit(t)
        p.commit(t)
        assertFalse(p.undo(t))
        assertEquals(listOf("a"), committed)
    }

    @Test fun commitAllFlushesPending() {
        p.stage("a")
        val t = p.stage("b")
        p.undo(t)
        p.stage("c")
        p.commitAll()
        assertEquals(listOf("a", "c"), committed)
    }
}
