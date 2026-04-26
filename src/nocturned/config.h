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

    struct bucket_config *buckets;
    size_t buckets_n;
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
