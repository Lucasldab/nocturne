/*
 * why_cmd.c — `nocturned why <track-id>` (TUNE-03 daemon CLI).
 *
 * Read-only: looks up a resident track in <sync_meta>/manifest.json (the
 * authoritative source — what the phone consumes via Syncthing) and
 * prints the bucket(s) that qualified it.
 *
 * Lock policy: NO lockfile acquisition (mirrors doctor_cmd_main). This
 * lets `nocturned why` coexist with a long-running `nocturned watch` —
 * it must work mid-watch or it's useless during real tuning sessions.
 *
 * Anti-pattern (do NOT do): if manifest.json is missing, exit 1 with a
 * pointer at `nocturned publish`. We never silently fall back to a
 * fresh in-process resolve — that would take the writer lock and
 * contradict the read-only contract (Plan 08-01 Anti-pattern, Pitfall 1).
 *
 * Divergence warning (Pitfall 2): when manifest.json exists at the
 * default path AND a `manifest_current` row exists in SQLite for the
 * matched id with a different buckets_csv, print a stderr warning
 * naming both sources — the user is told which one we used. Skipped
 * when --manifest is in use (the user asked us to read that file
 * specifically; we don't second-guess them).
 *
 * Exit codes (per cli.h):
 *   0 (NOCT_EXIT_OK)      track found
 *   1 (NOCT_EXIT_FAILURE) track not in manifest, manifest unreadable,
 *                         or ambiguous prefix
 *  64 (NOCT_EXIT_USAGE)   bad track-id (NULL, < 8 chars, non-hex chars)
 */
#define _GNU_SOURCE

#include "cli.h"
#include "config.h"
#include "db.h"
#include "paths.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>
#include <sqlite3.h>

/* Minimum prefix length when not given a full 64-char hex id. Below this,
 * collision risk is too high — we refuse to guess. */
#define WHY_MIN_PREFIX 8
#define WHY_FULL_HEX   64

/* Validate that `s` is non-NULL, length-bounded, and pure lowercase hex.
 * Caller is expected to have lower-cased input already; we tolerate
 * mixed-case and lowercase-in-place via *out_normalised. Returns:
 *   0  ok (writes lower-case copy to out_normalised; caller frees)
 *   1  too short (< WHY_MIN_PREFIX chars)
 *   2  too long  (> WHY_FULL_HEX chars)
 *   3  non-hex char encountered
 *   4  NULL / empty
 */
static int validate_track_id(const char *s, char **out_normalised)
{
    if (!s || !*s) return 4;
    size_t n = strlen(s);
    if (n < WHY_MIN_PREFIX) return 1;
    if (n > WHY_FULL_HEX)   return 2;
    char *low = malloc(n + 1);
    if (!low) return 4;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            free(low);
            return 3;
        }
        low[i] = (char) tolower(c);
    }
    low[n] = '\0';
    *out_normalised = low;
    return 0;
}

/* Build "<sync_meta_root>/manifest.json" into the heap. NULL on OOM/missing. */
static char *default_manifest_path(const struct nocturne_config *cfg)
{
    if (!cfg || !cfg->sync_meta_root || !*cfg->sync_meta_root) return NULL;
    size_t n = strlen(cfg->sync_meta_root) + strlen("/manifest.json") + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s/manifest.json", cfg->sync_meta_root);
    return p;
}

/* Comma-join the strings in `arr` into a fresh heap buffer. Caller frees.
 * Empty array → empty string ""; NULL on OOM. */
static char *buckets_csv_from_array(json_t *arr)
{
    size_t n = json_array_size(arr);
    size_t cap = 64;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t cur = 0;
    for (size_t i = 0; i < n; i++) {
        json_t *e = json_array_get(arr, i);
        if (!json_is_string(e)) continue;
        const char *s = json_string_value(e);
        size_t need = strlen(s) + (i ? 1 : 0) + 1;
        while (cur + need >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        if (i) buf[cur++] = ',';
        memcpy(buf + cur, s, strlen(s));
        cur += strlen(s);
        buf[cur] = '\0';
    }
    return buf;
}

/* Look up `id_or_prefix` (already lowercase, hex-validated) in
 * SQLite's `manifest_current` table. Returns:
 *   1  found (writes csv to *out_csv; caller frees)
 *   0  not found
 *  -1  any error (DB unavailable, prepare failed, etc.)
 * The current implementation only consults exact-id matches because
 * divergence-warning is a best-effort niceness, not a correctness gate. */
static int lookup_manifest_current_csv(const char *id, char **out_csv)
{
    *out_csv = NULL;
    const char *db_path = paths_db_file();
    if (!db_path) return -1;

    /* Open read-only; nocturned uses WAL so concurrent readers are safe. */
    sqlite3 *raw = NULL;
    if (sqlite3_open_v2(db_path, &raw, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(raw,
        "SELECT buckets_csv FROM manifest_current WHERE sha256 = ?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(raw); return -1; }
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);

    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t) {
            *out_csv = strdup((const char *) t);
            found = (*out_csv != NULL) ? 1 : -1;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(raw);
    return found;
}

int why_cmd_main(struct cli_args *args)
{
    if (!args) return NOCT_EXIT_USAGE;

    /* --- 1. Validate the track-id ----------------------------------- */
    char *needle = NULL;
    int v = validate_track_id(args->track_id, &needle);
    if (v != 0) {
        switch (v) {
        case 1:
            fprintf(stderr,
                "nocturned why: prefix must be at least 8 hex chars "
                "(or a full 64-char sha256)\n");
            break;
        case 2:
            fprintf(stderr,
                "nocturned why: track-id too long (max 64 hex chars)\n");
            break;
        case 3:
            fprintf(stderr,
                "nocturned why: track-id must be hex only (0-9, a-f)\n");
            break;
        case 4:
        default:
            fprintf(stderr,
                "nocturned why: missing track-id "
                "(usage: nocturned why <track-id>)\n");
            break;
        }
        free(needle);
        return NOCT_EXIT_USAGE;
    }
    size_t needle_len = strlen(needle);
    int needle_is_full = (needle_len == WHY_FULL_HEX);

    /* --- 2. Resolve manifest.json path ------------------------------ */
    char *manifest_path = NULL;
    int using_override = (args->manifest_path_override &&
                          *args->manifest_path_override);
    struct nocturne_config cfg;
    int cfg_loaded = 0;

    if (using_override) {
        manifest_path = strdup(args->manifest_path_override);
    } else {
        const char *cfg_path = args->config_path ? args->config_path
                                                 : paths_config_file();
        if (config_load(cfg_path, &cfg) != 0) {
            fprintf(stderr,
                "nocturned why: cannot load config from %s\n",
                cfg_path ? cfg_path : "(null)");
            free(needle);
            config_free(&cfg);
            return NOCT_EXIT_FAILURE;
        }
        cfg_loaded = 1;
        manifest_path = default_manifest_path(&cfg);
        if (!manifest_path) {
            fprintf(stderr,
                "nocturned why: sync_meta.path not set in config\n");
            free(needle);
            config_free(&cfg);
            return NOCT_EXIT_FAILURE;
        }
    }

    /* --- 3. Load manifest.json -------------------------------------- */
    json_error_t jerr;
    json_t *root = json_load_file(manifest_path, 0, &jerr);
    if (!root) {
        fprintf(stderr,
            "nocturned why: cannot read %s: %s\n",
            manifest_path,
            jerr.text[0] ? jerr.text : "load failed");
        fprintf(stderr,
            "nocturned why: hint — run `nocturned publish` to (re)create it\n");
        free(manifest_path);
        free(needle);
        if (cfg_loaded) config_free(&cfg);
        return NOCT_EXIT_FAILURE;
    }

    /* --- 4. Walk resident[] looking for matches --------------------- */
    json_t *resident = json_object_get(root, "resident");
    if (!json_is_array(resident)) {
        fprintf(stderr,
            "nocturned why: %s has no 'resident' array (corrupt or v!=1?)\n",
            manifest_path);
        json_decref(root);
        free(manifest_path);
        free(needle);
        if (cfg_loaded) config_free(&cfg);
        return NOCT_EXIT_FAILURE;
    }

    /* On match: capture id (heap-dup; root may be freed mid-output)
     * and a CSV-of-buckets. We allow up to N matches for collision
     * diagnostics on stderr; only commit to text/JSON output if there's
     * exactly 1. */
    enum { MAX_AMBIG_REPORT = 5 };
    int matches = 0;
    char *match_id = NULL;
    char *match_csv = NULL;
    json_t *match_buckets = NULL;       /* borrowed reference; root owns */
    char *ambig_ids[MAX_AMBIG_REPORT];
    int ambig_n = 0;

    size_t rn = json_array_size(resident);
    for (size_t i = 0; i < rn; i++) {
        json_t *ent = json_array_get(resident, i);
        if (!json_is_object(ent)) continue;
        json_t *jid = json_object_get(ent, "id");
        if (!json_is_string(jid)) continue;
        const char *id = json_string_value(jid);
        if (!id) continue;

        size_t id_len = strlen(id);
        int hit = 0;
        if (needle_is_full) {
            hit = (id_len == WHY_FULL_HEX) && (strcmp(id, needle) == 0);
        } else {
            /* Prefix match: needle is 8..63 chars, lowercase hex. id is
             * always lowercase hex per publish.c contract. */
            hit = (id_len >= needle_len) &&
                  (strncmp(id, needle, needle_len) == 0);
        }
        if (!hit) continue;

        matches++;
        if (matches == 1) {
            match_id = strdup(id);
            match_buckets = json_object_get(ent, "buckets");
            if (json_is_array(match_buckets)) {
                match_csv = buckets_csv_from_array(match_buckets);
            } else {
                match_csv = strdup("");
            }
        }
        if (ambig_n < MAX_AMBIG_REPORT) {
            ambig_ids[ambig_n++] = strdup(id);
        }
    }

    /* --- 5. Decide output ------------------------------------------- */
    int rc;
    if (matches == 0) {
        fprintf(stderr,
            "nocturned why: track-id not in manifest: %s\n", needle);
        rc = NOCT_EXIT_FAILURE;
    } else if (matches > 1) {
        fprintf(stderr,
            "nocturned why: ambiguous prefix: matches %d residents\n",
            matches);
        for (int i = 0; i < ambig_n; i++) {
            fprintf(stderr, "    %s\n", ambig_ids[i]);
        }
        if (matches > ambig_n) {
            fprintf(stderr, "    ... and %d more\n", matches - ambig_n);
        }
        rc = NOCT_EXIT_FAILURE;
    } else {
        /* Exactly one match — print and (maybe) divergence-warn. */
        if (args->json) {
            /* Single-line JSON, mirrors publish.c shape: {"id":"...","buckets":[...]}.
             * We rebuild the buckets array from match_csv so the output
             * is independent of any json_dumps formatting wobbles. */
            printf("{\"id\":\"%s\",\"buckets\":[", match_id);
            int first = 1;
            if (match_csv && *match_csv) {
                char *dup = strdup(match_csv);
                char *tok = strtok(dup, ",");
                while (tok) {
                    printf("%s\"%s\"", first ? "" : ",", tok);
                    first = 0;
                    tok = strtok(NULL, ",");
                }
                free(dup);
            }
            printf("]}\n");
        } else {
            printf("%s  %s\n", match_id, match_csv ? match_csv : "");
        }

        /* Divergence warning vs SQLite manifest_current — only when the
         * caller did not pass --manifest. */
        if (!using_override) {
            char *db_csv = NULL;
            int lk = lookup_manifest_current_csv(match_id, &db_csv);
            if (lk == 1 && db_csv && match_csv &&
                strcmp(db_csv, match_csv) != 0) {
                fprintf(stderr,
                    "warning: manifest.json and manifest_current disagree "
                    "for %s; phone shows what's in manifest.json\n",
                    match_id);
                fprintf(stderr,
                    "    manifest.json     : %s\n", match_csv);
                fprintf(stderr,
                    "    manifest_current  : %s\n", db_csv);
            }
            free(db_csv);
        }
        rc = NOCT_EXIT_OK;
    }

    /* --- 6. Cleanup ------------------------------------------------- */
    for (int i = 0; i < ambig_n; i++) free(ambig_ids[i]);
    free(match_id);
    free(match_csv);
    json_decref(root);
    free(manifest_path);
    free(needle);
    if (cfg_loaded) config_free(&cfg);
    return rc;
}
