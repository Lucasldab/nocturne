package io.nocturne.phone.player

import io.nocturne.phone.data.db.entity.TrackEntity
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class AlbumUnitShuffleTest {

    private fun t(id: String): TrackEntity = TrackEntity(
        id = id, title = id, artist = listOf("A"), albumArtist = listOf("A"),
        album = "Alb", albumId = "alb", albumArtistId = "aa",
        genre = listOf(), genreId = null,
        trackNumber = 1, discNumber = 1, year = null,
        durationMs = 1000L, sizeBytes = 1L, format = "flac",
        mtimeNs = 0L, dateAdded = "x", path = "/p/$id.flac",
        isResident = true, searchBlob = "x",
    )

    // Flat reference: a1,a2,a3 (0..2)  b1,b2 (3..4)  c1,c2,c3,c4 (5..8)
    private val albumA = listOf(t("a1"), t("a2"), t("a3"))
    private val albumB = listOf(t("b1"), t("b2"))
    private val albumC = listOf(t("c1"), t("c2"), t("c3"), t("c4"))
    private val albums = listOf(albumA, albumB, albumC)

    @Test
    fun preservesAlbumCohesionWithinGroups() {
        val perm = AlbumUnitShuffle.buildShuffledIndices(albums, seed = 42L)
        // Walk the permutation; every group of (size of album) consecutive
        // entries must be contiguous indices ascending by 1.
        val groupRanges = listOf(0..2, 3..4, 5..8) // a, b, c
        var i = 0
        while (i < perm.size) {
            val first = perm[i]
            val group = groupRanges.first { first in it }
            val size = group.last - group.first + 1
            for (k in 0 until size) {
                check(perm[i + k] == first + k) {
                    "album cohesion broken at i=${i + k}: " +
                        "expected ${first + k}, got ${perm[i + k]} (perm=${perm.toList()})"
                }
            }
            i += size
        }
    }

    @Test
    fun deterministicGivenSameSeed() {
        val a = AlbumUnitShuffle.buildShuffledIndices(albums, seed = 12345L)
        val b = AlbumUnitShuffle.buildShuffledIndices(albums, seed = 12345L)
        check(a.contentEquals(b)) { "same seed must give same permutation" }
    }

    @Test
    fun differentSeedsProduceDifferentOrderingsAtLeastOnce() {
        // Conservative test — for >=2 albums there are >=2 distinct orderings,
        // so within 10 random seed pairs we expect a difference at least once.
        val base = AlbumUnitShuffle.buildShuffledIndices(albums, seed = 1L)
        var sawDifferent = false
        for (s in 2L..11L) {
            if (!base.contentEquals(AlbumUnitShuffle.buildShuffledIndices(albums, seed = s))) {
                sawDifferent = true
                break
            }
        }
        check(sawDifferent) { "expected at least one seed in [2..11] to permute differently" }
    }

    @Test
    fun singleAlbumIsIdentity() {
        val perm = AlbumUnitShuffle.buildShuffledIndices(listOf(albumA), seed = 99L)
        check(perm.toList() == listOf(0, 1, 2))
    }

    @Test
    fun allAlbumIndicesPresentExactlyOnce() {
        val perm = AlbumUnitShuffle.buildShuffledIndices(albums, seed = 7L)
        check(perm.size == 9)
        check(perm.toSet() == (0..8).toSet()) {
            "permutation missing or duplicating indices: ${perm.toList()}"
        }
    }

    @Test
    fun emptyInputReturnsEmptyArray() {
        check(AlbumUnitShuffle.buildShuffledIndices(emptyList(), seed = 1L).isEmpty())
        check(AlbumUnitShuffle.buildShuffledIndices(listOf(emptyList()), seed = 1L).isEmpty())
    }

    @Test
    fun toDefaultShuffleOrderProducesNonNullOrder() {
        val order = AlbumUnitShuffle.toDefaultShuffleOrder(albums, seed = 1L)
        check(order.length == 9)
    }
}
