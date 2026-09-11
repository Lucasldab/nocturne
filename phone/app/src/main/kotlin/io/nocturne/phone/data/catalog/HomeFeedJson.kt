package io.nocturne.phone.data.catalog

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * `discovery.json` — this week's Weekly Discovery picks, published by
 * `nocturne-recommend discovery` on the desktop.
 *
 * Deliberately separate from manifest.json. The resolver only makes
 * weekly_discovery tracks RESIDENT once there are 10+ phone plays (RESOLVE-06
 * cold start), which hides them exactly when a freshly rebuilt library needs
 * them most. A pick is a suggestion, not a file — publishing it independently
 * lets this surface show picks and fetch on tap regardless of residency.
 */
@Serializable
data class DiscoveryJson(
    val v: Int = 1,
    @SerialName("generated_at") val generatedAt: Long = 0L,
    val picks: List<DiscoveryPick> = emptyList(),
)

@Serializable
data class DiscoveryPick(
    val id: String = "",
    val artist: String = "",
    val title: String = "",
    val album: String = "",
    /** never_played | aged_out | adjacent_to_loved | random */
    val reason: String = "",
    val week: String = "",
    /** Pre-built because the phone has no INTERNET permission (CROSS-01). */
    val query: String = "",
)

/**
 * `recommendations.json` — artists NOT in the library, ranked from
 * ListenBrainz similarity weighted by local plays/pins/likes.
 */
@Serializable
data class RecommendationsJson(
    val v: Int = 1,
    @SerialName("generated_at") val generatedAt: Long = 0L,
    val recommendations: List<Recommendation> = emptyList(),
)

@Serializable
data class Recommendation(
    @SerialName("artist_mbid") val artistMbid: String = "",
    val name: String = "",
    val comment: String = "",
    val score: Double = 0.0,
    /** Owned artists that led here — "because you listen to X". */
    val because: List<String> = emptyList(),
    @SerialName("top_track") val topTrack: String? = null,
    val sharers: Int = 0,
    /** Resolved on the desktop; absent until `nocturne-recommend toptracks` runs. */
    val query: String? = null,
)
