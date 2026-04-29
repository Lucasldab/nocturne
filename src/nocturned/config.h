#ifndef NOCTURNE_NOCTURNED_CONFIG_H
#define NOCTURNE_NOCTURNED_CONFIG_H

#include <stddef.h>

struct bucket_config {
    char *name;          /* "recent_adds" / "top_played" / ... */
    int count;           /* max tracks taken from this bucket */
    char *source;        /* "recent_adds_by_mtime", ... */
    double weight;       /* tie-break weight; 0..1 */
    int window_days;     /* time window; 0 = all-time */
};

struct nocturne_config {
    char *library_root;          /* [library].path */
    char *sync_meta_root;        /* [sync_meta].path */
    long long cap_bytes;         /* [cap].bytes; default 12 GiB */
    double cap_effective_ratio;  /* [cap].effective_ratio; default 0.70 */
    double hysteresis_ratio;     /* [resolver].hysteresis; default 0.10 */
    int cold_start_play_threshold;
    unsigned long random_seed;

    /* Syncthing identity (Phase 3 — sync-config XML emission). */
    char *syncthing_desktop_name;       /* [syncthing].desktop_name */
    char *syncthing_phone_name;         /* [syncthing].phone_name */
    char *syncthing_phone_sync_files;   /* [syncthing.phone].sync_files_path */
    char *syncthing_phone_sync_meta;    /* [syncthing.phone].sync_meta_path */
    char *syncthing_desktop_device_id;  /* [syncthing].desktop_device_id */
    char *syncthing_phone_device_id;    /* [syncthing].phone_device_id */

    struct bucket_config *buckets;
    size_t buckets_n;

    /* [transcode] — when enabled, rotate's promote step writes a lossy copy
     * to resident/ instead of hardlinking the FLAC. Off by default; the
     * standalone `nocturned transcode` CLI ignores this and uses argv. */
    int transcode_enabled;       /* 0/1 */
    char *transcode_format;      /* "opus" | "aac"; NULL when default */
    int transcode_bitrate_kbps;  /* 0 → built-in default (128) */

    /* [discover] — Weekly Discovery picker. exclude_album_substrings is a
     * semicolon-separated list (e.g. "Live;Unplugged;Acoustic"); any track
     * whose album column contains one of these substrings is filtered from
     * the candidate pool. Empty/NULL = no filter. */
    char *discover_exclude_album_substrings;

    /* [listenbrainz] — read-only API access doesn't need a token; submit-
     * listens (v2 scrobbling) does. Token from
     * https://listenbrainz.org/profile/. Username is also stored so the
     * recommendation endpoints know whose recs to fetch (v3). */
    char *listenbrainz_username;
    char *listenbrainz_user_token;
};

/* Returns 0 on success. NULL or missing path → defaults (no error).
 * Parse error → -1, message to stderr. */
int config_load(const char *path, struct nocturne_config *out);

/* Initialise *out with documented defaults — used both by config_load when
 * no file is present and by tests that want a baseline config. Returns 0
 * on success, -1 on OOM. */
int config_default(struct nocturne_config *out);

void config_free(struct nocturne_config *c);

#endif /* NOCTURNE_NOCTURNED_CONFIG_H */
