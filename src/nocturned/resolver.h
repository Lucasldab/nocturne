#ifndef NOCTURNE_NOCTURNED_RESOLVER_H
#define NOCTURNE_NOCTURNED_RESOLVER_H

#include <stddef.h>
#include <stdio.h>

struct nocturne_db;
struct nocturne_config;

struct manifest_track {
    char *sha256;
    long long size_bytes;
    char **buckets;       /* sorted alphabetically */
    size_t buckets_n;
};

struct manifest {
    char *generated_at_iso;     /* deterministic; derived from input hash */
    long long cap_bytes;
    long long cap_effective_bytes;
    long long used_bytes;
    int resolver_version;
    int cold_start;             /* 1 if fallback path taken */

    struct manifest_track *resident;
    size_t resident_n;
};

/* Pure function: same (db, config) → byte-identical manifest. Reads from
 * `db` inside a single read transaction. Returns 0 on success, -1 error. */
int resolver_run(struct nocturne_db *db,
                 const struct nocturne_config *cfg,
                 struct manifest *out);

void manifest_free(struct manifest *m);

/* Stable JSON emitter. Keys in fixed order; tracks sorted by sha256;
 * buckets sorted alphabetically per track; trailing newline. Used by
 * golden tests AND by plan 02-06's publisher. Returns 0 on success, -1
 * on write error. */
int manifest_emit_json(const struct manifest *m, FILE *f);

#define RESOLVER_VERSION 1

#endif /* NOCTURNE_NOCTURNED_RESOLVER_H */
