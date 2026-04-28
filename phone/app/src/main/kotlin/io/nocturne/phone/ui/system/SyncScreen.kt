package io.nocturne.phone.ui.system

import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.ui.settings.RelativeTimeFormatter

/**
 * Quick task 260428-7zc — Sync / syncthing screen. Mirrors
 * /tmp/nocturne-design/nocturne/project/screens-system.jsx lines 94-152, but
 * intentionally narrower than the JSX mock: live Syncthing connection state
 * (transfer progress, peer ip/port, codec) is NOT surfaced on-device by
 * design (CROSS-03 forbids hitting the daemon REST). Fields we cannot know
 * locally render as "—" with an explanatory note.
 *
 * Reads ONLY existing SyncPrefs flows. No new network calls. audit-network.sh
 * stays green.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SyncScreen(container: AppContainer, onBack: () -> Unit) {
    val lastImport by container.syncPrefs.lastImportAt.collectAsStateWithLifecycle(initialValue = null)
    val lastStats by container.syncPrefs.lastStatsSyncAt.collectAsStateWithLifecycle(initialValue = null)
    val metaUri by container.syncPrefs.metaTreeUri.collectAsStateWithLifecycle(initialValue = null)
    val musicUri by container.syncPrefs.musicTreeUri.collectAsStateWithLifecycle(initialValue = null)

    var deviceId by remember { mutableStateOf("") }
    LaunchedEffect(Unit) {
        deviceId = container.syncPrefs.deviceId()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Sync", style = MaterialTheme.typography.titleMedium) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = "Back",
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface,
                    titleContentColor = MaterialTheme.colorScheme.onSurface,
                    navigationIconContentColor = MaterialTheme.colorScheme.onSurface,
                ),
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .background(MaterialTheme.colorScheme.background)
                .padding(padding)
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
            KV("meta uri",   metaUri?.let { Uri.parse(it).lastPathSegment } ?: "—")
            KV("music uri",  musicUri?.let { Uri.parse(it).lastPathSegment } ?: "—")
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
                    text = "Live Syncthing connection state is not surfaced on-device by design (CROSS-03).",
                    style = monoStyle(12),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
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
