package io.nocturne.phone.player

import android.app.PendingIntent
import android.content.Intent
import androidx.annotation.OptIn
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import io.nocturne.phone.MainActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel

/**
 * Media3 MediaSessionService skeleton (Phase 5 plan 05-02).
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
 *   - No direct setMediaItems() in onCreate — the service is a passive player;
 *     the UI sends commands via MediaController (plan 05-03).
 *
 * Plan 05-06 extends this file in two places:
 *   1. Replace ResumptionCallback's onPlaybackResumption with a real DataStore
 *      read (queue restoration after reboot — PLAY-04).
 *   2. Attach a Player.Listener that persists the queue on transitions.
 */
@OptIn(UnstableApi::class)
class PlaybackService : MediaSessionService() {

    private var mediaSession: MediaSession? = null
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
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

        mediaSession = MediaSession.Builder(this, player)
            .setSessionActivity(buildSessionActivityIntent())
            .setCallback(ResumptionCallback())
            .build()
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
     * Minimal Callback for plan 05-02. Plan 05-06 overrides
     * onPlaybackResumption with a real DataStore-backed implementation.
     *
     * The non-throwing empty-queue future here is REQUIRED — RESEARCH.md
     * Pitfall 3: the default Callback implementation throws
     * UnsupportedOperationException, which crashes the service on first
     * boot resumption.
     */
    private inner class ResumptionCallback : MediaSession.Callback {
        override fun onPlaybackResumption(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
        ): ListenableFuture<MediaSession.MediaItemsWithStartPosition> =
            Futures.immediateFuture(
                MediaSession.MediaItemsWithStartPosition(
                    /* mediaItems = */ emptyList<MediaItem>(),
                    /* startIndex = */ 0,
                    /* startPositionMs = */ 0L,
                ),
            )
    }
}
