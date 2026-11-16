package io.nocturne.phone.data.catalog

import org.junit.Assert.assertEquals
import org.junit.Test

class MetaFilesTest {

    @Test
    fun childOfSubfolderTreeJoinsWithSlash() {
        assertEquals(
            "primary:Sync/sync-meta/catalog.json",
            MetaFiles.childDocumentId("primary:Sync/sync-meta", "catalog.json"),
        )
    }

    @Test
    fun childOfVolumeRootTreeHasNoSlash() {
        assertEquals(
            "primary:manifest.json",
            MetaFiles.childDocumentId("primary:", "manifest.json"),
        )
    }

    @Test
    fun trailingSlashIsNotDoubled() {
        assertEquals(
            "primary:meta/catalog.json",
            MetaFiles.childDocumentId("primary:meta/", "catalog.json"),
        )
    }
}
