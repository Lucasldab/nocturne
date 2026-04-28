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
                BrowserRoot(container)
            }
        }
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
