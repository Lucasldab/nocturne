package io.nocturne.phone.ui.system

import androidx.core.net.toUri
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.catalog.CatalogReconciler
import io.nocturne.phone.data.catalog.ManifestReconciler
import io.nocturne.phone.data.db.entity.DownloadEntity
import io.nocturne.phone.ui.browser.BrowserViewModel
import io.nocturne.phone.ui.settings.RelativeTimeFormatter
import kotlinx.coroutines.launch

/**
 * Sync / syncthing screen — inline utility-mode content slot (no Scaffold /
 * TopAppBar / back-button; BrowserRoot owns chrome).
 *
 * Phone has no INTERNET permission (CROSS-01) so we cannot call Syncthing's
 * REST API. Live progress for pinned-but-not-resident tracks is derived from
 * SAF stats on Syncthing's `.syncthing.<name>.tmp` staging files (see
 * SyncProgressRepository). Peer-list / codec / connection state still
 * unavailable — those genuinely require Syncthing's REST and would force
 * dropping CROSS-01.
 */
@Composable
fun SyncScreen(container: AppContainer, browserVm: BrowserViewModel? = null) {
    val lastImport by container.syncPrefs.lastImportAt.collectAsStateWithLifecycle(initialValue = null)
    val lastStats by container.syncPrefs.lastStatsSyncAt.collectAsStateWithLifecycle(initialValue = null)
    val metaUri by container.syncPrefs.metaTreeUri.collectAsStateWithLifecycle(initialValue = null)
    val musicUri by container.syncPrefs.musicTreeUri.collectAsStateWithLifecycle(initialValue = null)

    var deviceId by remember { mutableStateOf("") }
    LaunchedEffect(Unit) {
        deviceId = container.syncPrefs.deviceId()
    }

    val scope = rememberCoroutineScope()
    var refreshing by remember { mutableStateOf(false) }
    var lastRefreshLabel by remember { mutableStateOf<String?>(null) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(horizontal = 16.dp)
            .padding(bottom = 80.dp)
            .verticalScroll(rememberScrollState()),
    ) {
        TerminalPrompt("~/sync", modifier = Modifier.padding(top = 16.dp))
        ScreenHero("syncthing")

        // Status row.
        val configured = metaUri != null && musicUri != null
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.padding(top = 8.dp),
        ) {
            Dot(if (configured) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
            Text(
                text = if (configured) "ready · local" else "not configured",
                style = monoStyle(12),
                color = if (configured) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 8.dp),
            )
        }

        SectionHeader("folders")
        FolderRow(
            name = "nocturne-meta",
            kind = "bidirectional",
            lastSync = lastImport?.let { "imported $it" } ?: "no imports yet",
            active = metaUri != null,
        )
        FolderRow(
            name = "nocturne-files",
            kind = "receive only",
            lastSync = "—",
            active = musicUri != null,
        )

        SectionHeader("device")
        KV("device id",  if (deviceId.isEmpty()) "…" else deviceId)
        KV("meta uri",   metaUri?.let { it.toUri().lastPathSegment } ?: "—")
        KV("music uri",  musicUri?.let { it.toUri().lastPathSegment } ?: "—")
        KV("last meta sync",  lastImport ?: "—")
        KV(
            "last stats sync",
            lastStats?.let { RelativeTimeFormatter.formatRelativeTime(it) } ?: "—",
        )

        SectionHeader("recent activity")
        val anyActivity = lastStats != null || lastImport != null
        if (!anyActivity) {
            Text(
                text = "no activity yet",
                style = monoStyle(12),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp),
            )
        } else {
            lastStats?.let { ts ->
                Activity(
                    time = RelativeTimeFormatter.formatRelativeTime(ts),
                    text = "appended stats event",
                )
            }
            lastImport?.let { iso ->
                Activity(time = "—", text = "imported catalog at $iso")
            }
        }

        // Manual reconcile fallback. AppRoot already polls manifest.json
        // mtime every 45s while foregrounded — this button covers the case
        // where the user wants instant feedback after pinning. Tapping
        // re-runs the same reconciler the poll loop calls.
        SectionHeader("manifest")
        val ctx = androidx.compose.ui.platform.LocalContext.current.applicationContext
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 4.dp, bottom = 4.dp)
                .clickable(enabled = metaUri != null && !refreshing) {
                    val uri = metaUri ?: return@clickable
                    refreshing = true
                    scope.launch {
                        val catalog = CatalogReconciler.reconcile(
                            ctx, uri, container.db, container.importer, container.syncPrefs,
                            container.queueRepository,
                        )
                        if (catalog == null) {
                            ManifestReconciler.reconcile(ctx, uri, container.db)
                        }
                        refreshing = false
                        lastRefreshLabel = "refreshed just now"
                    }
                }
                .padding(vertical = 8.dp),
        ) {
            Text(
                text = if (refreshing) "$ refreshing…" else "$ refresh now",
                style = monoStyle(13),
                color = if (metaUri == null) {
                    MaterialTheme.colorScheme.onSurfaceVariant
                } else {
                    MaterialTheme.colorScheme.primary
                },
            )
        }
        lastRefreshLabel?.let {
            Text(
                text = it,
                style = monoStyle(11),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(bottom = 4.dp),
            )
        }

        // Live download progress for pinned-not-resident tracks. Derived
        // from SAF stats on Syncthing's staging files (no network — CROSS-01
        // intact). See SyncProgressRepository.
        if (browserVm != null) {
            val progress by browserVm.pinnedDownloadProgress.collectAsStateWithLifecycle()
            SectionHeader("downloads")
            if (progress.pendingCount == 0) {
                Text(
                    text = "all pinned tracks resident",
                    style = monoStyle(12),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 4.dp),
                )
            } else {
                val pct = progress.aggregate?.let { (it * 100f).toInt() }
                val headline = if (pct != null) {
                    "syncing ${progress.pendingCount} · $pct%"
                } else {
                    "syncing ${progress.pendingCount} · queued"
                }
                Text(
                    text = headline,
                    style = monoStyle(13),
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.padding(top = 4.dp, bottom = 4.dp),
                )
                val activeCount = progress.perTrack.values.count { it != null && it < 1f }
                val readyCount = progress.perTrack.values.count { it != null && it >= 1f }
                val unknownCount = progress.pendingCount - activeCount - readyCount
                if (activeCount > 0) KV("active", activeCount.toString())
                if (readyCount > 0) KV("awaiting flip", readyCount.toString())
                if (unknownCount > 0) KV("queued", unknownCount.toString())
            }
        }

        // -------------------------------------------------------------------
        // $ download — phone-initiated request to flacget on the desktop.
        // Writes downloads-phone-<dev>.jsonl via DownloadsWriter; the daemon's
        // `download` subcommand shells flacget per request and echoes status
        // back through downloads-desktop.jsonl which DownloadStatusReader tails.
        // -------------------------------------------------------------------
        SectionHeader("download")
        DownloadSection(container = container)

        SectionHeader("note")
        // Compose has no built-in dashed border without a custom DrawScope;
        // a 1dp solid border + the explanatory text below is sufficient.
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 4.dp)
                .border(1.dp, MaterialTheme.colorScheme.surfaceVariant)
                .padding(12.dp),
        ) {
            Text(
                text = "Peer list, codec and rate require Syncthing REST — phone app has no INTERNET permission by design (CROSS-01).",
                style = monoStyle(12),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun DownloadSection(container: AppContainer) {
    val vm: DownloadsViewModel = viewModel(factory = DownloadsVMFactory(container))
    val recent by vm.recent.collectAsStateWithLifecycle()
    var query by remember { mutableStateOf("") }

    // Periodic 5s status poll, scoped to this section's visibility.
    LaunchedEffect(Unit) { vm.pollLoop() }

    Column(modifier = Modifier.fillMaxWidth().padding(top = 4.dp)) {
        // Terminal-style input. Prefix "$ " is rendered separately so it
        // looks like a typed prompt rather than part of the value.
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = "$ ",
                style = monoStyle(13),
                color = MaterialTheme.colorScheme.primary,
            )
            androidx.compose.material3.OutlinedTextField(
                value = query,
                onValueChange = { query = it },
                modifier = Modifier
                    .weight(1f)
                    .padding(end = 8.dp),
                singleLine = true,
                placeholder = {
                    Text(
                        text = "artist - title  /  spotify url",
                        style = monoStyle(12),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                },
                textStyle = monoStyle(13).copy(
                    color = MaterialTheme.colorScheme.onSurface,
                ),
                colors = androidx.compose.material3.OutlinedTextFieldDefaults.colors(
                    focusedContainerColor = androidx.compose.ui.graphics.Color.Transparent,
                    unfocusedContainerColor = androidx.compose.ui.graphics.Color.Transparent,
                ),
            )
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 4.dp)
                .clickable(enabled = query.isNotBlank()) {
                    vm.submit(query)
                    query = ""
                }
                .padding(vertical = 8.dp),
        ) {
            Text(
                text = "$ submit",
                style = monoStyle(13),
                color = if (query.isBlank()) {
                    MaterialTheme.colorScheme.onSurfaceVariant
                } else {
                    MaterialTheme.colorScheme.primary
                },
            )
        }

        if (recent.isEmpty()) {
            Text(
                text = "no requests yet",
                style = monoStyle(12),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp),
            )
        } else {
            // Cap the rendered list — the underlying StateFlow already limits
            // to 50, but the UI further trims to 10 most-recent so the section
            // doesn't dominate the Sync screen. Older rows remain in the DB.
            recent.take(10).forEach { DownloadRow(it) }
        }
    }
}

@Composable
private fun DownloadRow(row: DownloadEntity) {
    val stateColor = when (row.state) {
        "running" -> MaterialTheme.colorScheme.primary
        "done"    -> MaterialTheme.colorScheme.primary
        "error"   -> MaterialTheme.colorScheme.error
        else      -> MaterialTheme.colorScheme.onSurfaceVariant
    }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = row.state,
                style = monoStyle(11),
                color = stateColor,
                modifier = Modifier.width(70.dp),
            )
            Text(
                text = row.query,
                style = monoStyle(12),
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
        }
        val msg = row.message
        if (!msg.isNullOrBlank() && row.state == "error") {
            Text(
                text = msg,
                style = monoStyle(11),
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(start = 70.dp, top = 2.dp),
            )
        }
    }
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(MaterialTheme.colorScheme.surfaceVariant),
    )
}

@Composable
private fun Dot(color: Color) {
    Box(
        modifier = Modifier
            .size(8.dp)
            .clip(CircleShape)
            .background(color),
    )
}

@Composable
private fun KV(k: String, v: String) {
    Row(modifier = Modifier.padding(vertical = 2.dp)) {
        Text(
            text = k,
            style = monoStyle(12),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(110.dp),
        )
        Text(
            text = v,
            style = monoStyle(12),
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}

@Composable
private fun FolderRow(name: String, kind: String, lastSync: String, active: Boolean) {
    Column {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 12.dp),
        ) {
            Dot(if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
            Text(
                text = name,
                style = monoStyle(14),
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier
                    .weight(1f)
                    .padding(start = 8.dp),
            )
            Text(
                text = kind,
                style = monoStyle(11),
                color = if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Text(
            text = lastSync,
            style = monoStyle(11),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 16.dp, bottom = 4.dp),
        )
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant),
        )
    }
}

@Composable
private fun Activity(time: String, text: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(
            text = time,
            style = monoStyle(12),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(80.dp),
        )
        Text(
            text = text,
            style = monoStyle(12),
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}
