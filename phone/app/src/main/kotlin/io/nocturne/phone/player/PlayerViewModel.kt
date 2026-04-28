package io.nocturne.phone.player

import android.content.ComponentName
import android.content.Context
import androidx.annotation.OptIn
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import com.google.common.util.concurrent.ListenableFuture
import com.google.common.util.concurrent.MoreExecutors
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.entity.TrackEntity
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/**
 * Owns the MediaController <-> PlaybackService binder.
 *
 * Lifecycle (RESEARCH.md Pattern 3 / Pitfall 6):
 *   - connect(): build the controller via MediaController.Builder(...).buildAsync().
 *     Result is a ListenableFuture<MediaController>; we add a directExecutor
 *     listener that publishes the controller into _controller (StateFlow).
 *   - disconnect(): call MediaController.releaseFuture so the binder is
 *     released even if the future never completed.
 *   - onCleared(): always disconnect (second safety net after DisposableEffect).
 *
 * The UI observes `controller` (nullable until bind completes) and disables
 * playback affordances while it's null (matches the LOADING_SENTINEL pattern
 * from AppRoot.kt — null is the "not yet bound" sentinel).
 *
 * T-05-03-03 mitigation: DisposableEffect in BrowserRoot pairs connect/disconnect;
 * onCleared() is the second safety net to prevent binder leaks.
 */
@OptIn(UnstableApi::class)
class PlayerViewModel(
    private val container: AppContainer,
) : ViewModel() {

    private val context: Context = container.appContext
    private var controllerFuture: ListenableFuture<MediaController>? = null

    private val _controller = MutableStateFlow<MediaController?>(null)
    val controller: StateFlow<MediaController?> = _controller.asStateFlow()

    fun connect() {
        if (controllerFuture != null) return  // idempotent
        val token = SessionToken(
            context,
            ComponentName(context, PlaybackService::class.java),
        )
        val future = MediaController.Builder(context, token).buildAsync()
        controllerFuture = future
        future.addListener(
            {
                // get() is non-blocking here because the listener fires once
                // the future is done.
                _controller.value = future.get()
            },
            MoreExecutors.directExecutor(),
        )
    }

    fun disconnect() {
        controllerFuture?.let { MediaController.releaseFuture(it) }
        controllerFuture = null
        _controller.value = null
    }

    /**
     * PLAY-07: tap a track within an album → queue the album from that track
     * forward. Pure delegation to AlbumQueueBuilder; the controller call is
     * the only side effect.
     */
    /**
     * Play a single track without queueing siblings (Tracks / Artist Detail tap).
     *
     * Tracks tab + Artist Detail use Paging 3, so the full peer list isn't
     * available in-memory at tap time. Simplest sensible behaviour: queue
     * exactly the tapped track and start playback. The user can build an
     * album-context queue by going via Albums → Album Detail.
     */
    /** DB lookup for the current MediaController item — used by NowPlaying
     *  to pull format / bitrate / size into the file-info card. */
    suspend fun getTrack(id: String): TrackEntity? =
        container.db.trackDao().byId(id)

    fun playSingleTrack(track: TrackEntity) {
        val c = _controller.value ?: return  // not yet connected
        if (!track.isResident) {
            toastNotDownloaded()
            return
        }
        viewModelScope.launch {
            val musicUri = container.syncPrefs.musicTreeUri.first()?.let { android.net.Uri.parse(it) }
            if (musicUri == null) {
                toastPickMusicFolder()
                return@launch
            }
            val item = track.toMediaItem(musicTreeUri = musicUri)
            c.setMediaItems(listOf(item), 0, /* startPositionMs = */ 0L)
            c.prepare()
            c.play()
        }
    }

    private fun toastNotDownloaded() {
        android.os.Handler(android.os.Looper.getMainLooper()).post {
            android.widget.Toast.makeText(
                context,
                "Track not downloaded yet — wait for sync to complete.",
                android.widget.Toast.LENGTH_SHORT,
            ).show()
        }
    }

    private fun toastPickMusicFolder() {
        android.os.Handler(android.os.Looper.getMainLooper()).post {
            android.widget.Toast.makeText(
                context,
                "Pick the music folder in Settings (5th tab) before playing.",
                android.widget.Toast.LENGTH_LONG,
            ).show()
        }
    }

    /**
     * Play [startTrack] in the context of [allTracks], preserving list order
     * (no album-cohesion reorder). Anything after the start track plays next,
     * making the Tracks tab behave like one continuous playlist when tapped.
     */
    fun playFromList(allTracks: List<TrackEntity>, startTrack: TrackEntity) {
        val c = _controller.value ?: return
        if (!startTrack.isResident) {
            toastNotDownloaded()
            return
        }
        viewModelScope.launch {
            val musicUri = container.syncPrefs.musicTreeUri.first()?.let { android.net.Uri.parse(it) }
            if (musicUri == null) {
                toastPickMusicFolder()
                return@launch
            }
            // Resident-only queue. Non-resident tracks never reach ExoPlayer;
            // "next track" is the next downloaded track in list order.
            val resident = allTracks.filter { it.isResident }
            if (resident.isEmpty()) {
                toastNotDownloaded()
                return@launch
            }
            val startIdx = resident.indexOfFirst { it.id == startTrack.id }
                .takeIf { it >= 0 } ?: 0
            val items = resident.map { it.toMediaItem(musicTreeUri = musicUri) }
            c.setMediaItems(items, startIdx, /* startPositionMs = */ 0L)
            c.prepare()
            c.play()
        }
    }

    /**
     * Append a single track to the end of the current queue without disturbing
     * playback. If the queue is empty, falls through to playSingleTrack so the
     * user gets the expected "tap → it plays" behaviour.
     */
    fun enqueueTrack(track: TrackEntity) {
        val c = _controller.value ?: return
        if (!track.isResident) {
            toastNotDownloaded()
            return
        }
        viewModelScope.launch {
            val musicUri = container.syncPrefs.musicTreeUri.first()?.let { android.net.Uri.parse(it) }
            if (musicUri == null) {
                toastPickMusicFolder()
                return@launch
            }
            val item = track.toMediaItem(musicTreeUri = musicUri)
            if (c.mediaItemCount == 0) {
                c.setMediaItems(listOf(item), 0, /* startPositionMs = */ 0L)
                c.prepare()
                c.play()
            } else {
                c.addMediaItem(item)
                if (!c.isPlaying && c.playbackState == androidx.media3.common.Player.STATE_IDLE) {
                    c.prepare()
                }
            }
        }
    }

    fun playAlbumFromTrack(tracks: List<TrackEntity>, startTrack: TrackEntity) {
        val c = _controller.value ?: return  // not yet connected
        if (!startTrack.isResident) {
            toastNotDownloaded()
            return
        }
        viewModelScope.launch {
            val musicUri = container.syncPrefs.musicTreeUri.first()?.let { android.net.Uri.parse(it) }
            if (musicUri == null) {
                toastPickMusicFolder()
                return@launch
            }
            // Resident-only album queue. Non-resident tracks never reach
            // ExoPlayer, so playback flows through downloaded tracks only.
            val resident = tracks.filter { it.isResident }
            if (resident.isEmpty()) {
                toastNotDownloaded()
                return@launch
            }
            val (items, startIndex) = AlbumQueueBuilder.buildFromTrack(resident, startTrack, musicUri)
            android.util.Log.d(
                "nocturne",
                "playAlbumFromTrack: tracks=${tracks.size} resident=${resident.size} startIdx=$startIndex first.uri=${items.firstOrNull()?.localConfiguration?.uri} musicTree=$musicUri trackPath=${startTrack.path}",
            )
            c.setMediaItems(items, startIndex, /* startPositionMs = */ 0L)
            c.prepare()
            c.play()
        }
    }

    /**
     * PLAY-08: enable album-unit shuffle.
     *
     * Builds the flat MediaItem list in album-cohesive shuffled order (albums
     * shuffled as units; tracks within each album stay in track-number order)
     * and queues it via setMediaItems. Setting shuffleModeEnabled = true makes
     * the UI shuffle indicator reflect state, while the queue itself already
     * encodes the album-unit permutation (avoiding ExoPlayer's per-track
     * random shuffle which is the anti-pattern described in RESEARCH.md).
     *
     * Note: MediaController (IPC proxy) does not expose setShuffleOrder
     * (an ExoPlayer-only API). The album-unit order is therefore embedded
     * directly in the MediaItem sequence — same observable effect.
     *
     * Caller responsibility: pass `albumGroups` already ordered by track-
     * number within each group. The browser screens already do this via
     * pagedByAlbum / listByAlbum.
     *
     * Seed defaults to `System.currentTimeMillis()` so each invocation
     * produces a fresh shuffle. Pass an explicit seed in tests for
     * determinism.
     */
    fun enableAlbumShuffle(
        albumGroups: List<List<TrackEntity>>,
        seed: Long = System.currentTimeMillis(),
    ) {
        val c = _controller.value ?: return
        // Drop non-resident tracks; drop now-empty groups so shuffle doesn't
        // pick a phantom album.
        val residentGroups = albumGroups
            .map { g -> g.filter { it.isResident } }
            .filter { it.isNotEmpty() }
        val flat = residentGroups.flatten()
        if (flat.isEmpty()) return

        // Build album-unit shuffled index permutation, then reorder the flat
        // list according to that permutation.
        val indices = AlbumUnitShuffle.buildShuffledIndices(residentGroups, seed)
        viewModelScope.launch {
            val musicUri = container.syncPrefs.musicTreeUri.first()?.let { android.net.Uri.parse(it) }
            val shuffledItems = indices.map { flat[it].toMediaItem(musicTreeUri = musicUri) }
            c.setMediaItems(shuffledItems, /* startIndex = */ 0, /* startPositionMs = */ 0L)
            c.shuffleModeEnabled = true
            c.prepare()
            c.play()
        }
    }

    /** Disable shuffle (returns to the natural queue order). */
    fun disableShuffle() {
        _controller.value?.shuffleModeEnabled = false
    }

    /**
     * PLAY-01 (repeat): cycle OFF -> ALL -> ONE -> OFF.
     *
     * The RepeatButton from media3-ui-compose-material3 cycles automatically
     * when tapped, so the UI in 05-05 doesn't strictly need this — but
     * exposing it on the VM keeps the cycle deterministic for tests and
     * for future polish features (e.g. sleep timer that disables repeat).
     */
    fun cycleRepeat() {
        val c = _controller.value ?: return
        c.repeatMode = when (c.repeatMode) {
            Player.REPEAT_MODE_OFF -> Player.REPEAT_MODE_ALL
            Player.REPEAT_MODE_ALL -> Player.REPEAT_MODE_ONE
            else /* REPEAT_MODE_ONE */ -> Player.REPEAT_MODE_OFF
        }
    }

    // -------------------------------------------------------------------------
    // Phase 6 (STATS-03 / D-15 / D-16): like state for the currently-playing
    // track. The flow tracks controller.currentMediaItem.mediaId and queries
    // LikeDao.isLiked(id, "track"). NowPlayingScreen collects via
    // collectAsStateWithLifecycle to drive the heart icon.
    // -------------------------------------------------------------------------

    private val currentTrackIdFlow = MutableStateFlow<String?>(null)

    /**
     * Publish the currently-playing track id so [isLikedFlow] can react.
     * Called by NowPlayingScreen from its DisposableEffect on attach and from
     * Player.Listener.onMediaItemTransition.
     */
    fun publishCurrentTrackId(id: String?) {
        currentTrackIdFlow.value = id
    }

    /**
     * Like state of the currently-playing track. `false` when no track is
     * loaded OR no row exists in the likes table for the current id.
     */
    @OptIn(ExperimentalCoroutinesApi::class)
    val isLikedFlow: StateFlow<Boolean> = currentTrackIdFlow
        .flatMapLatest { id ->
            if (id == null) flowOf(false)
            else container.db.likeDao().isLiked(id, "track").map { it == true }
        }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), false)

    /**
     * Toggle the like state of the currently-playing track. Reads the current
     * mediaId off the controller, queries Room for the current `liked` value,
     * and emits the OPPOSITE state (idempotent if double-tapped — the second
     * tap re-flips, the LWW timestamp on the JSONL line resolves on the desktop).
     *
     * No-ops when the controller is null OR no track is currently loaded.
     */
    fun toggleLike() {
        val c = _controller.value ?: return
        val mediaId = c.currentMediaItem?.mediaId ?: return
        viewModelScope.launch {
            val current = container.db.likeDao().isLiked(mediaId, "track").first() == true
            container.likesWriter.record(id = mediaId, unit = "track", liked = !current)
        }
    }

    override fun onCleared() {
        disconnect()
    }
}

/**
 * Manual ViewModelProvider.Factory — Phase 4/5 convention (no Hilt).
 * Mirrors BrowserVMFactory in BrowserViewModel.kt.
 */
class PlayerVMFactory(private val container: AppContainer) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T =
        PlayerViewModel(container) as T
}
