package io.nocturne.phone.ui.browser

import io.nocturne.phone.ui.browser.components.LetterIndex
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Pure-JVM bucketing test for [LetterIndex.letterOf]. No Compose, no
 * Robolectric — runs under :app:testDebugUnitTest.
 *
 * The rule under test must match what the DAO `letterFirstIndex` queries do
 * server-side (UPPER(SUBSTR(title, 1, 1)) bucketed to '#' for non-A..Z), so
 * the composable can look up letter -> rowIndex with the same key the DAO
 * emits. Accented letters bucket to '#' because SQLite's UPPER does not
 * decompose accents — consistent bucketing on both ends matters more than
 * alphabetic correctness for the v1 rail.
 */
class LetterIndexTest {

    @Test fun letterOf_aphex_twin_uppercase_A() {
        assertEquals('A', LetterIndex.letterOf("Aphex Twin"))
    }

    @Test fun letterOf_lowercase_aphex_twin_buckets_to_A() {
        assertEquals('A', LetterIndex.letterOf("aphex twin"))
    }

    @Test fun letterOf_punctuation_start_buckets_to_hash() {
        assertEquals('#', LetterIndex.letterOf("¡Forward, Russia!"))
    }

    @Test fun letterOf_digit_start_buckets_to_hash() {
        assertEquals('#', LetterIndex.letterOf("21 Pilots"))
    }

    @Test fun letterOf_empty_string_buckets_to_hash() {
        assertEquals('#', LetterIndex.letterOf(""))
    }

    @Test fun letterOf_accented_start_buckets_to_hash() {
        // SQLite UPPER(SUBSTR(...,1,1)) does not decompose accents; consistency
        // with the DAO is the priority, so 'Émile' lands in '#' on both ends.
        assertEquals('#', LetterIndex.letterOf("Émile"))
    }

    @Test fun letterOf_Z_uppercase() {
        assertEquals('Z', LetterIndex.letterOf("Z"))
    }

    @Test fun letters_set_contains_27_with_hash_first() {
        assertEquals(27, LetterIndex.LETTERS.size)
        assertEquals('#', LetterIndex.LETTERS.first())
        assertEquals('Z', LetterIndex.LETTERS.last())
    }
}
