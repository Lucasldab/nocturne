package io.nocturne.phone.ui.system

import io.nocturne.phone.data.catalog.ManifestJson
import io.nocturne.phone.data.db.entity.TrackEntity

/**
 * Rotation/buckets aggregator (pure JVM).
 *
 * Joins the daemon-published `manifest.json` (cap_bytes / used_bytes / per-id
 * bucket assignments) with the local Room track index (sizeBytes per id) to
 * produce a per-bucket roll-up the System / Rotation screen can render
 * without touching the disk again.
 *
 * Canonical bucket order is locked to schema/manifest.schema.json:
 *   recent_adds, top_played, recent_plays, loved, exploration, manual_pins.
 *
 * Forward compatibility: any unknown bucket id emitted by a future daemon is
 * appended after the canonical block in encounter order, with the raw id used
 * as the human label. Tracks that appear in manifest.resident but not in the
 * track map (race with import / DB pruning) still count toward `count` but
 * contribute zero bytes — total bytes are NOT used for the cap-fraction
 * display; that comes from the manifest's authoritative `used_bytes`.
 */
data class BucketRow(
    val id: String,
    val label: String,
    val count: Int,
    val bytes: Long,
)

data class RotationView(
    val buckets: List<BucketRow>,
    val totalUsedBytes: Long,
    val capBytes: Long,
    val generatedAt: String?,
) {
    companion object {
        val empty = RotationView(emptyList(), 0L, 0L, null)
    }
}

object RotationAggregator {
    private val CANONICAL_ORDER = listOf(
        "recent_adds",
        "top_played",
        "recent_plays",
        "loved",
        "exploration",
        "manual_pins",
    )
    private val BUCKET_LABELS = mapOf(
        "recent_adds" to "recent adds",
        "top_played" to "top played",
        "recent_plays" to "recent plays",
        "loved" to "loved",
        "exploration" to "exploration",
        "manual_pins" to "manual pins",
    )

    private class Acc(var count: Int = 0, var bytes: Long = 0L)

    fun aggregate(
        manifest: ManifestJson?,
        trackById: Map<String, TrackEntity>,
    ): RotationView {
        if (manifest == null) return RotationView.empty
        // LinkedHashMap preserves first-encounter order — used to keep
        // unknown buckets stable across calls.
        val accs = LinkedHashMap<String, Acc>()
        for (entry in manifest.resident) {
            val sz = trackById[entry.id]?.sizeBytes ?: 0L
            for (bucketId in entry.buckets) {
                val a = accs.getOrPut(bucketId) { Acc() }
                a.count += 1
                a.bytes += sz
            }
        }
        val canonicalRows = CANONICAL_ORDER
            .filter { accs[it]?.count != null && accs[it]!!.count > 0 }
            .map { id ->
                val a = accs.getValue(id)
                BucketRow(
                    id = id,
                    label = BUCKET_LABELS[id] ?: id,
                    count = a.count,
                    bytes = a.bytes,
                )
            }
        val unknownRows = accs.entries
            .filter { it.key !in CANONICAL_ORDER }
            .map { (id, a) ->
                BucketRow(id = id, label = id, count = a.count, bytes = a.bytes)
            }
        return RotationView(
            buckets = canonicalRows + unknownRows,
            totalUsedBytes = manifest.usedBytes,
            capBytes = manifest.capBytes,
            generatedAt = manifest.generatedAt,
        )
    }
}
