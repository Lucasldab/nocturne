package io.nocturne.phone.player

import androidx.media3.common.C
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.source.ShuffleOrder
import kotlin.random.Random

/**
 * ExoPlayer shuffle order that keeps queue edits meaningful while shuffle is on.
 *
 * ExoPlayer's [ShuffleOrder.DefaultShuffleOrder] drops every inserted item at a
 * random point of the play order, so "play next" with shuffle on played the
 * track whenever, and "add to queue" did not mean "last". This order:
 *
 *   - filling an empty queue (setMediaItems) → random permutation, as before;
 *   - appending at the end of the list       → appended to the end of the play order;
 *   - inserting anywhere else (play next)    → placed right after its list
 *     predecessor in the play order, in list order. Play next inserts after
 *     the current item (and after earlier play-next items), so the inserted
 *     tracks play next, first-queued first, with shuffle on or off.
 *
 * Pure index bookkeeping; no Android dependency beyond the Media3 interface.
 */
@UnstableApi
class QueueShuffleOrder private constructor(
    private val shuffled: IntArray,
    private val random: Random,
) : ShuffleOrder {

    /** Fresh, empty order. [seed] drives the permutation of filled queues. */
    constructor(seed: Long = System.currentTimeMillis()) : this(IntArray(0), Random(seed))

    /** Restore a known play order (indices into the list, first to last). */
    constructor(order: IntArray, seed: Long) : this(order.copyOf(), Random(seed))

    private val indexInShuffled = IntArray(shuffled.size).also { inv ->
        shuffled.forEachIndexed { pos, idx -> inv[idx] = pos }
    }

    override fun getLength(): Int = shuffled.size

    override fun getNextIndex(index: Int): Int {
        val pos = indexInShuffled[index] + 1
        return if (pos < shuffled.size) shuffled[pos] else C.INDEX_UNSET
    }

    override fun getPreviousIndex(index: Int): Int {
        val pos = indexInShuffled[index] - 1
        return if (pos >= 0) shuffled[pos] else C.INDEX_UNSET
    }

    override fun getLastIndex(): Int = if (shuffled.isEmpty()) C.INDEX_UNSET else shuffled.last()

    override fun getFirstIndex(): Int = if (shuffled.isEmpty()) C.INDEX_UNSET else shuffled.first()

    override fun cloneAndInsert(insertionIndex: Int, insertionCount: Int): ShuffleOrder {
        if (insertionCount == 0) return this
        val inserted = IntArray(insertionCount) { insertionIndex + it }
        if (shuffled.isEmpty()) {
            inserted.shuffle(random)
            return QueueShuffleOrder(inserted, random)
        }
        // Shift existing indices at or past the insertion point.
        val shifted = IntArray(shuffled.size) { pos ->
            val idx = shuffled[pos]
            if (idx >= insertionIndex) idx + insertionCount else idx
        }
        val at = when {
            insertionIndex >= shuffled.size -> shifted.size // append → end of play order
            insertionIndex == 0 -> 0
            else -> indexInShuffled[insertionIndex - 1] + 1 // right after the list predecessor
        }
        val out = IntArray(shifted.size + insertionCount)
        shifted.copyInto(out, 0, 0, at)
        inserted.copyInto(out, at)
        shifted.copyInto(out, at + insertionCount, at, shifted.size)
        return QueueShuffleOrder(out, random)
    }

    override fun cloneAndRemove(indexFrom: Int, indexToExclusive: Int): ShuffleOrder {
        val removed = indexToExclusive - indexFrom
        if (removed == 0) return this
        val out = IntArray(shuffled.size - removed)
        var o = 0
        for (idx in shuffled) {
            when {
                idx < indexFrom -> out[o++] = idx
                idx >= indexToExclusive -> out[o++] = idx - removed
            }
        }
        return QueueShuffleOrder(out, random)
    }

    override fun cloneAndClear(): ShuffleOrder = QueueShuffleOrder(IntArray(0), random)

    /** Play order snapshot, for tests. */
    internal fun order(): List<Int> = shuffled.toList()
}
