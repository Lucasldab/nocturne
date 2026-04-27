package io.nocturne.phone.player

import androidx.annotation.OptIn
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.source.ShuffleOrder
import io.nocturne.phone.data.db.entity.TrackEntity
import kotlin.random.Random

/**
 * Album-unit shuffle (PLAY-08).
 *
 * Goal: shuffle ALBUMS as units. Within any album, tracks stay in their
 * original (track-number) order; the album order itself is randomised.
 *
 * Implementation: take the flat track list as concatenated album groups,
 * compute per-album start indices, shuffle the album order with a seeded
 * Random, then expand back to a permutation IntArray over [0, totalTracks).
 *
 * RESEARCH.md anti-pattern note: `controller.setShuffleModeEnabled(true)`
 * shuffles individual tracks; we override the underlying ShuffleOrder so
 * that Media3 walks the queue in our album-cohesive order while shuffle
 * mode is on.
 *
 * Pure function: no state, no side effects. JVM-testable via Robolectric
 * (Media3's ShuffleOrder.DefaultShuffleOrder is the only Android dep; no android.* import).
 */
@OptIn(UnstableApi::class)
object AlbumUnitShuffle {

    /**
     * Compute the album-unit shuffled index permutation.
     *
     * @param albums list of albums; each album is a list of tracks (already
     *               in track-number order). Empty albums are skipped silently.
     * @param seed   deterministic random seed.
     * @return       IntArray that is a permutation of [0, totalTracks).
     *               If totalTracks == 0, returns IntArray(0).
     */
    fun buildShuffledIndices(albums: List<List<TrackEntity>>, seed: Long): IntArray {
        val nonEmpty = albums.filter { it.isNotEmpty() }
        val totalTracks = nonEmpty.sumOf { it.size }
        if (totalTracks == 0) return IntArray(0)

        // 1. Compute the start offset for each album in the FLAT (un-shuffled) list.
        val starts = IntArray(nonEmpty.size)
        var offset = 0
        for ((i, album) in nonEmpty.withIndex()) {
            starts[i] = offset
            offset += album.size
        }

        // 2. Shuffle the album order with a deterministic Random.
        val rng = Random(seed)
        val shuffledAlbumOrder = nonEmpty.indices.toMutableList().apply { shuffle(rng) }

        // 3. Expand back to a per-track index permutation.
        val out = IntArray(totalTracks)
        var write = 0
        for (albumIdx in shuffledAlbumOrder) {
            val start = starts[albumIdx]
            val size = nonEmpty[albumIdx].size
            for (k in 0 until size) {
                out[write++] = start + k
            }
        }
        return out
    }

    /**
     * Build a Media3 DefaultShuffleOrder from album-unit shuffled indices.
     *
     * @param albums list of album track groups (same contract as buildShuffledIndices)
     * @param seed   deterministic random seed; defaults to current time for production use
     * @return       ShuffleOrder.DefaultShuffleOrder ready to pass to controller.setShuffleOrder(...)
     */
    fun toDefaultShuffleOrder(
        albums: List<List<TrackEntity>>,
        seed: Long = System.currentTimeMillis(),
    ): ShuffleOrder.DefaultShuffleOrder =
        ShuffleOrder.DefaultShuffleOrder(buildShuffledIndices(albums, seed), seed)
}
