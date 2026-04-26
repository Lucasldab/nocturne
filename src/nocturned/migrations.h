#ifndef NOCTURNE_NOCTURNED_MIGRATIONS_H
#define NOCTURNE_NOCTURNED_MIGRATIONS_H

struct nocturne_db;

/* Apply any pending migrations. Returns 0 on success, -1 on error. */
int migrations_apply(struct nocturne_db *db);

/* Highest schema version this binary knows about. */
int migrations_target_version(void);

#endif /* NOCTURNE_NOCTURNED_MIGRATIONS_H */
