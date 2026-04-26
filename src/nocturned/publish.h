#ifndef NOCTURNE_NOCTURNED_PUBLISH_H
#define NOCTURNE_NOCTURNED_PUBLISH_H

struct nocturne_db;

/* Atomically write catalog.json + manifest.json into `out_dir`.
 *
 * catalog.json: built via catalog_emit (every track row).
 * manifest.json: built from manifest_current + manifest_meta (populated
 * by `nocturned resolve`). If manifest_current is empty, returns -1
 * with a stderr message — we don't publish a stale or empty manifest.
 *
 * Returns 0 on success, -1 on error. */
int publish_run(struct nocturne_db *db, const char *out_dir);

#endif
