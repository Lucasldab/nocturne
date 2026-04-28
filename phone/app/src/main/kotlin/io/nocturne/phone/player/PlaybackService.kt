package io.nocturne.phone.player

import android.app.PendingIntent
import android.content.Intent
import androidx.annotation.OptIn
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import io.nocturne.phone.MainActivity
import kotlinx.coroutines.flow.first
import io.nocturne.phone.NocturneApp
import io.nocturne.phone.data.stats.StatsListener
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.consumeAsFlow
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Media3 MediaSessionService (Phase 5).
 *
 * Owns a single ExoPlayer instance configured with:
 *   - setAudioAttributes(USAGE_MEDIA + CONTENT_TYPE_MUSIC, handleAudioFocus = true)
 *     → satisfies PLAY-06 (audio focus on play, transient duck on incoming call,
 *     stop on permanent loss). Internal AudioFocusRequest is built by Media3.
 *   - handleAudioBecomingNoisy=true — satisfies PLAY-05 (BT/headphone
 *     unplug auto-pause). Media3 registers an internal BroadcastReceiver for
 *     ACTION_AUDIO_BECOMING_NOISY.
 *   - No explicit wake mode — opt-out default in Media3 1.9+ keeps wake lock
 *     active while playing; satisfies PLAY-03 (30-min background survival).
 *   - No gapless override — ExoPlayer's default-on gapless playback satisfies
 *     PLAY-02 by virtue of NOT being disabled.
 *
 * Anti-patterns explicitly avoided (RESEARCH.md):
 *   - No manual FGS promotion — MediaSessionService manages FGS lifecycle.
 *   - No custom BroadcastReceiver for becoming-noisy.
 *   - No custom AudioFocusRequest state machine.
 *
 * Plan 05-06 extensions:
 *   1. ResumptionCallback.onPlaybackResumption now reads QueueRepository via
 *      PlaybackResumption.toMediaItemsWithStartPosition (PLAY-04 reboot resumption).
 *   2. A Player.Listener emits queue snapshots into a Channel that is debounced
 *      500ms before writing to QueueRepository.saveQueue (avoids write storms
 *      on rapid seek / track transitions).
 */
@OptIn(UnstableApi::class)
class PlaybackService : MediaSessionService() {

    private var mediaSession: MediaSession? = null
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    // Channel for debounced queue persistence. Capacity CONFLATED keeps only
    // the latest snapshot (older ones are overwritten before the debounce fires).
    private val queueSaveChannel = Channel<SavedQueue>(Channel.CONFLATED)

    @OptIn(FlowPreview::class)
    override fun onCreate() {
        super.onCreate()

        val container = (application as NocturneApp).container
        val queueRepository = container.queueRepository
        val trackDao = container.db.trackDao()

        val player = ExoPlayer.Builder(this)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(C.USAGE_MEDIA)
                    .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC)
                    .build(),
                /* handleAudioFocus = */ true,
            )
            .setHandleAudioBecomingNoisy(true)
            // Wake mode: opt-out default in Media3 1.9+ (PLAY-03).
            // Gapless: ExoPlayer default-on satisfies PLAY-02.
            .build()

        // Surface playback errors via Toast AND auto-skip on missing file.
        // Without this the player silently transitions to STATE_ENDED on URI
        // failure / format failure / permission failure and the user sees
        // nothing but a play→pause flicker. With auto-skip, queueing a track
        // whose audio file isn't synced to the phone yet (Syncthing-Fork
        // still pulling) jumps to the next loadable item instead of stalling.
        player.addListener(object : Player.Listener {
            override fun onPlayerError(error: androidx.media3.common.PlaybackException) {
                val item = player.currentMediaItem
                val uri = item?.localConfiguration?.uri?.toString() ?: "<no item>"
                val title = item?.mediaMetadata?.title?.toString() ?: "?"
                android.util.Log.e("nocturne", "Player error ${error.errorCodeName}: ${error.message} | URI: $uri", error)

                val isMissingFile =
                    error.errorCode == androidx.media3.common.PlaybackException.ERROR_CODE_IO_FILE_NOT_FOUND ||
                    error.errorCode == androidx.media3.common.PlaybackException.ERROR_CODE_IO_UNSPECIFIED
                val hasNext = player.hasNextMediaItem()

                val toast = if (isMissingFile && hasNext) {
                    "Skipping '$title' — file not on phone yet. Trying next…"
                } else if (isMissingFile) {
                    "'$title' is not on the phone yet. Wait for Syncthing to finish."
                } else {
                    "Player error ${error.errorCodeName}: ${error.message}"
                }
                android.os.Handler(android.os.Looper.getMainLooper()).post {
                    android.widget.Toast.makeText(applicationContext, toast, android.widget.Toast.LENGTH_LONG).show()
                    // Recover playback by jumping to the next item if one exists.
                    if (isMissingFile && hasNext) {
                        try {
                            player.seekToNextMediaItem()
                            player.prepare()
                            player.play()
                        } catch (e: Exception) {
                            android.util.Log.e("nocturne", "auto-skip failed: ${e.message}", e)
                        }
                    }
                }
            }
        })

        mediaSession = MediaSession.Builder(this, player)
            .setSessionActivity(buildSessionActivityIntent())
            .setCallback(ResumptionCallback(queueRepository, trackDao, container.syncPrefs))
            .build()

        // PLAY-09 + Pitfall 7: ExoPlayer parses embedded APIC frames and fires
        // onMediaMetadataChanged with the raw art bytes. We scale them down to
        // 300x300 JPEG q=85 (ArtworkLoader, RESEARCH.md Pitfall 5 mitigation)
        // and re-publish via player.replaceMediaItem so the lock-screen /
        // notification surface receives Binder-IPC-safe bytes.
        player.addListener(object : Player.Listener {
            override fun onMediaMetadataChanged(metadata: MediaMetadata) {
                val raw = metadata.artworkData ?: return
                serviceScope.launch {
                    val scaled = ArtworkLoader.scaleTo300(raw) ?: return@launch
                    // Guard: if the bytes didn't actually change (already scaled),
                    // skip the replaceMediaItem to avoid a redundant listener cycle.
                    if (scaled.contentEquals(raw)) return@launch
                    withContext(Dispatchers.Main) {
                        val current = player.currentMediaItem ?: return@withContext
                        val updated = current.buildUpon()
                            .setMediaMetadata(
                                metadata.buildUpon()
                                    .setArtworkData(
                                        scaled,
                                        MediaMetadata.PICTURE_TYPE_FRONT_COVER,
                                    )
                                    .build(),
                            )
                            .build()
                        player.replaceMediaItem(player.currentMediaItemIndex, updated)
                    }
                }
            }

            // --- PLAY-04: queue persistence listener ---
            // Every salient state change emits a snapshot into queueSaveChannel.
            // The channel is CONFLATED so rapid events only keep the latest.
            // The debounce(500ms) downstream coalesces bursts into a single write.

            override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
                enqueueSnapshot(player)
            }

            override fun onPositionDiscontinuity(
                oldPosition: Player.PositionInfo,
                newPosition: Player.PositionInfo,
                reason: Int,
            ) {
                enqueueSnapshot(player)
            }

            override fun onShuffleModeEnabledChanged(shuffleModeEnabled: Boolean) {
                enqueueSnapshot(player)
            }

            override fun onRepeatModeChanged(repeatMode: Int) {
                enqueueSnapshot(player)
            }

            override fun onIsPlayingChanged(isPlaying: Boolean) {
                // Capture position on pause so resumption starts at the right spot.
                if (!isPlaying) enqueueSnapshot(player)
            }
        })

        // Phase 6 (STATS-01 / STATS-02 / D-24): stats listener for play/skip
        // JSONL emission. Hosted on serviceScope (FGS — Doze-immune) so writes
        // survive when the screen is off. Writes go to the SAF tree URI from
        // SyncPrefs.metaTreeUri; if not provisioned the writer drops events
        // silently. Attached as a SECOND listener — the queue-persistence
        // listener above is intentionally untouched.
        val statsWriter = container.statsWriter
        player.addListener(StatsListener(player, statsWriter, serviceScope))

        // Phase 6 (D-25 / STATS-03): drain any pending pin/like events on service start.
        // The serviceScope (FGS) hosts the drain so it survives Doze; if metaTreeUri
        // is not yet provisioned (first run), drain returns 0 and the rows stay
        // unsynced for the next attempt.
        serviceScope.launch {
            container.pinsWriter.drain()
            container.likesWriter.drain()
        }

        // Consume the debounced channel and write to DataStore on the IO dispatcher.
        serviceScope.launch {
            queueSaveChannel.consumeAsFlow()
                .debounce(500L)
                .collect { snapshot -> queueRepository.saveQueue(snapshot) }
        }
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession? =
        mediaSession

    override fun onTaskRemoved(rootIntent: Intent?) {
        // User swiped the app away from recents.
        // If nothing is playing OR the queue is empty, stop the service so we
        // don't keep an idle FGS notification around. If music is still
        // playing, leave the service running — the user expects audio to
        // continue while they swipe away the catalog activity.
        val player = mediaSession?.player
        if (player == null || !player.playWhenReady || player.mediaItemCount == 0) {
            stopSelf()
        }
    }

    override fun onDestroy() {
        serviceScope.cancel()
        queueSaveChannel.close()
        mediaSession?.run {
            player.release()
            release()
        }
        mediaSession = null
        super.onDestroy()
    }

    private fun buildSessionActivityIntent(): PendingIntent =
        PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )

    /**
     * Snapshot the player's current queue state and send it to the debounced
     * save channel. Called from Player.Listener callbacks (main thread);
     * the channel send is non-blocking (CONFLATED — drops older pending).
     */
    private fun enqueueSnapshot(player: Player) {
        val mediaIds = (0 until player.mediaItemCount).map { player.getMediaItemAt(it).mediaId }
        val snapshot = SavedQueue(
            mediaIds = mediaIds,
            currentIndex = player.currentMediaItemIndex.coerceAtLeast(0),
            currentPositionMs = player.currentPosition.coerceAtLeast(0L),
            shuffleMode = player.shuffleModeEnabled,
            repeatMode = player.repeatMode,
        )
        // trySend on CONFLATED channel: never blocks, never returns false
        // (oldest pending is replaced). Safe to call from main thread.
        queueSaveChannel.trySend(snapshot)
    }

    /**
     * MediaSession callback that reads QueueRepository on playback resumption
     * (e.g. reboot + media-button / BT reconnect — PLAY-04).
     *
     * Replaces the 05-02 stub which returned an empty queue.
     */
    private inner class ResumptionCallback(
        private val queueRepository: QueueRepository,
        private val trackDao: io.nocturne.phone.data.db.dao.TrackDao,
        private val syncPrefs: io.nocturne.phone.data.prefs.SyncPrefs,
    ) : MediaSession.Callback {

        override fun onPlaybackResumption(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
        ): ListenableFuture<MediaSession.MediaItemsWithStartPosition> {
            // Run on the service's IO scope; wrap the result in an immediate future.
            // Media3 calls this on the application's main thread; we hand it off to
            // the service scope so DataStore reads don't block the main thread.
            // Futures.submit is unavailable — use a coroutine future bridge instead.
            val future = com.google.common.util.concurrent.SettableFuture.create<MediaSession.MediaItemsWithStartPosition>()
            serviceScope.launch {
                try {
                    val saved = queueRepository.loadQueue()
                    val musicUri = syncPrefs.musicTreeUri.first()?.let { android.net.Uri.parse(it) }
                    val result = PlaybackResumption.toMediaItemsWithStartPosition(saved, trackDao, musicUri)
                    future.set(result)
                } catch (e: Exception) {
                    // Never propagate exceptions to Media3's callback — return empty queue.
                    future.set(
                        MediaSession.MediaItemsWithStartPosition(
                            emptyList<MediaItem>(),
                            0,
                            0L,
                        ),
                    )
                }
            }
            return future
        }
    }
}
