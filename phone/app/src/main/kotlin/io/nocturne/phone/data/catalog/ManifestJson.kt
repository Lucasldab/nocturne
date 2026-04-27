package io.nocturne.phone.data.catalog

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * `manifest.json` shape — Phase-3 rotate engine output. Identifies the
 * subset of catalog tracks currently materialised on the phone (resident).
 */
@Serializable
data class ManifestJson(
    val v: Int,
    @SerialName("generated_at") val generatedAt: String,
    @SerialName("cap_bytes") val capBytes: Long,
    @SerialName("used_bytes") val usedBytes: Long,
    val resident: List<ResidentEntry>,
)

@Serializable
data class ResidentEntry(
    val id: String,
    val buckets: List<String> = emptyList(),
)
