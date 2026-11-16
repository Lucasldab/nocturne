package io.nocturne.phone.data.catalog

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile

/**
 * Direct lookups of the daemon's files (catalog.json, manifest.json) in the
 * metadata tree.
 *
 * DocumentFile.findFile lists every child of the tree and builds a
 * DocumentFile per entry just to match one name, and the 45 s poll did that
 * for both files on every tick. Here the child's document URI is built from
 * the tree's document id and stat-ed with a single query. If that misses (a
 * provider whose document ids are not path-shaped, or the file is absent),
 * it falls back to findFile.
 */
internal object MetaFiles {

    data class Entry(val uri: Uri, val lastModified: Long)

    /** Document id of [name] directly under the tree document [treeDocId]. */
    fun childDocumentId(treeDocId: String, name: String): String =
        if (treeDocId.endsWith(':') || treeDocId.endsWith('/')) {
            "$treeDocId$name"
        } else {
            "$treeDocId/$name"
        }

    /** Blocking SAF I/O: call from a background dispatcher. */
    fun find(ctx: Context, metaTreeUri: String, name: String): Entry? {
        val tree = metaTreeUri.toUri()
        direct(ctx, tree, name)?.let { return it }
        val f = DocumentFile.fromTreeUri(ctx, tree)?.findFile(name) ?: return null
        return Entry(f.uri, f.lastModified())
    }

    private fun direct(ctx: Context, tree: Uri, name: String): Entry? = try {
        val docId = childDocumentId(DocumentsContract.getTreeDocumentId(tree), name)
        val uri = DocumentsContract.buildDocumentUriUsingTree(tree, docId)
        ctx.contentResolver.query(
            uri,
            arrayOf(DocumentsContract.Document.COLUMN_LAST_MODIFIED),
            null,
            null,
            null,
        )?.use { c ->
            if (c.moveToFirst()) Entry(uri, if (c.isNull(0)) 0L else c.getLong(0)) else null
        }
    } catch (_: Exception) {
        null
    }
}
