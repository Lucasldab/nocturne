package io.nocturne.phone.data.db.entity

import androidx.room.TypeConverter
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.builtins.serializer
import kotlinx.serialization.json.Json

/**
 * Room TypeConverter for `List<String>` <-> JSON string. Used for multi-value
 * tag fields (artist, albumArtist, genre) per Phase-2 catalog.json contract.
 */
class Converters {
    private val json = Json { ignoreUnknownKeys = true }
    private val serializer = ListSerializer(String.serializer())

    @TypeConverter
    fun stringListToJson(v: List<String>): String = json.encodeToString(serializer, v)

    @TypeConverter
    fun jsonToStringList(s: String): List<String> =
        if (s.isBlank()) emptyList() else json.decodeFromString(serializer, s)
}
