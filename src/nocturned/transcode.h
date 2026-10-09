#ifndef NOCTURNE_NOCTURNED_TRANSCODE_H
#define NOCTURNE_NOCTURNED_TRANSCODE_H

#include <stdbool.h>

/* Transcode configuration — populated from [transcode] config block, also
 * directly constructable for the standalone `nocturned transcode` CLI. */
struct transcode_cfg {
    bool enabled;
    const char *format;     /* "opus" | "aac" — only these two are wired */
    int bitrate_kbps;       /* 96/128/160/192/256 sane range; not validated */
    int level_lufs;         /* per-track loudness target, e.g. -18; 0 = off */
};

/* What transcode_audio() did about loudness. gain_db is NaN when levelling
 * was off; source_lufs is NaN when levelling was off or the measurement
 * failed (the encode then ran at 0 dB rather than failing). */
struct transcode_result {
    double gain_db;
    double source_lufs;
};

/* File-extension shorthand corresponding to cfg->format. Returns ".opus" /
 * ".m4a" / ".flac" for known formats; NULL on unsupported. Caller does NOT
 * free. */
const char *transcode_dst_ext(const struct transcode_cfg *cfg);

/* Run ffmpeg as a child process: read `src`, write `dst` per cfg's format +
 * bitrate. When cfg->level_lufs is non-zero, `src` is measured first and the
 * encode applies the per-track gain from loudness_gain(); `result` (may be
 * NULL) reports it. Embedded album art (cover) is preserved via `-map 0:v?`; metadata
 * via `-map_metadata 0`. Overwrites `dst` if it exists.
 *
 * Returns:
 *   0  — child exited 0
 *  -1  — fork/exec failed; errno set
 *  -2  — ffmpeg exited non-zero (stderr already drained to our stderr)
 *
 * Synchronous; blocks until the child completes. The caller is responsible
 * for ensuring `dst`'s parent directory exists.
 */
int transcode_audio(const char *src, const char *dst,
                    const struct transcode_cfg *cfg,
                    struct transcode_result *result);

#endif /* NOCTURNE_NOCTURNED_TRANSCODE_H */
