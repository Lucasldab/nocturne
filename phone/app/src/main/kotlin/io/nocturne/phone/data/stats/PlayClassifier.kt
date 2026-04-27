package io.nocturne.phone.data.stats

/**
 * Phase 6 (STATS-01 / STATS-02 / D-12): play-vs-skip threshold.
 *
 * A play counts when the user listened past `min(durationMs / 2, 240_000ms)` —
 * 50% of the track OR 4 minutes, whichever is smaller. Tracks under 8 minutes
 * use the 50% rule; tracks ≥ 8 minutes use the flat 4-minute cap.
 *
 * Pure function — no Android dependencies, testable in plain JVM. Negative
 * inputs are coerced to 0 (defensive against `oldPosition.positionMs < 0`
 * edge cases).
 *
 * Pitfall 2 (gapless tail-trim) mitigation lives at the CALLER: the listener
 * clamps `playedMs` to `durationMs` before invoking this function. This
 * function trusts its inputs.
 */
object PlayClassifier {
    const val MAX_THRESHOLD_MS: Long = 240_000L  // 4 minutes

    fun isPlay(playedMs: Long, durationMs: Long): Boolean {
        if (durationMs <= 0L) return false
        val played = playedMs.coerceAtLeast(0L)
        val threshold = minOf(durationMs / 2L, MAX_THRESHOLD_MS)
        return played >= threshold
    }
}
