package io.nocturne.phone.ui.search

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class FtsQueryBuilderTest {
    @Test fun blankInputReturnsNull() {
        assertNull(buildFtsQuery(""))
    }

    @Test fun whitespaceOnlyReturnsNull() {
        assertNull(buildFtsQuery("   "))
    }

    @Test fun simpleAsciiSuffixesStar() {
        assertEquals("cafe*", buildFtsQuery("cafe"))
    }

    @Test fun accentFoldsBeforeBuilding() {
        assertEquals("cafe*", buildFtsQuery("café"))
    }

    @Test fun multiTokenStarOnLast() {
        assertEquals("cafe orange*", buildFtsQuery("café orange"))
    }

    @Test fun sigurRosFolds() {
        assertEquals("sigur ros*", buildFtsQuery("Sigur Rós"))
    }

    @Test fun stripsQuotes() {
        assertEquals("foo bar*", buildFtsQuery("\"foo\" bar"))
    }

    @Test fun stripsColon() {
        assertEquals("foo bar*", buildFtsQuery("foo:bar"))
    }

    @Test fun stripsDashAndCollapsesWhitespace() {
        assertEquals("foo bar*", buildFtsQuery("foo  -bar  "))
    }

    @Test fun allOperatorsReturnsNull() {
        assertNull(buildFtsQuery("***"))
    }

    @Test fun allKeywordsReturnsNull() {
        assertNull(buildFtsQuery("AND OR NEAR"))
    }

    @Test fun mixedCaseKeywordStripped() {
        assertEquals("foo bar*", buildFtsQuery("foo and bar"))
    }

    @Test fun stripsParens() {
        assertEquals("foo bar*", buildFtsQuery("(foo) (bar)"))
    }

    @Test fun stripsPlusOperator() {
        assertEquals("foo bar*", buildFtsQuery("+foo bar"))
    }
}
