/*
 * discover.c — Weekly Discovery picker.
 *
 * Runs Monday 00:00 (systemd timer). Picks 20 tracks the user owns but
 * hasn't heard recently/at all, weighted by:
 *
 *   1. never_played       (highest priority, 50% of slots)
 *   2. aged_out (>120d)   (25% of slots)
 *   3. adjacent_to_loved  (15% of slots)
 *   4. random fill        (last 10% + any leftover)
 *
 * Per-album cap = 2 enforced across the whole batch. Re-running the same
 * Monday is a no-op (UNIQUE on sha256+week_start). Picks accumulate; the
 * resolver only reads MAX(week_start) so old weeks fade out automatically.
 *
 * Determinism: seeded with (year, ISO_week, library-path-hash). Same week +
 * same library = same picks if you re-run after a crash.
 */

#define _GNU_SOURCE

#include "discover.h"
#include "db.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

/* xorshift64 — same as resolver.c. Seed must be non-zero. */
static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *state = x ? x : 0xDEADBEEFULL;
    return *state;
}

/* Compute the most recent Monday (local time) in YYYY-MM-DD. */
static void monday_local(char out[16])
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    /* tm_wday: 0=Sunday, 1=Monday, ..., 6=Saturday. Days back to Monday. */
    int back = (tm.tm_wday == 0) ? 6 : (tm.tm_wday - 1);
    time_t monday = now - back * 86400;
    struct tm mtm;
    localtime_r(&monday, &mtm);
    strftime(out, 16, "%Y-%m-%d", &mtm);
}

static void iso_now_buf(char buf[40])
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, 40, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* Seed = year * 100 + ISO_week — stable for one calendar week. */
static uint64_t weekly_seed(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[16];
    strftime(buf, sizeof(buf), "%G%V", &tm);  /* ISO year + ISO week */
    uint64_t s = (uint64_t) atoll(buf);
    return s ? s : 0xC0FFEEULL;
}

/* === candidate buffer ==================================================== */

struct cand {
    char sha[65];
    char album[256];
    long long size_bytes;
    const char *reason;
};

struct cand_buf {
    struct cand *items;
    size_t n;
    size_t cap;
};

static int cb_push(struct cand_buf *b, const char *sha, const char *album,
                   long long size, const char *reason)
{
    if (b->n == b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 64;
        struct cand *items = realloc(b->items, newcap * sizeof(*items));
        if (!items) return -1;
        b->items = items;
        b->cap = newcap;
    }
    struct cand *c = &b->items[b->n++];
    strncpy(c->sha, sha, sizeof(c->sha) - 1);
    c->sha[sizeof(c->sha) - 1] = '\0';
    if (album) {
        strncpy(c->album, album, sizeof(c->album) - 1);
        c->album[sizeof(c->album) - 1] = '\0';
    } else {
        c->album[0] = '\0';
    }
    c->size_bytes = size;
    c->reason = reason;
    return 0;
}

static void cb_free(struct cand_buf *b)
{
    free(b->items);
    b->items = NULL;
    b->n = b->cap = 0;
}

/* Fisher-Yates shuffle, seeded. */
static void shuffle(struct cand *items, size_t n, uint64_t *state)
{
    for (size_t i = n; i > 1; i--) {
        size_t j = (size_t)(xorshift64(state) % i);
        struct cand tmp = items[i - 1];
        items[i - 1] = items[j];
        items[j] = tmp;
    }
}

/* Already-picked set: sha256s already chosen this run, used to prevent the
 * same track being picked twice from different reason buckets. */
struct sha_set {
    char (*items)[65];
    size_t n;
    size_t cap;
};

static int set_has(const struct sha_set *s, const char *sha)
{
    for (size_t i = 0; i < s->n; i++) {
        if (!strcmp(s->items[i], sha)) return 1;
    }
    return 0;
}

static int set_add(struct sha_set *s, const char *sha)
{
    if (s->n == s->cap) {
        size_t newcap = s->cap ? s->cap * 2 : 32;
        char (*items)[65] = realloc(s->items, newcap * sizeof(*items));
        if (!items) return -1;
        s->items = items;
        s->cap = newcap;
    }
    strncpy(s->items[s->n], sha, 64);
    s->items[s->n][64] = '\0';
    s->n++;
    return 0;
}

static void set_free(struct sha_set *s)
{
    free(s->items);
    s->items = NULL;
    s->n = s->cap = 0;
}

/* Per-album count: enforces the cap=2 rule. */
struct album_count {
    char album[256];
    int count;
};

struct album_counter {
    struct album_count *items;
    size_t n;
    size_t cap;
};

static int ac_get(const struct album_counter *a, const char *album)
{
    if (!album || !*album) return 0;
    for (size_t i = 0; i < a->n; i++) {
        if (!strcmp(a->items[i].album, album)) return a->items[i].count;
    }
    return 0;
}

static int ac_inc(struct album_counter *a, const char *album)
{
    if (!album || !*album) return 0;
    for (size_t i = 0; i < a->n; i++) {
        if (!strcmp(a->items[i].album, album)) {
            a->items[i].count++;
            return a->items[i].count;
        }
    }
    if (a->n == a->cap) {
        size_t newcap = a->cap ? a->cap * 2 : 32;
        struct album_count *items = realloc(a->items, newcap * sizeof(*items));
        if (!items) return -1;
        a->items = items;
        a->cap = newcap;
    }
    strncpy(a->items[a->n].album, album, sizeof(a->items[a->n].album) - 1);
    a->items[a->n].album[sizeof(a->items[a->n].album) - 1] = '\0';
    a->items[a->n].count = 1;
    a->n++;
    return 1;
}

static void ac_free(struct album_counter *a)
{
    free(a->items);
    a->items = NULL;
    a->n = a->cap = 0;
}

/* === SQL ================================================================= */

static const char *SQL_NEVER_PLAYED =
    "SELECT t.sha256, t.album, t.size_bytes "
    "FROM tracks t "
    "WHERE NOT EXISTS (SELECT 1 FROM plays p "
    "                  WHERE p.sha256 = t.sha256 AND p.src LIKE 'phone-%') "
    "ORDER BY t.sha256 ASC";

static const char *SQL_AGED_OUT =
    "SELECT t.sha256, t.album, t.size_bytes "
    "FROM tracks t "
    "JOIN ( "
    "  SELECT sha256, MAX(played_at) AS last_play "
    "  FROM plays WHERE src LIKE 'phone-%' "
    "  GROUP BY sha256 "
    ") p ON p.sha256 = t.sha256 "
    "WHERE julianday('now') - julianday(p.last_play) > 120 "
    "ORDER BY p.last_play ASC";

/* Adjacent-to-loved: tracks whose album_artist matches a pinned track's
 * album_artist, but the candidate itself has zero phone plays. */
static const char *SQL_ADJACENT_TO_LOVED =
    "SELECT t.sha256, t.album, t.size_bytes "
    "FROM tracks t "
    "WHERE t.album_artist IN ( "
    "  SELECT DISTINCT t2.album_artist FROM tracks t2 "
    "  JOIN pins p ON p.unit='track' AND p.id = t2.sha256 AND p.pinned = 1 "
    ") "
    "AND NOT EXISTS (SELECT 1 FROM plays p "
    "                WHERE p.sha256 = t.sha256 AND p.src LIKE 'phone-%') "
    "ORDER BY t.sha256 ASC";

static const char *SQL_RANDOM_FILL =
    "SELECT t.sha256, t.album, t.size_bytes "
    "FROM tracks t "
    "ORDER BY t.sha256 ASC";

/* === picker ============================================================== */

#define PER_ALBUM_CAP 2

/* Try to take up to `target` picks from `pool`, respecting the album cap
 * and dedup against the already-picked set. Returns count actually taken. */
static int take_from_pool(struct cand_buf *pool,
                          struct cand_buf *picks,
                          struct sha_set *picked,
                          struct album_counter *albums,
                          int target)
{
    int taken = 0;
    for (size_t i = 0; i < pool->n && taken < target; i++) {
        const struct cand *c = &pool->items[i];
        if (set_has(picked, c->sha)) continue;
        if (ac_get(albums, c->album) >= PER_ALBUM_CAP) continue;
        if (cb_push(picks, c->sha, c->album, c->size_bytes, c->reason) != 0) {
            return taken;
        }
        set_add(picked, c->sha);
        ac_inc(albums, c->album);
        taken++;
    }
    return taken;
}

static int load_pool(struct sqlite3 *raw, const char *sql, const char *reason,
                     struct cand_buf *out)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *sha = sqlite3_column_text(st, 0);
        const unsigned char *album = sqlite3_column_text(st, 1);
        long long sz = sqlite3_column_int64(st, 2);
        if (!sha) continue;
        cb_push(out, (const char *) sha, (const char *) album, sz, reason);
    }
    sqlite3_finalize(st);
    return 0;
}

int discover_run(struct nocturne_db *db, int count, struct discover_stats *out)
{
    if (!db || count <= 0 || !out) return -1;
    memset(out, 0, sizeof(*out));
    monday_local(out->week_start);

    struct sqlite3 *raw = db_handle(db);
    if (!raw) return -1;

    char iso_now[40];
    iso_now_buf(iso_now);

    /* Quotas per reason (rounded so they sum to count for default 20). */
    int quota_never  = (count * 50 + 99) / 100;   /* 10 of 20 */
    int quota_aged   = (count * 25 + 99) / 100;   /* 5 of 20 */
    int quota_adj    = (count * 15 + 99) / 100;   /* 3 of 20 */
    /* random fills the rest */

    struct cand_buf never = {0}, aged = {0}, adj = {0}, fill = {0};
    if (load_pool(raw, SQL_NEVER_PLAYED, "never_played", &never) != 0) goto fail;
    if (load_pool(raw, SQL_AGED_OUT, "aged_out", &aged) != 0) goto fail;
    if (load_pool(raw, SQL_ADJACENT_TO_LOVED, "adjacent_to_loved", &adj) != 0) goto fail;
    if (load_pool(raw, SQL_RANDOM_FILL, "random", &fill) != 0) goto fail;

    out->candidates_seen = (long long)(never.n + aged.n + adj.n + fill.n);

    uint64_t seed = weekly_seed();
    shuffle(never.items, never.n, &seed);
    shuffle(aged.items,  aged.n,  &seed);
    shuffle(adj.items,   adj.n,   &seed);
    shuffle(fill.items,  fill.n,  &seed);

    struct cand_buf picks = {0};
    struct sha_set picked = {0};
    struct album_counter albums = {0};

    out->picked_never_played    = take_from_pool(&never, &picks, &picked, &albums, quota_never);
    out->picked_aged_out        = take_from_pool(&aged,  &picks, &picked, &albums, quota_aged);
    out->picked_adjacent_to_loved = take_from_pool(&adj, &picks, &picked, &albums, quota_adj);
    /* Fill remainder from any-track random pool. */
    int remaining = count - (int)(picks.n);
    if (remaining > 0) {
        out->picked_random = take_from_pool(&fill, &picks, &picked, &albums, remaining);
    }
    out->total_picked = (long long) picks.n;

    /* Persist. INSERT OR IGNORE to make re-running same week a no-op. */
    if (db_begin(db) != 0) { cb_free(&picks); set_free(&picked); ac_free(&albums); goto fail; }

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(raw,
        "INSERT OR IGNORE INTO weekly_discovery_picks "
        "  (sha256, week_start, reason, picked_at) VALUES (?, ?, ?, ?)",
        -1, &ins, NULL);
    for (size_t i = 0; i < picks.n; i++) {
        sqlite3_bind_text(ins, 1, picks.items[i].sha, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, out->week_start, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, picks.items[i].reason, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, iso_now, -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);

    if (db_commit(db) != 0) { cb_free(&picks); set_free(&picked); ac_free(&albums); goto fail; }

    cb_free(&never); cb_free(&aged); cb_free(&adj); cb_free(&fill);
    cb_free(&picks); set_free(&picked); ac_free(&albums);
    return 0;

fail:
    cb_free(&never); cb_free(&aged); cb_free(&adj); cb_free(&fill);
    return -1;
}
