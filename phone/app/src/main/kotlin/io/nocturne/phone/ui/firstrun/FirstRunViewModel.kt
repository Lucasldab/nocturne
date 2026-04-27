package io.nocturne.phone.ui.firstrun

import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.catalog.CatalogSource
import io.nocturne.phone.data.catalog.SafCatalogSource
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.Instant

/**
 * Drives the first-run / re-import flow. Plain ViewModel — no Hilt. Tests
 * use [importFromForTest] to bypass SAF and feed an in-memory CatalogSource.
 */
class FirstRunViewModel(
    private val container: AppContainer,
) : ViewModel() {
    private val _state = MutableStateFlow<ImportState>(ImportState.NotStarted)
    val state: StateFlow<ImportState> = _state.asStateFlow()

    fun onPickerOpening() {
        _state.value = ImportState.PickingFolder
    }

    fun onFolderPicked(treeUri: Uri) {
        viewModelScope.launch {
            try {
                container.syncPrefs.setMetaTreeUri(treeUri.toString())
            } catch (e: Exception) {
                _state.value = ImportState.Failed("setMetaTreeUri: ${e.message ?: e::class.simpleName}")
                return@launch
            }
            runImport(SafCatalogSource(container.appContext, treeUri))
        }
    }

    fun retry(treeUri: Uri) {
        _state.value = ImportState.NotStarted
        viewModelScope.launch {
            runImport(SafCatalogSource(container.appContext, treeUri))
        }
    }

    /** Test seam — bypasses SAF. */
    internal suspend fun importFromForTest(source: CatalogSource) {
        runImport(source)
    }

    private suspend fun runImport(source: CatalogSource) {
        withContext(Dispatchers.IO) {
            try {
                val cat = source.openCatalog()
                val man = source.openManifest()
                val result = container.importer.importAll(cat, man) { stage, done, total ->
                    _state.value = ImportState.Importing(stage, done, total)
                }
                container.syncPrefs.setLastImportAt(Instant.now().toString())
                _state.value = ImportState.Succeeded(result)
            } catch (e: Exception) {
                _state.value = ImportState.Failed(
                    buildString {
                        append(e::class.simpleName ?: "Error")
                        append(": ")
                        append(e.message ?: "(no message)")
                    },
                )
            }
        }
    }
}
