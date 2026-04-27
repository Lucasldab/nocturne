package io.nocturne.phone.ui.search

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.entity.TrackEntity
import kotlinx.collections.immutable.ImmutableList
import kotlinx.collections.immutable.toPersistentList
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

sealed interface SearchResult {
    data object Idle : SearchResult
    data object Loading : SearchResult
    data class Results(val items: ImmutableList<TrackEntity>) : SearchResult

    /** Query produced no rows — distinct from Idle so the UI can say "no matches". */
    data object Empty : SearchResult
    data class Error(val message: String) : SearchResult
}

@OptIn(FlowPreview::class)
class SearchViewModel(
    private val container: AppContainer,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
) : ViewModel() {

    private val _query = MutableStateFlow("")
    val query: StateFlow<String> = _query.asStateFlow()

    private val _results = MutableStateFlow<SearchResult>(SearchResult.Idle)
    val results: StateFlow<SearchResult> = _results.asStateFlow()

    init {
        viewModelScope.launch {
            _query
                .debounce(DEBOUNCE_MS)
                .map { input -> buildFtsQuery(input) }
                .distinctUntilChanged()
                .collectLatest { ftsQuery ->
                    if (ftsQuery == null) {
                        _results.value = SearchResult.Idle
                        return@collectLatest
                    }
                    _results.value = SearchResult.Loading
                    try {
                        val rows = withContext(ioDispatcher) {
                            container.db.searchDao().typeahead(ftsQuery, limit = ROW_LIMIT)
                        }
                        _results.value = if (rows.isEmpty()) {
                            SearchResult.Empty
                        } else {
                            SearchResult.Results(rows.toPersistentList())
                        }
                    } catch (e: Exception) {
                        _results.value = SearchResult.Error(
                            e.message ?: e::class.simpleName.orEmpty(),
                        )
                    }
                }
        }
    }

    fun onQueryChange(s: String) {
        _query.value = s
    }

    fun clear() {
        _query.value = ""
        _results.value = SearchResult.Idle
    }

    private companion object {
        const val DEBOUNCE_MS = 120L
        const val ROW_LIMIT = 50
    }
}

class SearchVMFactory(private val container: AppContainer) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T =
        SearchViewModel(container) as T
}
