package io.nocturne.phone.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.nocturne.phone.data.AppContainer

/**
 * Phase 6 (STATS-06 / UI-SPEC Surface 3): read-only Settings screen.
 *
 * Single section "STATS SYNC" displaying the relative time of the last
 * successful JSONL append (proxy for "ready to sync" — phone has no direct
 * visibility into desktop ingestion, per CONTEXT.md D-27).
 *
 * No buttons, no toggles, no actions in v1. Future polish phases may add
 * sleep timer / theme switcher etc. — out of scope for Phase 6.
 *
 * Animation gate: per UI-SPEC, no animated-visibility / fade primitives between
 * the empty state and the timestamp row — recompose-on-Flow-emit is the only
 * motion. Library-internal Scaffold/TopAppBar inset adjustments are exempt.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(container: AppContainer) {
    val lastSync by container.syncPrefs.lastStatsSyncAt
        .collectAsStateWithLifecycle(initialValue = null)

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text("Settings", style = MaterialTheme.typography.titleMedium)
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface,
                    titleContentColor = MaterialTheme.colorScheme.onSurface,
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
                .verticalScroll(rememberScrollState()),
        ) {
            // Section header — UI-SPEC: literal "STATS SYNC", labelMedium / onSurfaceVariant
            Text(
                text = "STATS SYNC",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 24.dp, bottom = 8.dp),
            )
            // Primary row — empty-state vs. last-event copy verbatim per UI-SPEC.
            val ts = lastSync
            Text(
                text = if (ts == null) {
                    "No events logged yet"
                } else {
                    "Last event logged: ${RelativeTimeFormatter.formatRelativeTime(ts)}"
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            // Caption — verbatim per UI-SPEC Copywriting Contract.
            Text(
                text = "Sync delivery depends on WiFi and Syncthing.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp),
            )
        }
    }
}
