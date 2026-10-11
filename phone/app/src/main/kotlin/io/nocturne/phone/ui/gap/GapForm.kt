package io.nocturne.phone.ui.gap

import io.nocturne.phone.data.stats.GapsWriter

/**
 * The gap sheet's logic, kept free of Compose so it unit-tests on the JVM.
 * Text survives a failed send; only a successful one clears it.
 */
class GapForm(private val send: suspend (String) -> Boolean) {
    var text: String = ""
        private set

    val canSend: Boolean get() = GapsWriter.normalize(text).isNotEmpty()

    /** Edits that would pass the cap are refused, so the field stops accepting input. */
    fun edit(new: String) {
        if (new.length <= GapsWriter.MAX_CHARS) text = new
    }

    suspend fun submit(): Boolean {
        if (!canSend) return false
        val ok = send(text)
        if (ok) text = ""
        return ok
    }
}
