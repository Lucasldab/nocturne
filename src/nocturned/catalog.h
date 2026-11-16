#ifndef NOCTURNE_NOCTURNED_CATALOG_H
#define NOCTURNE_NOCTURNED_CATALOG_H

#include <stdio.h>

struct nocturne_db;

/* Emit catalog.json shape per CONTEXT into `f`. Stable key order, tracks
 * sorted by sha256. Multi-value tags emit as JSON arrays (single values
 * become single-element arrays for shape stability). Path is RELATIVE
 * to library_root from scan_meta — no absolute desktop paths leak.
 * Returns 0 on success, -1 on error. */
int catalog_emit(struct nocturne_db *db, FILE *f);

/* Same as catalog_emit, but stamps `generated_at` with the given string
 * instead of the current time (NULL = now). publish_run uses it to
 * re-emit with the previous stamp and detect an unchanged catalog. */
int catalog_emit_at(struct nocturne_db *db, FILE *f, const char *generated_at);

/* Read catalog.json from `f` and UPSERT every track into `db`, keyed on
 * sha256. Path reconstructed by joining `library_root` from scan_meta
 * with the relative path. Refuses if v != 1. Returns rows imported,
 * -1 on error. */
long catalog_import(struct nocturne_db *db, FILE *f);

#endif
