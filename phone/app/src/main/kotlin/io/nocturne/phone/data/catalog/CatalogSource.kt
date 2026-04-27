package io.nocturne.phone.data.catalog

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStream

/**
 * Abstraction over the metadata source. Production uses the SAF
 * implementation (user-picked tree URI); tests can pass an in-memory or
 * file-backed source.
 */
interface CatalogSource {
    /** Throws IOException if catalog.json cannot be opened. */
    fun openCatalog(): InputStream

    /** Returns null if manifest.json is absent (cold-start case). */
    fun openManifest(): InputStream?
}

/**
 * SAF-backed source. The user picks a tree URI in the first-run flow
 * (Plan 04-04); we persist read permission via
 * `contentResolver.takePersistableUriPermission(...)` and resolve `catalog.json`
 * + `manifest.json` as direct children of that tree.
 */
class SafCatalogSource(
    private val context: Context,
    private val treeUri: Uri,
) : CatalogSource {
    private fun findChild(name: String): DocumentFile? {
        val tree = DocumentFile.fromTreeUri(context, treeUri) ?: return null
        return tree.findFile(name)
    }

    override fun openCatalog(): InputStream {
        val f = findChild("catalog.json")
            ?: throw FileNotFoundException("catalog.json not found under $treeUri")
        return context.contentResolver.openInputStream(f.uri)
            ?: throw IOException("openInputStream returned null for ${f.uri}")
    }

    override fun openManifest(): InputStream? {
        val f = findChild("manifest.json") ?: return null
        return context.contentResolver.openInputStream(f.uri)
    }
}
