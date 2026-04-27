package io.nocturne.phone.ui.browser

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.paging.Pager
import androidx.paging.PagingConfig
import androidx.paging.PagingData
import androidx.paging.cachedIn
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.entity.AlbumEntity
import io.nocturne.phone.data.db.entity.ArtistEntity
import io.nocturne.phone.data.db.entity.GenreEntity
import io.nocturne.phone.data.db.entity.TrackEntity
import kotlinx.coroutines.flow.Flow

/**
 * Single ViewModel scoping all browser screens. Pager flows for the four axes
 * are eagerly created (cheap — they're cold until collected) and cached in
 * viewModelScope so tab switches don't lose scroll position.
 *
 * Detail flows (`tracksByAlbum` / `tracksByArtist` / `tracksByGenre`) are
 * created on demand because their query parameter is per-route.
 */
class BrowserViewModel(private val container: AppContainer) : ViewModel() {

    private val cfg = PagingConfig(
        pageSize = 50,
        enablePlaceholders = false,
        prefetchDistance = 25,
        initialLoadSize = 100,
    )

    val albums: Flow<PagingData<AlbumEntity>> =
        Pager(cfg) { container.db.albumDao().pagedAll() }.flow.cachedIn(viewModelScope)

    val artists: Flow<PagingData<ArtistEntity>> =
        Pager(cfg) { container.db.artistDao().pagedAll() }.flow.cachedIn(viewModelScope)

    val tracks: Flow<PagingData<TrackEntity>> =
        Pager(cfg) { container.db.trackDao().pagedAll() }.flow.cachedIn(viewModelScope)

    val genres: Flow<PagingData<GenreEntity>> =
        Pager(cfg) { container.db.genreDao().pagedAll() }.flow.cachedIn(viewModelScope)

    fun tracksByAlbum(albumId: String): Flow<PagingData<TrackEntity>> =
        Pager(cfg) { container.db.trackDao().pagedByAlbum(albumId) }
            .flow.cachedIn(viewModelScope)

    fun tracksByArtist(artistId: String): Flow<PagingData<TrackEntity>> =
        Pager(cfg) { container.db.trackDao().pagedByArtist(artistId) }
            .flow.cachedIn(viewModelScope)

    fun tracksByGenre(genreId: String): Flow<PagingData<TrackEntity>> =
        Pager(cfg) { container.db.trackDao().pagedByGenre(genreId) }
            .flow.cachedIn(viewModelScope)

    fun albumsByArtist(artistId: String): Flow<List<AlbumEntity>> =
        container.db.albumDao().albumsByArtist(artistId)

    suspend fun albumById(id: String): AlbumEntity? = container.db.albumDao().byId(id)
    suspend fun artistById(id: String): ArtistEntity? = container.db.artistDao().byId(id)
}

class BrowserVMFactory(private val container: AppContainer) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T =
        BrowserViewModel(container) as T
}
