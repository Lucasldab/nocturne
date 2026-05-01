package io.nocturne.phone.ui.browser

import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
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
import io.nocturne.phone.ui.system.StatsAggregator
import io.nocturne.phone.ui.system.StatsView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader

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

    /**
     * Non-paged full library list — used to queue the entire Tracks tab as
     * one continuous playlist when the user taps a row.
     */
    suspend fun tracksAllList(): List<TrackEntity> =
        container.db.trackDao().listAll()

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
     * Map of pinned track id -> pinnedAt epoch-ms. Powers the
     * RecentlyDownloaded pin overlay (quick task 260430-vtb Bug 2). Filtered
     * to unit="track" rows with pinned=true so unpinned tombstones don't leak
     * into the ordering.
     */
    val pinnedAtById: StateFlow<Map<String, Long>> =
        container.db.pinDao().flowAllPinned()
            .map { rows ->
                rows.asSequence()
                    .filter { it.pinned && it.unit == "track" }
                    .associate { it.id to it.pinnedAt }
            }
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.WhileSubscribed(5_000),
                initialValue = emptyMap(),
            )

    // -------------------------------------------------------------------------
    // Quick task 260430-vtb (Bug 1 + Bug 3): non-paged alphabetical browse axes.
    //
    // Pager+scrollToItem(target) silently swallows seeks past the loaded paging
    // window, so the LetterScrollRail couldn't snap to letters far from the
    // current scroll position. Library is bounded (~1899 tracks); a single
    // sortedWith over Dispatchers.IO is well under the frame budget. Initial
    // emit is `emptyList()` so screens render immediately on cold start; the
    // real list arrives on the first IO tick (also addresses Bug 3 — Pager
    // warm-up was the cold-start TracksScreen latency cause).
    // -------------------------------------------------------------------------

    val tracksAlphabetical: StateFlow<List<TrackEntity>> =
        flow {
            emit(withContext(Dispatchers.IO) { container.db.trackDao().listAll() })
        }
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.WhileSubscribed(5_000),
                initialValue = emptyList(),
            )

    val albumsAll: StateFlow<List<AlbumEntity>> =
        flow {
            emit(withContext(Dispatchers.IO) { container.db.albumDao().listAll() })
        }
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.WhileSubscribed(5_000),
                initialValue = emptyList(),
            )

    val artistsAll: StateFlow<List<ArtistEntity>> =
        flow {
            emit(withContext(Dispatchers.IO) { container.db.artistDao().listAll() })
        }
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.WhileSubscribed(5_000),
                initialValue = emptyList(),
            )

    // -------------------------------------------------------------------------
    // Pinned-track download progress (SAF-stat probe; no network — CROSS-01).
    //
    // Polls every 5s while the StateFlow has subscribers. Each tick re-derives
    // the set of pinned-but-not-resident track ids from pinDao + tracks table
    // and asks SyncProgressRepository for SAF stats on the Syncthing temp
    // files. Empty pulling set → no probing, but the StateFlow still ticks
    // (cheap; the probe short-circuits on empty input). When all subscribers
    // disappear, WhileSubscribed(5s) tears down the timer.
    //
    // Aggregate fields exposed alongside the per-id map so SyncScreen can show
    // "syncing N · 67%" without re-deriving in compose.
    // -------------------------------------------------------------------------

    data class SyncProgressState(
        /** Map of trackId → progress (0..1, or null when unknown). */
        val perTrack: Map<String, Float?> = emptyMap(),
        /** Number of tracks currently pinned-but-not-resident. */
        val pendingCount: Int = 0,
        /** Aggregate completion fraction across pending tracks (0..1, null when empty). */
        val aggregate: Float? = null,
    )

    val pinnedDownloadProgress: kotlinx.coroutines.flow.StateFlow<SyncProgressState> =
        flow {
            while (true) {
                val state = computeProgressOnce()
                emit(state)
                delay(5_000L)
            }
        }
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.WhileSubscribed(5_000),
                initialValue = SyncProgressState(),
            )

    private suspend fun computeProgressOnce(): SyncProgressState {
        val pinnedRows = container.db.pinDao().flowAllPinned().first()
            .filter { it.pinned && it.unit == "track" }
        if (pinnedRows.isEmpty()) return SyncProgressState()
        val tracks = pinnedRows.map { it.id }.chunked(500)
            .flatMap { container.db.trackDao().byIds(it) }
        val pending = tracks.filter { !it.isResident }
        if (pending.isEmpty()) return SyncProgressState()
        val perTrack = container.syncProgress.probe(pending.map { it.id })
        // Aggregate: weight progress by expected size so a half-done 50MB track
        // counts more than a half-done 5MB track.
        var totalBytes = 0L
        var doneBytes = 0L
        for (t in pending) {
            val p = perTrack[t.id] ?: continue
            totalBytes += t.sizeBytes
            doneBytes += (p * t.sizeBytes).toLong()
        }
        val agg = if (totalBytes > 0L) doneBytes.toFloat() / totalBytes.toFloat() else null
        return SyncProgressState(
            perTrack = perTrack,
            pendingCount = pending.size,
            aggregate = agg,
        )
    }

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

    // -------------------------------------------------------------------------
    // Long-press track/album actions: "unsync" (unpin and unload, reversible)
    // and "delete" (didn't like it, destructive). Both fire-and-forget JSONL —
    // daemon-side actions.c handles the actual demote / file removal /
    // blacklisting. UI clears local pin row optimistically on unsync so the
    // pin chip updates immediately; deletion side relies on next catalog
    // reconcile to drop the row.
    // -------------------------------------------------------------------------

    fun unsyncTrack(trackId: String) {
        viewModelScope.launch {
            // Optimistic: clear local pin row so the chip updates without
            // waiting for round-trip via Syncthing.
            container.db.pinDao().setPinned(id = trackId, pinned = false, ts = System.currentTimeMillis())
            container.actionsWriter.emitUnsyncTrack(trackId)
        }
    }

    fun unsyncAlbum(albumId: String) {
        viewModelScope.launch {
            container.db.pinDao().setPinned(id = albumId, pinned = false, ts = System.currentTimeMillis())
            container.actionsWriter.emitUnsyncAlbum(albumId)
        }
    }

    fun deleteTrack(trackId: String) {
        viewModelScope.launch {
            container.actionsWriter.emitDeleteTrack(trackId)
        }
    }

    fun deleteAlbum(albumId: String) {
        viewModelScope.launch {
            container.actionsWriter.emitDeleteAlbum(albumId)
        }
    }

    // -------------------------------------------------------------------------
    // Quick task 260430-s5u — Tracks-tab sort modes.
    //
    // [trackSortMode] mirrors SyncPrefs so process death + relaunch restores
    // the user's last selection. [tracksSorted] is the non-paged sorted list
    // for the three non-Alphabetical modes (Alphabetical keeps the existing
    // paged path so the letter rail's row-index math stays valid).
    //
    // Stats JSONL is read lazily and cached in [_statsView]: only modes that
    // need play counts / lastTs (MostListened + RecentlyListened) trigger
    // the SAF read, and a second sort flip between those two reuses the
    // cache. RecentlyDownloaded reads dateAdded directly from TrackEntity
    // and bypasses the cache entirely.
    // -------------------------------------------------------------------------

    val trackSortMode: StateFlow<TrackSortMode> =
        container.syncPrefs.trackSortMode
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.WhileSubscribed(5_000),
                initialValue = TrackSortMode.DEFAULT,
            )

    fun setTrackSortMode(mode: TrackSortMode) {
        viewModelScope.launch { container.syncPrefs.setTrackSortMode(mode) }
    }

    private val _statsView = MutableStateFlow<StatsView?>(null)

    /**
     * Reads the local stats JSONL via the same SAF pattern SystemViewModel
     * uses (kept private there — duplicated here verbatim rather than
     * widening visibility, per scope_decisions point 4).
     *
     * Returns [StatsView.empty] on any first-run failure (no SAF tree yet,
     * stats dir missing, file missing) — the sort flow renders a
     * dateAdded-and-title ordering in that case rather than throwing.
     */
    private suspend fun loadStatsView(): StatsView = withContext(Dispatchers.IO) {
        val deviceId = container.syncPrefs.deviceId()
        val uriStr = container.syncPrefs.metaTreeUri.first() ?: return@withContext StatsView.empty()
        val tree = DocumentFile.fromTreeUri(container.appContext, uriStr.toUri())
            ?: return@withContext StatsView.empty()
        val statsDir = tree.findFile("stats") ?: return@withContext StatsView.empty()
        val f = statsDir.findFile("phone-$deviceId.jsonl") ?: return@withContext StatsView.empty()
        val lines = container.appContext.contentResolver.openInputStream(f.uri)?.use { ins ->
            BufferedReader(InputStreamReader(ins, Charsets.UTF_8)).use { it.readLines() }
        } ?: return@withContext StatsView.empty()
        StatsAggregator.aggregate(
            lines = lines.iterator(),
            nowMs = System.currentTimeMillis(),
            allTime = true,
        )
    }

    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    val tracksSorted: StateFlow<List<TrackEntity>> =
        trackSortMode
            .flatMapLatest { mode ->
                flow {
                    if (mode == TrackSortMode.Alphabetical) {
                        // Alphabetical uses the paged path; this StateFlow
                        // is irrelevant. Emit an empty list so subscribers
                        // that don't gate on mode see a coherent value.
                        emit(emptyList())
                        return@flow
                    }
                    val all = withContext(Dispatchers.IO) {
                        container.db.trackDao().listAll()
                    }
                    val needsStats = mode == TrackSortMode.MostListened ||
                        mode == TrackSortMode.RecentlyListened
                    val stats = if (needsStats) {
                        _statsView.value ?: loadStatsView().also { _statsView.value = it }
                    } else {
                        StatsView.empty()
                    }
                    // Quick task 260430-vtb Bug 2: feed pinnedAt into the
                    // RecentlyDownloaded ordering. StateFlow read is
                    // non-suspending so this doesn't introduce a join point.
                    val pinnedAtMap = if (mode == TrackSortMode.RecentlyDownloaded) {
                        pinnedAtById.value
                    } else {
                        emptyMap()
                    }
                    emit(
                        TrackSorter.sort(
                            tracks = all,
                            mode = mode,
                            perTrackPlays = stats.perTrackPlays,
                            perTrackLastTs = stats.perTrackLastTs,
                            perTrackPinnedAt = pinnedAtMap,
                        ),
                    )
                }
            }
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.WhileSubscribed(5_000),
                initialValue = emptyList(),
            )
}

class BrowserVMFactory(private val container: AppContainer) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T =
        BrowserViewModel(container) as T
}
