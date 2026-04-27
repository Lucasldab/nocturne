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
import io.nocturne.phone.data.db.entity.PinEntity
import io.nocturne.phone.data.db.entity.TrackEntity
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/**
 * Single ViewModel scoping all browser screens. Pager flows for the four axes
 * are eagerly created (cheap — they're cold until collected) and cached in
 * viewModelScope so tab switches don't lose scroll position.
 *
 * Detail flows (`tracksByAlbum` / `tracksByArtist` / `tracksByGenre`) are
 * created on demand because their query parameter is per-route.
 *
 * Phase 5 (plan 05-06) additions:
 *  - [pinnedIdSet]: StateFlow<Set<String>> derived from PinDao.allPinnedIds().
 *    Efficient: one Flow shared across all call sites; no per-track Flows.
 *  - [pinTrack]: upserts a PinEntity via PinDao.upsert with LWW timestamp.
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

    /**
     * Non-paged album track list for PLAY-07 queue building (plan 05-03).
     * Albums are bounded (<30 tracks typically) — no Paging needed here.
     */
    suspend fun tracksByAlbumList(albumId: String): List<TrackEntity> =
        container.db.trackDao().listByAlbum(albumId)

    // -------------------------------------------------------------------------
    // Phase 5 (plan 05-06) — PLAY-10: pin write from the catalog browser
    // -------------------------------------------------------------------------

    /**
     * Set of currently-pinned track IDs derived from PinDao.allPinnedIds().
     * One StateFlow shared across all TrackRow call sites — much more efficient
     * than subscribing a separate Flow per row in a large list.
     *
     * WhileSubscribed(5_000): keeps the upstream DB query alive for 5s after
     * the last subscriber disappears (e.g. navigating away) to avoid a cold
     * restart on immediate back-navigation.
     */
    val pinnedIdSet = container.db.pinDao().allPinnedIds()
        .map { ids -> ids.toSet() }
        .stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(5_000),
            initialValue = emptySet(),
        )

    /**
     * Upsert a pin record for [trackId] with the current timestamp.
     * Idempotent: Room's OnConflictStrategy.REPLACE on PinDao.upsert updates
     * the pinnedAt timestamp on a duplicate pin (LWW semantics for Phase 7).
     *
     * Unit: always "track" in Phase 5 (album-level pins are a Phase 6+ concern).
     */
    fun pinTrack(trackId: String) {
        viewModelScope.launch {
            container.db.pinDao().upsert(
                PinEntity(
                    id = trackId,
                    unit = "track",
                    pinnedAt = System.currentTimeMillis(),
                    synced = false,
                ),
            )
        }
    }

    // -------------------------------------------------------------------------
    // Phase 6 (STATS-03 / D-17 / D-18) — PinChip toggle dispatcher
    //
    // If no row exists OR the row is currently pinned=false, this pins the
    // (id, unit) by upserting with pinned=true and synced=false.
    // If the row exists and is currently pinned=true, this flips pinned=false
    // (the unpin tombstone) and resets synced=false.
    //
    // After the DB write, kicks off a one-shot drain so the JSONL line emits
    // promptly while the user's screen is on. Drain runs on viewModelScope —
    // if the user navigates away mid-drain, the FGS or next-launch drain
    // (PlaybackService.onCreate per 06-03) picks up any unsynced rows.
    // -------------------------------------------------------------------------

    fun togglePinTrack(trackId: String) = togglePin(id = trackId, unit = "track")

    fun togglePinAlbum(albumId: String) = togglePin(id = albumId, unit = "album")

    private fun togglePin(id: String, unit: String) {
        viewModelScope.launch {
            val dao = container.db.pinDao()
            val now = System.currentTimeMillis()
            val existing = dao.flowAllPinned().first().firstOrNull { it.id == id && it.unit == unit }
            if (existing == null) {
                // Brand-new pin.
                dao.upsert(
                    PinEntity(
                        id = id,
                        unit = unit,
                        pinnedAt = now,
                        synced = false,
                        pinned = true,
                    ),
                )
            } else {
                dao.setPinned(id = id, pinned = !existing.pinned, ts = now)
            }
            container.pinsWriter.drain()
        }
    }
}

class BrowserVMFactory(private val container: AppContainer) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T =
        BrowserViewModel(container) as T
}
