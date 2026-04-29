/*
 * config.c — minimal INI/TOML-subset parser for nocturne config.toml.
 *
 * Pre-approved deviation from plan 02-05: we vendor a hand-rolled parser
 * instead of toml-c. We only need:
 *   - Top-level [section] headers.
 *   - Nested [buckets.<name>] subsections (the dot-path becomes a flat key).
 *   - key = value lines with: integer, float, double-quoted string.
 *   - Comments starting with `#` or `;` (anywhere on a line).
 *   - Empty lines.
 *
 * Anything fancier (arrays, inline tables, multi-line strings, datetime)
 * we don't use, so we don't parse.
 *
 * Defaults (when file is missing) describe the six canonical buckets the
 * resolver consumes; users override via the example config.
 */

#define _GNU_SOURCE

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* === defaults ============================================================ */

struct bucket_seed {
    const char *name;
    int count;
    const char *source;
    double weight;
    int window_days;
};

static const struct bucket_seed DEFAULT_BUCKETS[] = {
    { "recent_adds",       80, "recent_adds_by_mtime",   1.00, 14 },
    { "top_played",        80, "top_played_phone",       0.90, 90 },
    { "recent_plays",      60, "recent_plays_phone",     0.85, 30 },
    { "loved",            100, "loved",                  1.00, 0  },
    { "exploration",       40, "exploration_random",     0.50, 0  },
    { "manual_pins",      200, "manual_pins",            1.00, 0  },
    { "weekly_discovery",  20, "weekly_discovery_picks", 0.95, 0  },
};

static const size_t DEFAULT_BUCKETS_N =
    sizeof(DEFAULT_BUCKETS) / sizeof(DEFAULT_BUCKETS[0]);

int config_default(struct nocturne_config *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->cap_bytes              = 12LL * 1024 * 1024 * 1024;  /* 12 GiB */
    out->cap_effective_ratio    = 0.70;
    out->hysteresis_ratio       = 0.10;
    out->cold_start_play_threshold = 10;
    out->random_seed            = 0;

    out->buckets = calloc(DEFAULT_BUCKETS_N, sizeof(*out->buckets));
    if (!out->buckets) return -1;
    out->buckets_n = DEFAULT_BUCKETS_N;
    for (size_t i = 0; i < DEFAULT_BUCKETS_N; i++) {
        out->buckets[i].name        = strdup(DEFAULT_BUCKETS[i].name);
        out->buckets[i].count       = DEFAULT_BUCKETS[i].count;
        out->buckets[i].source      = strdup(DEFAULT_BUCKETS[i].source);
        out->buckets[i].weight      = DEFAULT_BUCKETS[i].weight;
        out->buckets[i].window_days = DEFAULT_BUCKETS[i].window_days;
        if (!out->buckets[i].name || !out->buckets[i].source) {
            config_free(out);
            return -1;
        }
    }
    return 0;
}

void config_free(struct nocturne_config *c)
{
    if (!c) return;
    free(c->library_root);
    free(c->sync_meta_root);
    free(c->syncthing_desktop_name);
    free(c->syncthing_phone_name);
    free(c->syncthing_phone_sync_files);
    free(c->syncthing_phone_sync_meta);
    free(c->syncthing_desktop_device_id);
    free(c->syncthing_phone_device_id);
    free(c->transcode_format);
    free(c->discover_exclude_album_substrings);
    free(c->listenbrainz_username);
    free(c->listenbrainz_user_token);
    for (size_t i = 0; i < c->buckets_n; i++) {
        free(c->buckets[i].name);
        free(c->buckets[i].source);
    }
    free(c->buckets);
    memset(c, 0, sizeof(*c));
}

/* === parser ============================================================== */

/* Trim leading/trailing whitespace in place by adjusting pointers and
 * NUL-terminating. Returns the new start. */
static char *trim(char *s)
{
    if (!s) return s;
    while (*s && isspace((unsigned char) *s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char) *(end - 1))) end--;
    *end = '\0';
    return s;
}

/* Strip a comment starting with '#' or ';' (ignoring those inside quoted
 * strings). Modifies in place. */
static void strip_comment(char *s)
{
    int in_str = 0;
    for (char *p = s; *p; p++) {
        if (*p == '"' && (p == s || *(p - 1) != '\\')) in_str = !in_str;
        else if (!in_str && (*p == '#' || *p == ';')) { *p = '\0'; return; }
    }
}

/* Parse an int64 from `s`. Returns 0 on success, -1 on invalid. */
static int parse_ll(const char *s, long long *out)
{
    if (!s || !*s) return -1;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || end == s) return -1;
    while (end && *end && isspace((unsigned char) *end)) end++;
    if (end && *end != '\0') return -1;
    *out = v;
    return 0;
}

static int parse_dbl(const char *s, double *out)
{
    if (!s || !*s) return -1;
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s) return -1;
    while (end && *end && isspace((unsigned char) *end)) end++;
    if (end && *end != '\0') return -1;
    *out = v;
    return 0;
}

/* Strip a single layer of "..." and unescape \" and \\. Returns
 * heap-allocated unquoted string or NULL on error. */
static char *parse_string(const char *s)
{
    if (!s || s[0] != '"') return NULL;
    size_t n = strlen(s);
    if (n < 2 || s[n - 1] != '"') return NULL;
    char *out = malloc(n - 1);  /* worst case: same length minus surrounding quotes */
    if (!out) return NULL;
    char *o = out;
    for (size_t i = 1; i < n - 1; i++) {
        if (s[i] == '\\' && i + 1 < n - 1) {
            char c = s[i + 1];
            switch (c) {
            case '"':  *o++ = '"'; break;
            case '\\': *o++ = '\\'; break;
            case 'n':  *o++ = '\n'; break;
            case 't':  *o++ = '\t'; break;
            default:   *o++ = c; break;
            }
            i++;
        } else {
            *o++ = s[i];
        }
    }
    *o = '\0';
    return out;
}

static struct bucket_config *find_or_add_bucket(struct nocturne_config *c,
                                                const char *name)
{
    for (size_t i = 0; i < c->buckets_n; i++) {
        if (c->buckets[i].name && !strcmp(c->buckets[i].name, name)) {
            return &c->buckets[i];
        }
    }
    struct bucket_config *fresh = realloc(c->buckets,
                                          (c->buckets_n + 1) * sizeof(*fresh));
    if (!fresh) return NULL;
    c->buckets = fresh;
    memset(&c->buckets[c->buckets_n], 0, sizeof(c->buckets[c->buckets_n]));
    c->buckets[c->buckets_n].name = strdup(name);
    if (!c->buckets[c->buckets_n].name) return NULL;
    return &c->buckets[c->buckets_n++];
}

/* Apply a key=value to the right field given the current section path.
 * Section path examples: "library", "cap", "buckets.recent_adds",
 * "resolver". Returns 0 on success, -1 on error. */
static int apply_kv(struct nocturne_config *c,
                    const char *section,
                    const char *key,
                    const char *raw_value,
                    int line_no)
{
    /* String first — used in multiple branches. */
    char *as_str = NULL;
    if (raw_value && raw_value[0] == '"') {
        as_str = parse_string(raw_value);
        if (!as_str) {
            fprintf(stderr, "config:%d: malformed string value for %s.%s\n",
                    line_no, section, key);
            return -1;
        }
    }

    long long as_ll = 0;
    double as_dbl = 0.0;
    int have_ll = (parse_ll(raw_value, &as_ll) == 0);
    int have_dbl = (parse_dbl(raw_value, &as_dbl) == 0);

    if (!strcmp(section, "library")) {
        if (!strcmp(key, "path")) {
            free(c->library_root);
            c->library_root = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
    } else if (!strcmp(section, "sync_meta")) {
        if (!strcmp(key, "path")) {
            free(c->sync_meta_root);
            c->sync_meta_root = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
    } else if (!strcmp(section, "syncthing")) {
        if (!strcmp(key, "desktop_name")) {
            free(c->syncthing_desktop_name);
            c->syncthing_desktop_name = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
        if (!strcmp(key, "phone_name")) {
            free(c->syncthing_phone_name);
            c->syncthing_phone_name = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
        if (!strcmp(key, "desktop_device_id")) {
            free(c->syncthing_desktop_device_id);
            c->syncthing_desktop_device_id = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
        if (!strcmp(key, "phone_device_id")) {
            free(c->syncthing_phone_device_id);
            c->syncthing_phone_device_id = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
    } else if (!strcmp(section, "syncthing.phone")) {
        if (!strcmp(key, "sync_files_path")) {
            free(c->syncthing_phone_sync_files);
            c->syncthing_phone_sync_files = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
        if (!strcmp(key, "sync_meta_path")) {
            free(c->syncthing_phone_sync_meta);
            c->syncthing_phone_sync_meta = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
    } else if (!strcmp(section, "cap")) {
        if (!strcmp(key, "bytes")) {
            if (!have_ll) goto fail;
            c->cap_bytes = as_ll;
            free(as_str); return 0;
        }
        if (!strcmp(key, "effective_ratio")) {
            if (!have_dbl) goto fail;
            c->cap_effective_ratio = as_dbl;
            free(as_str); return 0;
        }
    } else if (!strcmp(section, "resolver")) {
        if (!strcmp(key, "hysteresis")) {
            if (!have_dbl) goto fail;
            c->hysteresis_ratio = as_dbl;
            free(as_str); return 0;
        }
        if (!strcmp(key, "cold_start_play_threshold")) {
            if (!have_ll) goto fail;
            c->cold_start_play_threshold = (int) as_ll;
            free(as_str); return 0;
        }
        if (!strcmp(key, "seed")) {
            if (!have_ll) goto fail;
            c->random_seed = (unsigned long) as_ll;
            free(as_str); return 0;
        }
    } else if (!strcmp(section, "transcode")) {
        if (!strcmp(key, "enabled")) {
            if (!have_ll) goto fail;
            c->transcode_enabled = (as_ll != 0);
            free(as_str); return 0;
        }
        if (!strcmp(key, "format")) {
            free(c->transcode_format);
            c->transcode_format = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
        if (!strcmp(key, "bitrate_kbps")) {
            if (!have_ll) goto fail;
            if (as_ll < 32) as_ll = 32;
            if (as_ll > 510) as_ll = 510;
            c->transcode_bitrate_kbps = (int) as_ll;
            free(as_str); return 0;
        }
    } else if (!strcmp(section, "discover")) {
        if (!strcmp(key, "exclude_album_substrings")) {
            free(c->discover_exclude_album_substrings);
            c->discover_exclude_album_substrings = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
    } else if (!strcmp(section, "listenbrainz")) {
        if (!strcmp(key, "username")) {
            free(c->listenbrainz_username);
            c->listenbrainz_username = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
        if (!strcmp(key, "user_token")) {
            free(c->listenbrainz_user_token);
            c->listenbrainz_user_token = as_str ? as_str : NULL;
            if (!as_str) goto fail;
            return 0;
        }
    } else if (!strncmp(section, "buckets.", 8)) {
        const char *bn = section + 8;
        struct bucket_config *b = find_or_add_bucket(c, bn);
        if (!b) goto fail;
        if (!strcmp(key, "count")) {
            if (!have_ll) goto fail;
            /* Clamp at 1e6 (T-02-05-05 mitigation). */
            if (as_ll < 0) as_ll = 0;
            if (as_ll > 1000000) as_ll = 1000000;
            b->count = (int) as_ll;
            free(as_str); return 0;
        }
        if (!strcmp(key, "weight")) {
            if (!have_dbl) goto fail;
            b->weight = as_dbl;
            free(as_str); return 0;
        }
        if (!strcmp(key, "window_days")) {
            if (!have_ll) goto fail;
            b->window_days = (int) as_ll;
            free(as_str); return 0;
        }
        if (!strcmp(key, "source")) {
            if (!as_str) goto fail;
            free(b->source);
            b->source = as_str;
            return 0;
        }
    }

    /* Unknown key: warn but don't fail. */
    fprintf(stderr, "config:%d: ignoring unknown key %s.%s\n",
            line_no, section, key);
    free(as_str);
    return 0;

fail:
    fprintf(stderr, "config:%d: invalid value for %s.%s\n",
            line_no, section, key);
    free(as_str);
    return -1;
}

int config_load(const char *path, struct nocturne_config *out)
{
    if (config_default(out) != 0) return -1;
    if (!path) return 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;  /* missing is not an error */
        fprintf(stderr, "config_load: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char section[128] = {0};
    char buf[1024];
    int line_no = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line_no++;
        strip_comment(buf);
        char *line = trim(buf);
        if (!*line) continue;

        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (!close) {
                fprintf(stderr, "config:%d: malformed section header\n", line_no);
                fclose(f);
                return -1;
            }
            *close = '\0';
            char *name = trim(line + 1);
            if (strlen(name) >= sizeof(section)) {
                fprintf(stderr, "config:%d: section name too long\n", line_no);
                fclose(f);
                return -1;
            }
            strcpy(section, name);
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "config:%d: expected key = value\n", line_no);
            fclose(f);
            return -1;
        }
        *eq = '\0';
        char *key = trim(line);
        char *val = trim(eq + 1);
        if (!*section) {
            fprintf(stderr, "config:%d: key '%s' outside any [section]\n", line_no, key);
            fclose(f);
            return -1;
        }
        if (apply_kv(out, section, key, val, line_no) != 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}
