package io.nocturne.phone.data.db.entity

import androidx.compose.runtime.Immutable
import androidx.room.Entity
import androidx.room.PrimaryKey

/**
 * Phone-initiated download request — the "Download a new song" screen drops
 * one row here per submit, the [io.nocturne.phone.data.stats.DownloadsWriter]
 * drains it to JSONL, and the desktop daemon's `download` command echoes the
 * lifecycle back through `downloads-desktop.jsonl` which the
 * [io.nocturne.phone.data.sync.DownloadStatusReader] applies via
 * [io.nocturne.phone.data.db.dao.DownloadDao.updateStatus].
 *
 * State machine (matches daemon-side download.c):
 *   - "pending"  — local row only, not yet emitted to JSONL.
 *   - "queued"   — JSONL line written; daemon hasn't picked it up yet.
 *   - "running"  — daemon has exec'd flacget (echoed via status JSONL).
 *   - "done"     — flacget exited 0 (echoed via status JSONL).
 *   - "error"    — flacget non-zero (echoed via status JSONL); [message]
 *                   carries the diagnostic ("flacget rc=120").
 *
 * Schema v=5: introduced alongside the download workflow.
 */
@Entity(tableName = "downloads")
@Immutable
data class DownloadEntity(
    @PrimaryKey val id: String,            // UUID string
    val query: String,                     // search query or URL
    val requestedAt: Long,                 // System.currentTimeMillis()
    val state: String = "pending",         // see state machine above
    val message: String? = null,           // last status message (error detail)
    val synced: Boolean = false,           // false until DownloadsWriter drains
    val statusUpdatedAt: Long = 0L,        // ts of last status line applied
)
