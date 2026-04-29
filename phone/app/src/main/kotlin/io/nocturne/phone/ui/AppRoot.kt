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
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.compose.LifecycleEventEffect
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

    // v0.4.6: cold-start reconcile flipped isResident for newly-pinned
    // tracks but only on first composition.
    // v0.4.7: 45s mtime poll while composed.
    // v0.4.8: ON_RESUME reconcile. GrapheneOS aggressively kills backgrounded
    // apps; if nocturne dies while waiting for Syncthing to pull a file, the
    // poll never gets to fire. ON_RESUME runs every time the user returns to
    // the app — short backgrounding (process alive) AND process restart both
    // hit it. This is the path that makes pin-as-download "just work" without
    // any manual step.
    val reconcileScope = rememberCoroutineScope()
    var lastReconciledMtime by remember { mutableLongStateOf(0L) }
    var lastReconciledCatalogMtime by remember { mutableLongStateOf(0L) }

    // Reconcile order on every trigger: catalog first (covers schema +
    // newly-added tracks via full re-import), then manifest (cheap residency
    // flip) only if catalog was unchanged. CatalogReconciler restamps
    // residency in the same pass, so when it runs we skip the manifest leg.
    suspend fun runReconcile(uri: String) {
        val catCurrent = io.nocturne.phone.data.catalog.CatalogReconciler
            .catalogMtime(container.appContext, uri)
        if (catCurrent != null && catCurrent != lastReconciledCatalogMtime) {
            val cat = io.nocturne.phone.data.catalog.CatalogReconciler.reconcile(
                container.appContext, uri, container.db,
                container.importer, container.syncPrefs,
                container.queueRepository,
            )
            if (cat != null) {
                lastReconciledCatalogMtime = cat
                // Catalog import already applied manifest residency. Mark the
                // manifest leg as up-to-date so the 45s loop doesn't re-run it.
                io.nocturne.phone.data.catalog.ManifestReconciler
                    .manifestMtime(container.appContext, uri)
                    ?.let { lastReconciledMtime = it }
                trackCount = container.db.trackDao().count()
                return
            }
        }
        val mtime = io.nocturne.phone.data.catalog.ManifestReconciler.reconcile(
            container.appContext, uri, container.db,
        )
        if (mtime != null) lastReconciledMtime = mtime
    }

    // Cold-start path — fires once when metaTreeUri + trackCount become
    // valid. ON_RESUME below fires before SyncPrefs flows have emitted on
    // first composition, so we'd skip the very first reconcile without this.
    LaunchedEffect(metaTreeUri, trackCount) {
        if (metaTreeUri == LOADING_SENTINEL || metaTreeUri == null) return@LaunchedEffect
        if (trackCount <= 0) return@LaunchedEffect
        runReconcile(metaTreeUri!!)
    }

    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) {
        val uri = metaTreeUri
        if (uri == null || uri == LOADING_SENTINEL) return@LifecycleEventEffect
        if (trackCount <= 0) return@LifecycleEventEffect
        reconcileScope.launch { runReconcile(uri) }
    }

    // Long-foreground fallback: if user keeps the app open continuously and
    // a file lands, no ON_RESUME fires. Probe both catalog + manifest mtimes
    // every 45s.
    LaunchedEffect(metaTreeUri, trackCount) {
        if (metaTreeUri == LOADING_SENTINEL || metaTreeUri == null) return@LaunchedEffect
        if (trackCount <= 0) return@LaunchedEffect
        while (true) {
            kotlinx.coroutines.delay(45_000L)
            val uri = metaTreeUri ?: continue
            val catCurrent = io.nocturne.phone.data.catalog.CatalogReconciler
                .catalogMtime(container.appContext, uri)
            val manCurrent = io.nocturne.phone.data.catalog.ManifestReconciler
                .manifestMtime(container.appContext, uri)
            val catalogChanged = catCurrent != null && catCurrent != lastReconciledCatalogMtime
            val manifestChanged = manCurrent != null && manCurrent != lastReconciledMtime
            if (!catalogChanged && !manifestChanged) continue
            runReconcile(uri)
        }
    }

    // AppRoot-hosted POST_NOTIFICATIONS gate. Tap-to-play call sites submit a
    // deferred action via requestPlay; FirstPlayNotifGate decides whether to
    // show the rationale (first time only) or run immediately.
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
            // (setup contract: pick metadata then
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
        // mount the gate unconditionally so tap-to-play
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
