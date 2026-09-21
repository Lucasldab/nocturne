package io.nocturne.phone.data.sync

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract

/**
 * Stats a file inside the phone's music tree over SAF.
 *
 * Thin wrapper over the DocumentsContract dance so residency verification and
 * sync-progress probing agree on how a catalog path maps to a document URI.
 *
 * Construct once per pass — [treeDocId] is derived from [musicTreeUri] and
 * reused across the whole batch.
 */
class MusicFileProbe(
    private val resolver: ContentResolver,
    private val musicTreeUri: Uri,
) {
    private val treeDocId: String = DocumentsContract.getTreeDocumentId(musicTreeUri)

    /**
     * Byte size of [relPath] under the music tree, or null when the document
     * does not exist or cannot be read.
     */
    fun sizeOf(relPath: String): Long? {
        if (relPath.isEmpty()) return null
        val docUri = DocumentsContract.buildDocumentUriUsingTree(
            musicTreeUri,
            "$treeDocId/$relPath",
        )
        return try {
            resolver.query(
                docUri,
                arrayOf(DocumentsContract.Document.COLUMN_SIZE),
                null, null, null,
            )?.use { c ->
                if (c.moveToFirst() && !c.isNull(0)) c.getLong(0) else null
            }
        } catch (_: Throwable) {
            null
        }
    }
}
