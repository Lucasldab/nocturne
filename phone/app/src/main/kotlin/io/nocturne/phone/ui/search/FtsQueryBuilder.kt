package io.nocturne.phone.ui.search

import io.nocturne.phone.data.catalog.accentFold

private val FTS_FORBIDDEN_CHARS = Regex("[\"*\\-+:()]")
private val FTS_KEYWORDS = Regex("\\b(NEAR|AND|OR|NOT)\\b", RegexOption.IGNORE_CASE)
private val WS_RUN = Regex("\\s+")

/**
 * Build an FTS4 MATCH query from raw user input.
 *
 *  1. Accent-fold (so `café` matches the indexed `cafe` searchBlob).
 *  2. Strip FTS reserved operators + boolean keywords (defensive — single-user
 *     trust model means malicious queries are unlikely, but garbage input
 *     would otherwise raise a SQLite syntax error).
 *  3. Collapse whitespace, split into tokens, join with implicit AND.
 *  4. Append `*` to the LAST token for prefix-match typeahead.
 *
 * Returns null when the input is blank, all-whitespace, or yields nothing
 * after the operator/keyword strip — the caller treats null as "no query".
 */
fun buildFtsQuery(rawInput: String): String? {
    val folded = accentFold(rawInput)
    if (folded.isBlank()) return null
    val cleaned = folded
        .replace(FTS_FORBIDDEN_CHARS, " ")
        .replace(FTS_KEYWORDS, " ")
        .replace(WS_RUN, " ")
        .trim()
    if (cleaned.isEmpty()) return null
    val tokens = cleaned.split(' ')
    val tail = tokens.last() + "*"
    return (tokens.dropLast(1) + tail).joinToString(" ")
}
