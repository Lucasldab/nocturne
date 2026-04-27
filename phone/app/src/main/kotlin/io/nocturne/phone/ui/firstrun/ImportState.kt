package io.nocturne.phone.ui.firstrun

import io.nocturne.phone.data.catalog.ImportResult
import io.nocturne.phone.data.catalog.Stage

/**
 * Unidirectional UI state for the first-run / re-import flow.
 *
 *   NotStarted     → idle, FirstRunScreen rendered.
 *   PickingFolder  → SAF picker is open (transient; framework-driven).
 *   Importing(...) → importer is running; total=0 means indeterminate.
 *   Failed(msg)    → exception bubbled out of the importer; offer retry.
 *   Succeeded(r)   → done; AppRoot transitions to BrowserPlaceholder.
 */
sealed interface ImportState {
    data object NotStarted : ImportState
    data object PickingFolder : ImportState
    data class Importing(val stage: Stage, val done: Int, val total: Int) : ImportState
    data class Failed(val message: String) : ImportState
    data class Succeeded(val result: ImportResult) : ImportState
}
