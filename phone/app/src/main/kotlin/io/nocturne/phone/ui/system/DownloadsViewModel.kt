package io.nocturne.phone.ui.system

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.entity.DownloadEntity
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import java.util.UUID

/**
 * Drives the `$ download` section of SyncScreen.
 *
 * State:
 *   - [recent]: live Flow of the 50 most-recent download rows.
 *
 * Actions:
 *   - [submit]: inserts a pending row, drains it to JSONL, then kicks the
 *     status reader so the user sees the daemon's "running"/"done" echo
 *     without waiting for the periodic poll. Throws no errors — on JSONL
 *     write failure the row stays `synced = 0` and the next drain call
 *     (e.g. PlaybackService.onCreate) retries.
 *   - [refresh]: one-shot status poll. Bound to the periodic 5s loop
 *     started in [startPollingWhileVisible].
 */
class DownloadsViewModel(
    private val container: AppContainer,
) : ViewModel() {

    val recent: StateFlow<List<DownloadEntity>> =
        container.db.downloadDao().flowRecent()
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.WhileSubscribed(5_000),
                initialValue = emptyList(),
            )

    fun submit(rawQuery: String) {
        val query = rawQuery.trim()
        if (query.isEmpty()) return
        viewModelScope.launch {
            val id = UUID.randomUUID().toString()
            val now = System.currentTimeMillis()
            container.db.downloadDao().upsert(
                DownloadEntity(
                    id = id,
                    query = query,
                    requestedAt = now,
                    state = "pending",
                ),
            )
            container.downloadsWriter.drain()
            // Re-poll the status file shortly after drain so the UI rolls
            // forward to "running" the moment the daemon picks it up. The
            // periodic poll covers the steady-state case.
            delay(750)
            container.downloadStatusReader.poll()
        }
    }

    fun refresh() {
        viewModelScope.launch { container.downloadStatusReader.poll() }
    }

    /**
     * Long-lived 5s status poll. Caller invokes from a LaunchedEffect tied
     * to the `$ download` section's visibility so the loop only runs while
     * the user can see the result.
     */
    suspend fun pollLoop() {
        while (true) {
            container.downloadStatusReader.poll()
            delay(5_000L)
        }
    }
}

class DownloadsVMFactory(private val container: AppContainer) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T =
        DownloadsViewModel(container) as T
}
