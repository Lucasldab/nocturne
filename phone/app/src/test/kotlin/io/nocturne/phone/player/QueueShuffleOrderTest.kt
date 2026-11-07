package io.nocturne.phone.player

import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import org.junit.Assert.assertEquals
import org.junit.Test

@UnstableApi
class QueueShuffleOrderTest {

    private fun walk(o: QueueShuffleOrder): List<Int> {
        val out = mutableListOf<Int>()
        var i = o.firstIndex
        while (i != C.INDEX_UNSET) { out.add(i); i = o.getNextIndex(i) }
        return out
    }

    private fun base() = QueueShuffleOrder(intArrayOf(3, 0, 4, 1, 2), seed = 1L)

    @Test fun fillingEmptyQueueIsAPermutation() {
        val o = QueueShuffleOrder(seed = 7L).cloneAndInsert(0, 6) as QueueShuffleOrder
        assertEquals((0 until 6).toSet(), o.order().toSet())
        assertEquals(6, o.order().size)
    }

    @Test fun appendGoesToEndOfPlayOrder() {
        val o = base().cloneAndInsert(5, 2) as QueueShuffleOrder
        assertEquals(listOf(3, 0, 4, 1, 2, 5, 6), o.order())
        assertEquals(o.order(), walk(o))
    }

    @Test fun insertPlaysRightAfterListPredecessor() {
        // Current = list index 0 (second in play order). Play next inserts at 1.
        val o = base().cloneAndInsert(1, 1) as QueueShuffleOrder
        // Old indices >= 1 shift by one: 3→4, 4→5, 1→2, 2→3.
        assertEquals(listOf(4, 0, 1, 5, 2, 3), o.order())
    }

    @Test fun repeatedPlayNextKeepsTapOrder() {
        // First play next at 1 (after current 0), second at 2 (after the first).
        val o = base().cloneAndInsert(1, 1).cloneAndInsert(2, 1) as QueueShuffleOrder
        val play = o.order()
        val afterCurrent = play.subList(play.indexOf(0) + 1, play.indexOf(0) + 3)
        assertEquals(listOf(1, 2), afterCurrent)
    }

    @Test fun insertAtFrontGoesFirst() {
        val o = base().cloneAndInsert(0, 1) as QueueShuffleOrder
        assertEquals(listOf(0, 4, 1, 5, 2, 3), o.order())
    }

    @Test fun removeShiftsDown() {
        val o = base().cloneAndRemove(1, 3) as QueueShuffleOrder // drop list 1,2
        assertEquals(listOf(1, 0, 2), o.order())
        assertEquals(C.INDEX_UNSET, o.getPreviousIndex(1))
        assertEquals(2, o.lastIndex)
    }

    @Test fun clearEmpties() {
        val o = base().cloneAndClear()
        assertEquals(0, o.length)
        assertEquals(C.INDEX_UNSET, o.firstIndex)
    }
}
