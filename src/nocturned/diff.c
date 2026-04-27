/*
 * diff.c — manifest diff formatter for `nocturned resolve --dry-run --diff`.
 *
 * STUB (TDD RED). The real walker that joins manifest_current with the
 * resolved-in-memory `struct manifest` lands in the GREEN commit.
 */

#define _GNU_SOURCE

#include "diff.h"

#include "db.h"
#include "resolver.h"

#include <stdio.h>

int print_resolve_diff(struct nocturne_db *db,
                       const struct manifest *next,
                       int as_json,
                       FILE *out)
{
    (void) db;
    (void) next;
    (void) as_json;
    (void) out;
    return 0;
}
