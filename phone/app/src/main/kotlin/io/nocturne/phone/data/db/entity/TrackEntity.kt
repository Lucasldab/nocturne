package io.nocturne.phone.data.db.entity

import androidx.compose.runtime.Immutable
import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey

// NOTE: We use raw `String` rather than the `Sha256Hex` / `SyntheticId`
// typealiases from io.nocturne.phone.data.db.RoomTypes because Room's KSP
// processor (room-compiler 2.8.0) emits MissingType errors when entity
// fields reference typealiases declared in another package. The aliases
// remain available for non-entity code to express intent.

/**
 * Track row. Mirrors the catalog.json shape from Phase-2's catalog publisher.
 *
 * - `id` is the catalog sha256 hex string (64 lowercase chars). Same identity
 *   the daemon publishes; never re-derived locally.
 * - Multi-value tag fields (`artist`, `albumArtist`, `genre`) are stored as
 *   JSON-encoded `List<String>` via [Converters].
 * - `albumId`, `albumArtistId`, `genreId` are SYNTHETIC ids derived during
 *   import (Plan 04-03). They group rows for browse axes (CAT-02). Algorithm:
 *   `albumId = sha256("${albumArtist[0]} ${album.lowercase()}")`,
 *   `albumArtistId = sha256(albumArtist[0].lowercase())`,
 *   `genreId = sha256(genre[0].lowercase())` (null if genre list is empty).
 * - `searchBlob` is pre-folded (NFKD strip-marks lowercase whitespace-collapse).
 *   The accent-fold algorithm lives in CatalogImporter (Plan 04-03);
 *   TestFixtures duplicates it for unit tests.
 * - `isResident` is derived from manifest.json during import; `resident/...`
 *   prefix paths are flagged true. Plan 04-05 dims non-resident rows.
 */
@Entity(
    tableName = "tracks",
    indices = [
        Index("albumId"),
        Index("albumArtistId"),
        Index("genreId"),
        Index("title"),
        Index("dateAdded"),
        Index("mtimeNs"),
    ],
)
@Immutable
data class TrackEntity(
    @PrimaryKey val id: String,
    val title: String,
    val artist: List<String>,
    val albumArtist: List<String>,
    val album: String,
    val albumId: String,
    val albumArtistId: String,
    val genre: List<String>,
    val genreId: String?,
    val trackNumber: Int?,
    val discNumber: Int?,
    val year: Int?,
    val durationMs: Long?,
    val sizeBytes: Long,
    val format: String,
    val mtimeNs: Long,
    val dateAdded: String,
    val path: String,
    val isResident: Boolean,
    val searchBlob: String,
)
