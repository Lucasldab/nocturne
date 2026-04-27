package io.nocturne.phone.data.catalog

import androidx.room.withTransaction
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.db.entity.AlbumEntity
import io.nocturne.phone.data.db.entity.ArtistEntity
import io.nocturne.phone.data.db.entity.GenreEntity
import io.nocturne.phone.data.db.entity.TrackEntity
import kotlinx.serialization.json.Json
import java.io.InputStream
import java.security.MessageDigest

private val json = Json { ignoreUnknownKeys = true; isLenient = false }

private fun sha256Hex(s: String): String =
    MessageDigest.getInstance("SHA-256")
        .digest(s.toByteArray())
        .joinToString("") { "%02x".format(it) }

data class ImportResult(
    val tracksImported: Int,
    val albumsImported: Int,
    val artistsImported: Int,
    val genresImported: Int,
    /** Number of resident-marked tracks; -1 if no manifest was provided. */
    val residentMarked: Int,
    val durationMs: Long,
)

enum class Stage {
    PARSING_CATALOG,
    INSERTING_TRACKS,
    DERIVING_GROUPS,
    INSERTING_GROUPS,
    APPLYING_MANIFEST,
    DONE,
}

/**
 * Catalog importer: parse → derive Album/Artist/Genre rows → batched
 * UPSERT into Room → apply manifest residency.
 *
 * Strategy is fresh-rebuild per import: deleteAll() the four tables under a
 * single transaction, then insert in batches of 500. Phase 4 catalogs top
 * out at ~15k rows / ~1MB; full rebuild is cheap and idempotent. Delta
 * updates can be revisited if catalog size grows past the point where
 * full-rebuild noticeably stalls the UI (probably never, given the
 * single-user scale).
 *
 * The accent-fold algorithm in [accentFold] is applied to the search blob
 * exactly once during import — there is no query-time normalisation
 * (Pitfall 5). Plan 04-06's search overlay must call the SAME [accentFold]
 * to fold its query before MATCH.
 */
class CatalogImporter(private val db: NocturneDatabase) {

    suspend fun importAll(
        catalogIn: InputStream,
        manifestIn: InputStream? = null,
        progress: (Stage, Int, Int) -> Unit = { _, _, _ -> },
    ): ImportResult {
        val t0 = System.nanoTime()

        progress(Stage.PARSING_CATALOG, 0, 0)
        val catalog: CatalogJson = catalogIn.bufferedReader().use { reader ->
            json.decodeFromString(CatalogJson.serializer(), reader.readText())
        }
        require(catalog.v == 1) {
            "Unsupported catalog schema v=${catalog.v}; this app supports v=1"
        }

        // Manifest is OPTIONAL — pre-load resident set so we can stamp tracks during insert.
        val residentIds: Set<String> = if (manifestIn != null) {
            val m: ManifestJson = manifestIn.bufferedReader().use {
                json.decodeFromString(ManifestJson.serializer(), it.readText())
            }
            require(m.v == 1) {
                "Unsupported manifest schema v=${m.v}; this app supports v=1"
            }
            m.resident.map { it.id }.toSet()
        } else {
            emptySet()
        }

        val tracksTotal = catalog.tracks.size
        progress(Stage.INSERTING_TRACKS, 0, tracksTotal)

        val albums = HashMap<String, AlbumAccum>()
        val artists = HashMap<String, ArtistAccum>()
        val genres = HashMap<String, GenreAccum>()

        db.withTransaction {
            // Fresh rebuild — Phase 4 strategy. Idempotence: rerunning the
            // same catalog leaves the DB in the same final state.
            db.trackDao().deleteAll()
            db.albumDao().deleteAll()
            db.artistDao().deleteAll()
            db.genreDao().deleteAll()

            val batch = ArrayList<TrackEntity>(BATCH_SIZE)
            var inserted = 0
            for (t in catalog.tracks) {
                val albumArtistGroup = (
                    t.albumArtist.firstOrNull { it.isNotBlank() }
                        ?: t.artist.firstOrNull { it.isNotBlank() }
                        ?: "Unknown Artist"
                    ).trim()
                val albumTitle = (t.album ?: "Unknown Album").trim()
                val resolvedTitle = (t.title ?: "Unknown Title").trim()
                val albumId = sha256Hex("$albumArtistGroup ${albumTitle.lowercase()}")
                val artistId = sha256Hex(albumArtistGroup.lowercase())
                val firstGenre = t.genre.firstOrNull { it.isNotBlank() }?.trim()
                val genreId = firstGenre?.let { sha256Hex(it.lowercase()) }
                val isResident = residentIds.contains(t.id)

                val blob = accentFold(
                    buildString {
                        append(resolvedTitle); append(' ')
                        t.artist.forEach { append(it); append(' ') }
                        append(albumTitle); append(' ')
                        t.albumArtist.forEach { append(it); append(' ') }
                        t.genre.forEach { append(it); append(' ') }
                    },
                )

                val row = TrackEntity(
                    id = t.id,
                    title = resolvedTitle,
                    artist = t.artist,
                    albumArtist = t.albumArtist,
                    album = albumTitle,
                    albumId = albumId,
                    albumArtistId = artistId,
                    genre = t.genre,
                    genreId = genreId,
                    trackNumber = t.track,
                    discNumber = t.disc,
                    year = t.year,
                    durationMs = t.durationMs,
                    sizeBytes = t.sizeBytes,
                    format = t.format,
                    mtimeNs = t.mtimeNs,
                    dateAdded = t.dateAdded,
                    path = t.path,
                    isResident = isResident,
                    searchBlob = blob,
                )
                batch.add(row)

                albums.getOrPut(albumId) {
                    AlbumAccum(
                        id = albumId,
                        title = albumTitle,
                        albumArtist = t.albumArtist.ifEmpty { listOf(albumArtistGroup) },
                        year = t.year,
                    )
                }.also {
                    it.trackCount += 1
                    it.totalSizeBytes += t.sizeBytes
                    if (isResident) it.hasResident = true
                    if (it.year == null && t.year != null) it.year = t.year
                }
                artists.getOrPut(artistId) {
                    ArtistAccum(id = artistId, name = albumArtistGroup)
                }.also {
                    it.trackCount += 1
                    it.albumIds.add(albumId)
                }
                if (firstGenre != null && genreId != null) {
                    genres.getOrPut(genreId) {
                        GenreAccum(id = genreId, name = firstGenre)
                    }.also {
                        it.trackCount += 1
                    }
                }

                if (batch.size >= BATCH_SIZE) {
                    db.trackDao().insertAll(batch)
                    inserted += batch.size
                    progress(Stage.INSERTING_TRACKS, inserted, tracksTotal)
                    batch.clear()
                }
            }
            if (batch.isNotEmpty()) {
                db.trackDao().insertAll(batch)
                inserted += batch.size
                progress(Stage.INSERTING_TRACKS, inserted, tracksTotal)
                batch.clear()
            }

            progress(Stage.DERIVING_GROUPS, albums.size, albums.size)

            db.albumDao().insertAll(
                albums.values.map {
                    AlbumEntity(
                        id = it.id,
                        title = it.title,
                        albumArtist = it.albumArtist,
                        year = it.year,
                        trackCount = it.trackCount,
                        totalSizeBytes = it.totalSizeBytes,
                        hasResident = it.hasResident,
                    )
                },
            )
            db.artistDao().insertAll(
                artists.values.map {
                    ArtistEntity(
                        id = it.id,
                        name = it.name,
                        albumCount = it.albumIds.size,
                        trackCount = it.trackCount,
                    )
                },
            )
            db.genreDao().insertAll(
                genres.values.map {
                    GenreEntity(id = it.id, name = it.name, trackCount = it.trackCount)
                },
            )

            progress(
                Stage.INSERTING_GROUPS,
                albums.size + artists.size + genres.size,
                albums.size + artists.size + genres.size,
            )

            // Residency was already stamped onto each TrackEntity during the
            // insert loop. The Stage.APPLYING_MANIFEST callback fires here
            // for UI observability. A defensive setResidentFor pass would be
            // redundant.
            progress(Stage.APPLYING_MANIFEST, residentIds.size, residentIds.size)
        }

        progress(Stage.DONE, tracksTotal, tracksTotal)
        return ImportResult(
            tracksImported = catalog.tracks.size,
            albumsImported = albums.size,
            artistsImported = artists.size,
            genresImported = genres.size,
            residentMarked = if (manifestIn != null) residentIds.size else -1,
            durationMs = (System.nanoTime() - t0) / 1_000_000,
        )
    }

    private companion object {
        const val BATCH_SIZE = 500
    }

    private data class AlbumAccum(
        val id: String,
        val title: String,
        val albumArtist: List<String>,
        var year: Int?,
        var trackCount: Int = 0,
        var totalSizeBytes: Long = 0L,
        var hasResident: Boolean = false,
    )

    private data class ArtistAccum(
        val id: String,
        val name: String,
        var trackCount: Int = 0,
        val albumIds: HashSet<String> = HashSet(),
    )

    private data class GenreAccum(
        val id: String,
        val name: String,
        var trackCount: Int = 0,
    )
}
