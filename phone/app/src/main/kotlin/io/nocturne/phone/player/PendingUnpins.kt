package io.nocturne.phone.player

/**
 * Mini-player hold-to-unpin with undo.
 *
 * The unpin is staged, not written: nothing touches the pins table or the
 * meta JSONL until the undo window closes. [commit] then runs the normal
 * unpin path (pin row tombstone + unsync action, drained to the meta folder);
 * [undo] just forgets the staged id, so the pin is left exactly as it was —
 * no re-pin line, no daemon round trip, no re-download.
 *
 * Tokens make a late commit/undo for an older snackbar harmless.
 */
class PendingUnpins(private val commitAction: (trackId: String) -> Unit) {
    private val pending = LinkedHashMap<Long, String>()
    private var nextToken = 1L

    @Synchronized
    fun stage(trackId: String): Long {
        val token = nextToken++
        pending[token] = trackId
        return token
    }

    /** @return true if the unpin was still pending and is now cancelled. */
    @Synchronized
    fun undo(token: Long): Boolean = pending.remove(token) != null

    fun commit(token: Long) {
        val id = synchronized(this) { pending.remove(token) } ?: return
        commitAction(id)
    }

    /** Commit everything still pending (e.g. the ViewModel is going away). */
    fun commitAll() {
        val ids = synchronized(this) { pending.values.toList().also { pending.clear() } }
        ids.forEach(commitAction)
    }

    @Synchronized
    fun isPending(token: Long): Boolean = token in pending
}
