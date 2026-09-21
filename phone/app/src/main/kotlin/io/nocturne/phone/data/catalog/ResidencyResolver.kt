package io.nocturne.phone.data.catalog

import io.nocturne.phone.data.db.entity.TrackEntity

/**
 * Decides which *declared* tracks are actually resident on this phone.
 *
 * `manifest.json` states what the daemon has decided should live on the
 * phone. It is not evidence that the audio arrived. The manifest rides the
 * `sync-meta` Syncthing folder (~25 KB, seconds); the FLACs ride `sync-files`
 * (hundreds of MB, minutes to hours). Those two folders are routinely out of
 * step — measured 2026-09-21: sync-meta 100%, sync-files 98.49%, 40 items and
 * 28 MB outstanding.
 *
 * Treating the manifest as ground truth meant declared-but-absent tracks were
 * flagged resident, which put them in shuffle queues (ExoPlayer raised
 * ERROR_CODE_IO_FILE_NOT_FOUND and PlaybackService auto-skipped them) and drew
 * them undimmed in the browser. Residency is now manifest ∩ bytes-on-disk.
 *
 * Pure: no Android types, no I/O. The caller supplies the size probe.
 */
object ResidencyResolver {

    /**
     * Filter [declared] down to the tracks whose audio is really present.
     *
     * @param declared catalog rows for the ids listed in `manifest.resident[]`.
     * @param actualSize on-disk byte count for a track, or null when the file
     *        is absent or unreadable. May throw; a throwing probe counts as
     *        absent — failing open is what queued unplayable tracks before.
     * @return ids to mark resident, in [declared] order.
     */
    fun verify(
        declared: List<TrackEntity>,
        actualSize: (TrackEntity) -> Long?,
    ): List<String> = declared.filter { track ->
        val size = try {
            actualSize(track)
        } catch (_: Throwable) {
            null
        }
        when {
            size == null -> false
            // Short file = Syncthing still writing it. Not playable yet.
            track.sizeBytes > 0L -> size >= track.sizeBytes
            // Catalog row missing sizeBytes — any non-empty file will do.
            else -> size > 0L
        }
    }.map { it.id }

    /**
     * Strip the residency prefix from a catalog path, yielding a path relative
     * to the phone's music tree root.
     *
     * Exactly one leading prefix is removed. Chained `removePrefix` calls would
     * eat both segments of a path like `resident/archive/x.flac` and stat the
     * wrong file.
     */
    fun relPath(trackPath: String): String = when {
        trackPath.startsWith(RESIDENT_PREFIX) -> trackPath.removePrefix(RESIDENT_PREFIX)
        trackPath.startsWith(ARCHIVE_PREFIX) -> trackPath.removePrefix(ARCHIVE_PREFIX)
        else -> trackPath
    }

    private const val RESIDENT_PREFIX = "resident/"
    private const val ARCHIVE_PREFIX = "archive/"
}
