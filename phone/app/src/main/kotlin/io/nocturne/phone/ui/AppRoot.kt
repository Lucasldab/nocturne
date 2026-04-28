package io.nocturne.phone.ui

import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import io.nocturne.phone.NocturneApp
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.ui.browser.BrowserRoot
import io.nocturne.phone.ui.firstrun.FirstRunScreen
import io.nocturne.phone.ui.firstrun.FirstRunViewModel
import io.nocturne.phone.ui.firstrun.ImportProgressScreen
import io.nocturne.phone.ui.firstrun.ImportState
import io.nocturne.phone.ui.player.FirstPlayNotifGate
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

private const val LOADING_SENTINEL = "__loading__"

/**
 * Root navigation:
 *  - while metaTreeUri flow has not yet emitted: splash.
 *  - if metaTreeUri is null OR DB has 0 tracks: FirstRunScreen, then
 *    ImportProgressScreen on importer state changes.
 *  - if metaTreeUri non-null AND tracks > 0: BrowserPlaceholder
 *    (Plan 04-05 replaces this).
 */
@Composable
fun AppRoot(app: NocturneApp) {
    val container = app.container
    val metaTreeUri by container.syncPrefs.metaTreeUri
        .collectAsStateWithLifecycle(initialValue = LOADING_SENTINEL)
    val musicTreeUri by container.syncPrefs.musicTreeUri
        .collectAsStateWithLifecycle(initialValue = LOADING_SENTINEL)
    var trackCount by remember { mutableIntStateOf(-1) }

    LaunchedEffect(metaTreeUri) {
        if (metaTreeUri != LOADING_SENTINEL) {
            trackCount = container.db.trackDao().count()
        }
    }

    // Manifest reconciliation on every app launch. CatalogImporter only ever
    // ran on first install; without this, `track.isResident` stays frozen at
    // the original import even though the daemon's manifest.json keeps
    // changing as new pins land. Re-read the latest manifest, clear+set the
    // resident flag for every track id in `manifest.resident[]`. Cheap (~18 KB
    // JSON, ~125 ids) and idempotent.
    LaunchedEffect(metaTreeUri, trackCount) {
        if (metaTreeUri == LOADING_SENTINEL || metaTreeUri == null) return@LaunchedEffect
        if (trackCount <= 0) return@LaunchedEffect
        runCatching {
            val tree = androidx.documentfile.provider.DocumentFile
                .fromTreeUri(container.appContext, Uri.parse(metaTreeUri))
                ?: return@runCatching
            val manifestFile = tree.findFile("manifest.json") ?: return@runCatching
            val text = container.appContext.contentResolver
                .openInputStream(manifestFile.uri)?.use { it.readBytes().toString(Charsets.UTF_8) }
                ?: return@runCatching
            val json = kotlinx.serialization.json.Json { ignoreUnknownKeys = true }
            val manifest = json.decodeFromString(
                io.nocturne.phone.data.catalog.ManifestJson.serializer(), text,
            )
            val residentIds = manifest.resident.map { it.id }
            container.db.trackDao().clearAllResident()
            if (residentIds.isNotEmpty()) {
                // Room IN-clause has SQLite parameter limits. Chunk to be safe.
                residentIds.chunked(500).forEach { batch ->
                    container.db.trackDao().setResidentFor(batch, true)
                }
            }
            android.util.Log.i(
                "AppRoot",
                "manifest reconciled: ${residentIds.size} resident tracks",
            )
        }.onFailure {
            android.util.Log.w("AppRoot", "manifest reconcile failed: ${it.message}")
        }
    }

    // Quick task 260428-8i6: AppRoot-hosted POST_NOTIFICATIONS gate. Tap-to-play
    // call sites submit a deferred action via requestPlay; FirstPlayNotifGate
    // decides whether to show the rationale (first time only) or run immediately.
    var pendingPlayAction by remember { mutableStateOf<(() -> Unit)?>(null) }
    val requestPlay: ((() -> Unit) -> Unit) = { action ->
        pendingPlayAction = action
    }

    Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        when {
            metaTreeUri == LOADING_SENTINEL || musicTreeUri == LOADING_SENTINEL || trackCount == -1 -> {
                Box(Modifier.fillMaxSize().padding(24.dp)) {
                    Text(
                        "nocturne",
                        style = MaterialTheme.typography.headlineLarge,
                        color = MaterialTheme.colorScheme.onBackground,
                    )
                }
            }
            metaTreeUri == null || trackCount == 0 -> {
                FirstRunRoute(container) { result ->
                    trackCount = result.tracksImported
                }
            }
            // Metadata + import done, but the music folder hasn't been picked
            // yet — render the same first-run picker style for the music dir
            // (design pass2026-04-28 setup contract: pick metadata then
            // music in sequence, no Settings detour).
            musicTreeUri == null -> {
                io.nocturne.phone.ui.firstrun.MusicFolderPickerScreen(
                    onFolderPicked = { uri ->
                        // Caller side-effects: persist URI to SyncPrefs. Done
                        // inside the picker via SyncPrefs.setMusicTreeUri.
                    },
                    syncPrefs = container.syncPrefs,
                )
            }
            else -> {
                BrowserRoot(container = container, requestPlay = requestPlay)
            }
        }
        // Quick task 260428-8i6: mount the gate unconditionally so tap-to-play
        // requests from BrowserRoot can overlay an AlertDialog. pendingPlayAction
        // is null in the splash / first-run / picker paths so the gate is a no-op
        // until BrowserRoot fires requestPlay.
        FirstPlayNotifGate(
            syncPrefs = container.syncPrefs,
            pendingAction = pendingPlayAction,
            onConsumed = { pendingPlayAction = null },
        )
    }
}

@Composable
private fun FirstRunRoute(
    container: AppContainer,
    onSucceeded: (io.nocturne.phone.data.catalog.ImportResult) -> Unit,
) {
    val vm: FirstRunViewModel = viewModel(factory = FirstRunVMFactory(container))
    val state by vm.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()

    LaunchedEffect(state) {
        if (state is ImportState.Succeeded) {
            onSucceeded((state as ImportState.Succeeded).result)
        }
    }

    when (state) {
        ImportState.NotStarted -> FirstRunScreen(onFolderPicked = vm::onFolderPicked)
        else -> ImportProgressScreen(
            state = state,
            onRetry = {
                scope.launch {
                    val uri = container.syncPrefs.metaTreeUri.first()?.let(Uri::parse)
                    if (uri != null) {
                        vm.retry(uri)
                    }
                    // If no URI is persisted, retry is a no-op; user can re-launch the
                    // app and the FirstRunScreen will re-prompt.
                }
            },
        )
    }
}

/**
 * Manual ViewModelProvider.Factory — Phase 4 convention; plan 04-06 follows
 * this same pattern for its search VM (no Hilt).
 */
class FirstRunVMFactory(private val container: AppContainer) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T =
        FirstRunViewModel(container) as T
}
