package io.nocturne.phone.player

import android.content.ComponentName
import android.content.Context
import androidx.annotation.OptIn
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import com.google.common.util.concurrent.ListenableFuture
import com.google.common.util.concurrent.MoreExecutors
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.db.entity.TrackEntity
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

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
    fun playAlbumFromTrack(tracks: List<TrackEntity>, startTrack: TrackEntity) {
        val c = _controller.value ?: return  // not yet connected
        val (items, startIndex) = AlbumQueueBuilder.buildFromTrack(tracks, startTrack)
        c.setMediaItems(items, startIndex, /* startPositionMs = */ 0L)
        c.prepare()
        c.play()
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
