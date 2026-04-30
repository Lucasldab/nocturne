package io.nocturne.phone.data.db

/**
 * Letter-rail anchor: ('A'..'Z' or '#', 0-based row index against the entity
 * table in its display ORDER BY).
 *
 * Used by the per-screen `letterFirstIndex()` DAO queries that power
 * [io.nocturne.phone.ui.browser.components.LetterScrollRail]. Note that
 * screens which prepend a SectionLabel header item must add 1 to [rowIndex]
 * before passing the result to the rail — the DAO is unaware of UI chrome.
 */
data class LetterAnchor(val letter: String, val rowIndex: Int)
