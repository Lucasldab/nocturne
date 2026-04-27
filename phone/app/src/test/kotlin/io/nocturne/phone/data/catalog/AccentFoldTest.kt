package io.nocturne.phone.data.catalog

import org.junit.Assert.assertEquals
import org.junit.Test

class AccentFoldTest {
    @Test fun ascii_lowercased() {
        assertEquals("ascii", accentFold("ASCII"))
    }

    @Test fun cafe_to_cafe() {
        assertEquals("cafe", accentFold("Café"))
    }

    @Test fun sigur_ros_to_sigur_ros() {
        assertEquals("sigur ros", accentFold("Sigur Rós"))
    }

    @Test fun etoile_to_etoile() {
        assertEquals("etoile", accentFold("Étoile"))
    }

    @Test fun naive_facade_to_naive_facade() {
        assertEquals("naive facade", accentFold("naïve façade"))
    }

    @Test fun multiple_whitespace_collapses_and_trims() {
        assertEquals("multiple spaces", accentFold("  multiple   spaces  "))
    }

    @Test fun ascii_whitespace_split_double_word() {
        // Plan documents that the NBSP (U+00A0) case is a Phase 4 limitation:
        // Java's default \\s+ does not match NBSP. catalog.json is daemon-
        // emitted UTF-8 with normal spaces, so we test the ASCII case here.
        assertEquals("uber uber", accentFold("Über Uber"))
    }

    @Test fun empty_input_returns_empty() {
        assertEquals("", accentFold(""))
    }

    @Test fun blank_input_returns_empty() {
        assertEquals("", accentFold("   \t\n"))
    }

    @Test fun mixed_case_unicode_preserves_ligatures() {
        // NFKD does NOT decompose `æ` (a separate Latin letter, not a
        // precomposed accent). It stays as `æ` after fold; only the `Á`
        // diacritic is stripped. This is intentional — the algorithm is
        // a strip-marks, not a transliteration. Documented for plan 04-06.
        assertEquals("agætis byrjun", accentFold("Ágætis byrjun"))
    }
}
