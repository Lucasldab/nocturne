package io.nocturne.phone.data.catalog

import java.text.Normalizer
import java.util.Locale

private val combiningMarks = Regex("\\p{InCombiningDiacriticalMarks}+")
private val whitespaceRun = Regex("\\s+")

/**
 * Accent-fold an arbitrary input string for use in `search_blob` (during
 * import) and as a query-time pre-fold (Plan 04-06).
 *
 * Algorithm:
 *   1. NFKD normalize (decomposes "café" → "café").
 *   2. Strip combining diacritical marks (́ etc).
 *   3. lowercase(Locale.ROOT).
 *   4. Collapse runs of whitespace to a single ASCII space.
 *   5. Trim.
 *
 * Preserved properties:
 *   - empty input → empty output (no crash).
 *   - ASCII passes through unchanged besides lowercasing.
 *   - "Sigur Rós" → "sigur ros".
 *   - "Café Bar" → "cafe bar".
 *
 * Whitespace caveat: the regex `\s+` in Java's default mode matches ASCII
 * whitespace only. Non-breaking space (U+00A0) is NOT collapsed by this
 * algorithm. catalog.json is daemon-emitted UTF-8 with normal spaces, so
 * NBSP doesn't appear in practice; documented as a Phase 4 limitation.
 */
fun accentFold(input: String): String {
    if (input.isEmpty()) return ""
    val nfkd = Normalizer.normalize(input, Normalizer.Form.NFKD)
    val stripped = nfkd.replace(combiningMarks, "")
    val lowered = stripped.lowercase(Locale.ROOT)
    return lowered.replace(whitespaceRun, " ").trim()
}
