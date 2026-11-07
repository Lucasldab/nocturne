package io.nocturne.phone.player

/**
 * Pure queue bookkeeping shared by [PlayerViewModel] and the Now Playing
 * queue view. Kept free of Media3 types so it is unit-testable on the JVM.
 */
object QueueOps {
    /**
     * MediaMetadata extra marking items the user queued by hand, as opposed to
     * the context the queue was built from (album, track list, shuffle).
     * Value is [QUEUED_NEXT] or [QUEUED_END].
     */
    const val EXTRA_QUEUED_BY = "nocturne.queuedBy"
    const val QUEUED_NEXT = "next"
    const val QUEUED_END = "end"

    /** MediaMetadata extra: unique id of one queue entry (see [entryKeys]). */
    const val EXTRA_ENTRY_ID = "nocturne.entryId"

    /**
     * List index for a "play next" insert: right after the current item and
     * after any play-next items already waiting there, so several play-next
     * taps play in the order they were made (not newest-first).
     */
    fun playNextInsertIndex(currentIndex: Int, itemCount: Int, isPlayNext: (Int) -> Boolean): Int {
        if (itemCount <= 0) return 0
        var i = (currentIndex + 1).coerceIn(0, itemCount)
        while (i < itemCount && isPlayNext(i)) i++
        return i
    }

    /**
     * Play order as list indices, walked from [first] via [next] (which returns
     * a negative value at the end). Falls back to list order if the walk does
     * not cover every item exactly once (timeline mid-update).
     */
    fun playOrder(itemCount: Int, first: Int, next: (Int) -> Int): List<Int> {
        val listOrder = (0 until itemCount).toList()
        if (itemCount == 0 || first !in 0 until itemCount) return listOrder
        val seen = BooleanArray(itemCount)
        val out = ArrayList<Int>(itemCount)
        var i = first
        while (i in 0 until itemCount && !seen[i]) {
            seen[i] = true
            out.add(i)
            i = next(i)
        }
        return if (out.size == itemCount) out else listOrder
    }

    /** History (already played), current, and upcoming split of [order]. */
    data class Split(val history: List<Int>, val current: Int?, val upcoming: List<Int>)

    fun split(order: List<Int>, currentIndex: Int): Split {
        val pos = order.indexOf(currentIndex)
        if (pos < 0) return Split(emptyList(), null, order)
        return Split(order.subList(0, pos), currentIndex, order.subList(pos + 1, order.size))
    }

    /**
     * Stable, unique LazyColumn keys for a queue that may hold the same track
     * twice. Keying by mediaId alone crashes the list as soon as both copies
     * are on screen. Entries carry a per-entry id ([EXTRA_ENTRY_ID]) that
     * survives moves and removals of other rows; entries without one (e.g.
     * built by an older version, restored from disk) fall back to
     * `<mediaId>#<occurrence>`, and any id seen twice gets the same suffix.
     */
    fun entryKeys(mediaIds: List<String>, entryIds: List<String?> = List(mediaIds.size) { null }): List<String> {
        val seen = HashMap<String, Int>()
        return mediaIds.mapIndexed { i, id ->
            val base = entryIds.getOrNull(i)?.let { "e:$it" } ?: id
            val n = seen.getOrDefault(base, 0)
            seen[base] = n + 1
            if (base.startsWith("e:") && n == 0) base else "$base#$n"
        }
    }
}
