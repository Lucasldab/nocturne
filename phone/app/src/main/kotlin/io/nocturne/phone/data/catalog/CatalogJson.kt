package io.nocturne.phone.data.catalog

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * `catalog.json` shape — mirrors the Phase-2 daemon emitter's locked schema
 * (see `.planning/phases/02-daemon-foundation/02-06-PLAN.md`).
 *
 * Decoder uses `Json { ignoreUnknownKeys = true; isLenient = false }` so the
 * daemon adding fields later does not break the phone (forward compat per
 * ARCHITECTURE.md schema-versioning rules).
 */
@Serializable
data class CatalogJson(
    val v: Int,
    @SerialName("generated_at") val generatedAt: String,
    val tracks: List<CatalogTrackJson>,
)

/**
 * Real-world catalog observation: the daemon emits `title: null` for files
 * whose tag walker found no title tag (e.g. unsplit single-track FLACs).
 * `track`/`disc`/`year` are likewise emitted as `0` rather than `null` when
 * the underlying tag is absent. We accept both shapes.
 */
@Serializable
data class CatalogTrackJson(
    val id: String,                       // 64-char lowercase hex sha256
    val path: String,                     // catalog-relative; "resident/..." or "archive/..."
    val title: String? = null,
    val artist: List<String> = emptyList(),
    val album: String? = null,
    @SerialName("album_artist") val albumArtist: List<String> = emptyList(),
    val track: Int? = null,
    val disc: Int? = null,
    val year: Int? = null,
    val genre: List<String> = emptyList(),
    @SerialName("duration_ms") val durationMs: Long? = null,
    @SerialName("size_bytes") val sizeBytes: Long,
    val format: String,
    @SerialName("mtime_ns") val mtimeNs: Long,
    @SerialName("date_added") val dateAdded: String,
)
