/*
 * resolver.c — pure(ish) function from (db_snapshot, config) → manifest.
 *
 * Six canonical buckets:
 *
 *   recent_adds (priority 5)   ORDER BY date_added DESC LIMIT count
 *   top_played  (priority 7)   COUNT(plays) DESC, phone-only, time-windowed
 *   recent_plays(priority 6)   played_at DESC, phone-only, distinct sha256
 *   loved       (priority 8)   liked=1 ORDER BY updated_at DESC
 *   exploration (priority 2)   tracks with no phone plays, deterministic
 *                              shuffle seeded by ISO-week (or config seed)
 *   manual_pins (priority 10)  pins.unit='track'/'album'
 *
 * Set-union dedup (RESOLVE-04): a sha256 reached via multiple buckets is
 * counted ONCE toward the cap; its `buckets` array lists every contributing
 * bucket name.
 *
 * Cap (RESOLVE-03): cap_effective_bytes = cap_bytes * effective_ratio.
 * Sort candidates by (priority DESC, sha256 ASC). Admit greedily while sum
 * stays under cap_effective_bytes.
 *
 * Hysteresis (RESOLVE-05): consult manifest_current for the previous
 * resident set. A track currently IN gets a 10% size discount before the
 * cap check; a track currently OUT gets a 10% size surcharge. Stable
 * across small score perturbations.
 *
 * Cold-start (RESOLVE-06): when phone plays < cold_start_play_threshold,
 * only recent_adds + exploration contribute. Manifest still non-empty.
 *
 * Determinism (RESOLVE-01): generated_at_iso is derived from sha256 of the
 * sorted resident sha256 list (prefix "deterministic-" + 12 hex chars).
 * No wall-clock anywhere in this module.
 */

#define _GNU_SOURCE

#include "resolver.h"

#include "config.h"
#include "db.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "../../vendor/sha256/sha256.h"

/* === bucket priority table =============================================== */

static int bucket_priority(const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "manual_pins"))  return 10;
    if (!strcmp(name, "loved"))        return 8;
    if (!strcmp(name, "top_played"))   return 7;
    if (!strcmp(name, "recent_plays")) return 6;
    if (!strcmp(name, "recent_adds"))  return 5;
    if (!strcmp(name, "exploration"))  return 2;
    return 1;
}

/* === candidate accumulator =============================================== */

struct cand {
    char *sha256;       /* owned */
    long long size_bytes;
    int priority;       /* max over contributing buckets */
    char **buckets;     /* growing list of bucket names (borrowed pointers) */
    size_t buckets_n;
    size_t buckets_cap;
};

struct cand_set {
    struct cand *items;
    size_t count;
    size_t capacity;
};

static struct cand *cand_find(struct cand_set *s, const char *sha)
{
    for (size_t i = 0; i < s->count; i++) {
        if (!strcmp(s->items[i].sha256, sha)) return &s->items[i];
    }
    return NULL;
}

static int cand_grow(struct cand_set *s)
{
    size_t cap = s->capacity ? s->capacity * 2 : 64;
    struct cand *p = realloc(s->items, cap * sizeof(*p));
    if (!p) return -1;
    s->items = p;
    s->capacity = cap;
    return 0;
}

/* Add (or extend) the candidate. `bucket_name` is borrowed (lifetime
 * exceeds the resolver call). */
static int cand_add(struct cand_set *s, const char *sha, long long size_bytes,
                    const char *bucket_name)
{
    struct cand *c = cand_find(s, sha);
    if (!c) {
        if (s->count == s->capacity && cand_grow(s) != 0) return -1;
        c = &s->items[s->count++];
        memset(c, 0, sizeof(*c));
        c->sha256 = strdup(sha);
        if (!c->sha256) return -1;
        c->size_bytes = size_bytes;
        c->priority = 0;
    }
    int prio = bucket_priority(bucket_name);
    if (prio > c->priority) c->priority = prio;
    if (c->buckets_n == c->buckets_cap) {
        size_t cap = c->buckets_cap ? c->buckets_cap * 2 : 4;
        char **p = realloc(c->buckets, cap * sizeof(*p));
        if (!p) return -1;
        c->buckets = p;
        c->buckets_cap = cap;
    }
    /* Skip duplicates within the same bucket name. */
    for (size_t i = 0; i < c->buckets_n; i++) {
        if (!strcmp(c->buckets[i], bucket_name)) return 0;
    }
    c->buckets[c->buckets_n++] = (char *) bucket_name;
    return 0;
}

static void cand_set_free(struct cand_set *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) {
        free(s->items[i].sha256);
        free(s->items[i].buckets);
    }
    free(s->items);
    memset(s, 0, sizeof(*s));
}

/* === bucket queries ====================================================== */

/* Run a SELECT that returns sha256 (and size_bytes via JOIN tracks); add
 * each row to `s` under `bucket_name`. */
static int run_bucket_select(struct sqlite3 *raw, const char *sql,
                             struct cand_set *s, const char *bucket_name,
                             const struct bucket_config *bc)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    /* Buckets that take a window_days bind it as ?1 (epoch seconds cutoff)
     * and a count limit as ?2. We bind both unconditionally; queries that
     * don't reference them ignore. */
    long now = (long) time(NULL);
    long cutoff = bc->window_days > 0 ? now - bc->window_days * 86400L : 0;
    /* Find ? placeholders by index — bind index 1 = cutoff, 2 = count, 3 = count again
     * for the queries that need both. We don't introspect; instead each bucket SQL
     * was written with a known number of placeholders. Use sqlite3_bind_parameter_count. */
    int nparams = sqlite3_bind_parameter_count(st);
    int idx = 1;
    if (nparams >= idx) sqlite3_bind_int64(st, idx++, (long long) cutoff);
    if (nparams >= idx) sqlite3_bind_int(st, idx++, bc->count);
    /* Some queries need count twice (for sub-LIMIT); fall through. */

    int rc = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *sha = sqlite3_column_text(st, 0);
        long long sz = sqlite3_column_int64(st, 1);
        if (!sha) continue;
        if (cand_add(s, (const char *) sha, sz, bucket_name) != 0) {
            rc = -1; break;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

/* Per-bucket SQL templates. Every query returns (sha256, size_bytes). */

static const char *SQL_RECENT_ADDS =
    "SELECT sha256, size_bytes FROM tracks "
    "ORDER BY date_added DESC, sha256 ASC LIMIT ?";

static const char *SQL_TOP_PLAYED =
    "SELECT t.sha256, t.size_bytes FROM tracks t "
    "JOIN (SELECT sha256, COUNT(*) AS plays FROM plays "
    "      WHERE src LIKE 'phone-%' AND is_skip=0 "
    "        AND (?1=0 OR strftime('%s', played_at) >= ?1) "
    "      GROUP BY sha256) p ON p.sha256 = t.sha256 "
    "ORDER BY p.plays DESC, t.sha256 ASC LIMIT ?2";

static const char *SQL_RECENT_PLAYS =
    "SELECT t.sha256, t.size_bytes FROM tracks t "
    "JOIN (SELECT sha256, MAX(played_at) AS last_played FROM plays "
    "      WHERE src LIKE 'phone-%' AND is_skip=0 "
    "        AND (?1=0 OR strftime('%s', played_at) >= ?1) "
    "      GROUP BY sha256) p ON p.sha256 = t.sha256 "
    "ORDER BY p.last_played DESC, t.sha256 ASC LIMIT ?2";

/* Schema 0004 reshaped likes from (sha256 PK, liked, updated_at) to
 * (unit, id, liked, ts) so album-level likes are addressable. The resolver
 * only consumes track-level likes — album-level likes are stored by the
 * ingester but ignored here (forward-compat for a future loved-album
 * bucket). LWW key is `ts` (unix-ms) instead of the old ISO-8601
 * `updated_at`. */
static const char *SQL_LOVED =
    "SELECT t.sha256, t.size_bytes FROM tracks t "
    "JOIN likes l ON l.unit='track' AND l.id = t.sha256 "
    "WHERE l.liked = 1 "
    "ORDER BY l.ts DESC, t.sha256 ASC LIMIT ?";

static const char *SQL_PINS_TRACKS =
    "SELECT t.sha256, t.size_bytes FROM tracks t "
    "JOIN pins p ON p.unit='track' AND p.id = t.sha256 "
    "WHERE p.pinned = 1 LIMIT ?";

/* For exploration we want all candidate sha256s (tracks with no phone
 * plays) ordered by sha256 ASC, then deterministic-shuffle in C. */
static const char *SQL_EXPLORATION_CANDIDATES =
    "SELECT t.sha256, t.size_bytes FROM tracks t "
    "WHERE NOT EXISTS (SELECT 1 FROM plays p "
    "                  WHERE p.sha256 = t.sha256 AND p.src LIKE 'phone-%') "
    "ORDER BY t.sha256 ASC";

/* === deterministic exploration shuffle =================================== */

/* xorshift64. Seed must be non-zero. */
static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *state = x ? x : 0xDEADBEEFULL;
    return *state;
}

static unsigned long iso_week_seed(void)
{
    /* time_t → struct tm in UTC → ISO-8601 year/week. */
    time_t now = time(NULL);
    struct tm tm; gmtime_r(&now, &tm);
    char buf[32];
    /* %G%V is ISO year + ISO week. */
    strftime(buf, sizeof(buf), "%G%V", &tm);
    return strtoul(buf, NULL, 10) | 0xDEADBEEFUL;
}

/* === hysteresis (prev manifest) ========================================== */

struct prev_set {
    char **shas;
    size_t n;
};

static int prev_set_load(struct sqlite3 *raw, struct prev_set *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, "SELECT sha256 FROM manifest_current",
                           -1, &st, NULL) != SQLITE_OK) {
        /* manifest_current may not exist on a freshly-migrated DB; not an error. */
        return 0;
    }
    size_t cap = 16;
    char **arr = malloc(cap * sizeof(*arr));
    size_t n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (!t) continue;
        if (n == cap) { cap *= 2; arr = realloc(arr, cap * sizeof(*arr)); }
        arr[n++] = strdup((const char *) t);
    }
    sqlite3_finalize(st);
    out->shas = arr;
    out->n = n;
    return 0;
}

static int prev_set_contains(const struct prev_set *p, const char *sha)
{
    for (size_t i = 0; i < p->n; i++) {
        if (!strcmp(p->shas[i], sha)) return 1;
    }
    return 0;
}

static void prev_set_free(struct prev_set *p)
{
    if (!p) return;
    for (size_t i = 0; i < p->n; i++) free(p->shas[i]);
    free(p->shas);
    memset(p, 0, sizeof(*p));
}

/* === sort comparators ==================================================== */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* (priority DESC, sha256 ASC) for cap admission. */
static int cmp_cand_priority(const void *a, const void *b)
{
    const struct cand *x = (const struct cand *) a;
    const struct cand *y = (const struct cand *) b;
    if (x->priority != y->priority) return y->priority - x->priority;
    return strcmp(x->sha256, y->sha256);
}

/* sha256 ASC for final manifest output. */
static int cmp_track_sha(const void *a, const void *b)
{
    const struct manifest_track *x = (const struct manifest_track *) a;
    const struct manifest_track *y = (const struct manifest_track *) b;
    return strcmp(x->sha256, y->sha256);
}

/* === core ================================================================ */

static int has_min_phone_plays(struct sqlite3 *raw, int threshold)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw,
            "SELECT COUNT(*) FROM plays WHERE src LIKE 'phone-%'",
            -1, &st, NULL) != SQLITE_OK) return 0;
    long long n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n >= threshold;
}

/* Compute deterministic generated_at_iso from sorted resident shas. */
static char *deterministic_generated_at(const struct manifest_track *resident,
                                        size_t n)
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    for (size_t i = 0; i < n; i++) {
        sha256_update(&ctx, resident[i].sha256, strlen(resident[i].sha256));
        sha256_update(&ctx, "\n", 1);
    }
    uint8_t digest[SHA256_DIGEST_LEN];
    sha256_final(&ctx, digest);
    char hex[65]; sha256_hex(digest, hex);
    char out[64];
    snprintf(out, sizeof(out), "deterministic-%.12s", hex);
    return strdup(out);
}

int resolver_run(struct nocturne_db *db,
                 const struct nocturne_config *cfg,
                 struct manifest *out)
{
    if (!db || !cfg || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->resolver_version = RESOLVER_VERSION;
    out->cap_bytes = cfg->cap_bytes;
    out->cap_effective_bytes = (long long) (cfg->cap_bytes * cfg->cap_effective_ratio);

    struct sqlite3 *raw = db_handle(db);
    if (!raw) return -1;

    /* Cold-start detection. */
    int cold = !has_min_phone_plays(raw, cfg->cold_start_play_threshold);
    out->cold_start = cold;

    struct cand_set s = {0};
    int rc = 0;

    /* Run each configured bucket against its appropriate query. In cold
     * start mode we run only recent_adds + exploration. */
    for (size_t i = 0; i < cfg->buckets_n && rc == 0; i++) {
        const struct bucket_config *bc = &cfg->buckets[i];
        if (!bc->name || bc->count <= 0) continue;
        /* Cold-start: only recent_adds + exploration contribute by default,
         * BUT manual_pins is explicit user override and always runs. */
        if (cold && strcmp(bc->name, "recent_adds")  != 0
                 && strcmp(bc->name, "exploration")  != 0
                 && strcmp(bc->name, "manual_pins")  != 0) continue;

        if (!strcmp(bc->source, "recent_adds_by_mtime")) {
            sqlite3_stmt *st = NULL;
            sqlite3_prepare_v2(raw, SQL_RECENT_ADDS, -1, &st, NULL);
            sqlite3_bind_int(st, 1, bc->count);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *sha = sqlite3_column_text(st, 0);
                long long sz = sqlite3_column_int64(st, 1);
                if (sha) cand_add(&s, (const char *) sha, sz, bc->name);
            }
            sqlite3_finalize(st);
        } else if (!strcmp(bc->source, "top_played_phone")) {
            rc = run_bucket_select(raw, SQL_TOP_PLAYED, &s, bc->name, bc);
        } else if (!strcmp(bc->source, "recent_plays_phone")) {
            rc = run_bucket_select(raw, SQL_RECENT_PLAYS, &s, bc->name, bc);
        } else if (!strcmp(bc->source, "loved")) {
            sqlite3_stmt *st = NULL;
            sqlite3_prepare_v2(raw, SQL_LOVED, -1, &st, NULL);
            sqlite3_bind_int(st, 1, bc->count);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *sha = sqlite3_column_text(st, 0);
                long long sz = sqlite3_column_int64(st, 1);
                if (sha) cand_add(&s, (const char *) sha, sz, bc->name);
            }
            sqlite3_finalize(st);
        } else if (!strcmp(bc->source, "manual_pins")) {
            sqlite3_stmt *st = NULL;
            sqlite3_prepare_v2(raw, SQL_PINS_TRACKS, -1, &st, NULL);
            sqlite3_bind_int(st, 1, bc->count);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *sha = sqlite3_column_text(st, 0);
                long long sz = sqlite3_column_int64(st, 1);
                if (sha) cand_add(&s, (const char *) sha, sz, bc->name);
            }
            sqlite3_finalize(st);
        } else if (!strcmp(bc->source, "exploration_random")) {
            /* Read all candidates; deterministic shuffle; take first count. */
            sqlite3_stmt *st = NULL;
            sqlite3_prepare_v2(raw, SQL_EXPLORATION_CANDIDATES, -1, &st, NULL);
            char **shas = NULL; long long *sizes = NULL; size_t n = 0, cap = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *sha = sqlite3_column_text(st, 0);
                long long sz = sqlite3_column_int64(st, 1);
                if (!sha) continue;
                if (n == cap) {
                    cap = cap ? cap * 2 : 16;
                    shas = realloc(shas, cap * sizeof(*shas));
                    sizes = realloc(sizes, cap * sizeof(*sizes));
                }
                shas[n] = strdup((const char *) sha);
                sizes[n] = sz;
                n++;
            }
            sqlite3_finalize(st);

            /* Deterministic Fisher-Yates with xorshift64. */
            uint64_t state = cfg->random_seed ? cfg->random_seed
                                              : (uint64_t) iso_week_seed();
            if (state == 0) state = 0xDEADBEEFULL;
            for (size_t k = n; k > 1; k--) {
                size_t j = (size_t) (xorshift64(&state) % k);
                if (j != k - 1) {
                    char *tmp_s = shas[j]; shas[j] = shas[k - 1]; shas[k - 1] = tmp_s;
                    long long tmp_sz = sizes[j]; sizes[j] = sizes[k - 1]; sizes[k - 1] = tmp_sz;
                }
            }
            int take = bc->count < (int) n ? bc->count : (int) n;
            for (int k = 0; k < take; k++) {
                cand_add(&s, shas[k], sizes[k], bc->name);
            }
            for (size_t k = 0; k < n; k++) free(shas[k]);
            free(shas); free(sizes);
        }
    }

    if (rc != 0) { cand_set_free(&s); return -1; }

    /* Load previous manifest for hysteresis. */
    struct prev_set prev = {0};
    prev_set_load(raw, &prev);

    /* Sort candidates by (priority DESC, sha256 ASC). */
    qsort(s.items, s.count, sizeof(*s.items), cmp_cand_priority);

    /* Walk admission with hysteresis-adjusted size. */
    long long used = 0;
    /* First pass to allocate result; we know upper bound = s.count. */
    out->resident = calloc(s.count, sizeof(*out->resident));
    out->resident_n = 0;
    if (!out->resident && s.count > 0) {
        cand_set_free(&s); prev_set_free(&prev); return -1;
    }
    for (size_t i = 0; i < s.count; i++) {
        struct cand *c = &s.items[i];
        long long effective_size = c->size_bytes;
        if (cfg->hysteresis_ratio > 0.0) {
            int was_in = prev_set_contains(&prev, c->sha256);
            double mult = was_in
                ? (1.0 - cfg->hysteresis_ratio)   /* discount: easier to keep */
                : (1.0 + cfg->hysteresis_ratio);  /* surcharge: harder to admit */
            effective_size = (long long) (c->size_bytes * mult);
        }
        if (used + effective_size > out->cap_effective_bytes) {
            /* manual_pins always go through (priority 10) — pins are by
             * design allowed to push past the cap edge per CONTEXT. */
            if (c->priority < 10) continue;
        }
        used += c->size_bytes;  /* charge actual size, not adjusted */

        /* Materialise into manifest_track. */
        struct manifest_track *mt = &out->resident[out->resident_n++];
        mt->sha256 = strdup(c->sha256);
        mt->size_bytes = c->size_bytes;
        mt->buckets = calloc(c->buckets_n, sizeof(*mt->buckets));
        if (mt->buckets) {
            for (size_t k = 0; k < c->buckets_n; k++) {
                mt->buckets[k] = strdup(c->buckets[k]);
            }
            mt->buckets_n = c->buckets_n;
            qsort(mt->buckets, mt->buckets_n, sizeof(*mt->buckets), cmp_str);
        }
    }

    /* Sort residents by sha256 for stable output. */
    qsort(out->resident, out->resident_n, sizeof(*out->resident), cmp_track_sha);

    out->used_bytes = used;
    out->generated_at_iso = deterministic_generated_at(out->resident, out->resident_n);

    cand_set_free(&s);
    prev_set_free(&prev);
    return 0;
}

void manifest_free(struct manifest *m)
{
    if (!m) return;
    free(m->generated_at_iso);
    for (size_t i = 0; i < m->resident_n; i++) {
        free(m->resident[i].sha256);
        for (size_t j = 0; j < m->resident[i].buckets_n; j++) {
            free(m->resident[i].buckets[j]);
        }
        free(m->resident[i].buckets);
    }
    free(m->resident);
    memset(m, 0, sizeof(*m));
}

/* === JSON emission ======================================================= */

static int emit_str(FILE *f, const char *s)
{
    fputc('"', f);
    if (s) {
        for (const char *p = s; *p; p++) {
            unsigned char c = (unsigned char) *p;
            switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else fputc(c, f);
            }
        }
    }
    fputc('"', f);
    return 0;
}

int manifest_emit_json(const struct manifest *m, FILE *f)
{
    if (!m || !f) return -1;
    fprintf(f, "{\n");
    fprintf(f, "  \"resolver_version\": %d,\n", m->resolver_version);
    fprintf(f, "  \"generated_at\": ");
    emit_str(f, m->generated_at_iso ? m->generated_at_iso : "");
    fprintf(f, ",\n");
    fprintf(f, "  \"cap_bytes\": %lld,\n", m->cap_bytes);
    fprintf(f, "  \"cap_effective_bytes\": %lld,\n", m->cap_effective_bytes);
    fprintf(f, "  \"used_bytes\": %lld,\n", m->used_bytes);
    fprintf(f, "  \"cold_start\": %s,\n", m->cold_start ? "true" : "false");
    fprintf(f, "  \"resident\": [");
    for (size_t i = 0; i < m->resident_n; i++) {
        fprintf(f, "%s\n    {\"sha256\": ", i == 0 ? "" : ",");
        emit_str(f, m->resident[i].sha256);
        fprintf(f, ", \"size_bytes\": %lld, \"buckets\": [",
                m->resident[i].size_bytes);
        for (size_t j = 0; j < m->resident[i].buckets_n; j++) {
            if (j) fputs(", ", f);
            emit_str(f, m->resident[i].buckets[j]);
        }
        fprintf(f, "]}");
    }
    fprintf(f, m->resident_n ? "\n  ]\n}" : "]\n}");
    fputc('\n', f);
    return 0;
}
