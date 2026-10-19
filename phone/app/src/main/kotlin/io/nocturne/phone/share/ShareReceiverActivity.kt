package io.nocturne.phone.share

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import io.nocturne.phone.NocturneApp
import io.nocturne.phone.data.db.entity.DownloadEntity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.UUID

/**
 * Invisible entry point for Spotify links from other apps: share-sheet text
 * (ACTION_SEND) and tapped `open.spotify.com/track` / `spotify:` links
 * (ACTION_VIEW). Writes the same [DownloadEntity] the `$ download` box does,
 * drains it to JSONL, toasts the outcome and finishes — the main UI never
 * opens. Progress shows up in the Sync screen's download list as usual.
 */
class ShareReceiverActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val raw = when (intent?.action) {
            Intent.ACTION_SEND -> intent.getStringExtra(Intent.EXTRA_TEXT)
            Intent.ACTION_VIEW -> intent.dataString
            else -> null
        }
        when (val parsed = parseSharedLink(raw)) {
            is SharedLink.Rejected -> done("nocturne: ${parsed.reason}")
            is SharedLink.Query -> lifecycleScope.launch {
                done(withContext(Dispatchers.IO) { request(parsed.query) })
            }
        }
    }

    private suspend fun request(query: String): String {
        val container = (application as NocturneApp).container
        val dao = container.db.downloadDao()
        val latest = dao.latestFor(query)
        if (latest != null && latest.state in IN_FLIGHT) return "nocturne: already requested"

        val id = UUID.randomUUID().toString()
        dao.upsert(DownloadEntity(id = id, query = query, requestedAt = System.currentTimeMillis()))
        runCatching { container.downloadsWriter.drain() }
        // drain() is a no-op until the meta folder is picked; the row stays
        // pending and PlaybackService.onCreate retries it.
        return if (dao.byId(id)?.state == "queued") {
            "nocturne: download queued"
        } else {
            "nocturne: saved, will send on next sync"
        }
    }

    private fun done(message: String) {
        Toast.makeText(applicationContext, message, Toast.LENGTH_SHORT).show()
        finish()
    }

    private companion object {
        val IN_FLIGHT = setOf("pending", "queued", "running")
    }
}
