package io.nocturne.phone.player

import org.junit.Assert.assertEquals
import org.junit.Test

class QueueOpsTest {

    @Test fun playNextGoesRightAfterCurrent() {
        assertEquals(3, QueueOps.playNextInsertIndex(2, 10) { false })
    }

    @Test fun playNextQueuesBehindEarlierPlayNexts() {
        val next = setOf(3, 4)
        assertEquals(5, QueueOps.playNextInsertIndex(2, 10) { it in next })
    }

    @Test fun playNextAtEndAndEmpty() {
        assertEquals(10, QueueOps.playNextInsertIndex(9, 10) { false })
        assertEquals(0, QueueOps.playNextInsertIndex(-1, 0) { false })
    }

    @Test fun playOrderWalksAndFallsBack() {
        val nxt = mapOf(2 to 0, 0 to 1, 1 to -1)
        assertEquals(listOf(2, 0, 1), QueueOps.playOrder(3, 2) { nxt.getValue(it) })
        // Walk misses an item → list order.
        assertEquals(listOf(0, 1, 2), QueueOps.playOrder(3, 2) { if (it == 2) 0 else -1 })
    }

    @Test fun splitAroundCurrent() {
        val s = QueueOps.split(listOf(2, 0, 3, 1), 3)
        assertEquals(listOf(2, 0), s.history)
        assertEquals(3, s.current)
        assertEquals(listOf(1), s.upcoming)
    }

    @Test fun entryKeysAreUniqueForDuplicates() {
        assertEquals(listOf("a#0", "b#0", "a#1"), QueueOps.entryKeys(listOf("a", "b", "a")))
    }

    @Test fun entryKeysPreferEntryIds() {
        assertEquals(
            listOf("e:x", "e:y", "a#0", "e:x#1"),
            QueueOps.entryKeys(listOf("a", "a", "a", "b"), listOf("x", "y", null, "x")),
        )
    }
}
