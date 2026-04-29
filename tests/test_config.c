/*
 * test_config.c — config loader behaviour.
 *
 * Behaviours under test (≥ 6 assertions):
 *   1. NULL path returns defaults; cap_bytes = 12 GiB.
 *   2. Missing path returns defaults (no error).
 *   3. config/nocturne.toml.example parses cleanly.
 *   4. Bucket parsing: count, weight, source, window_days from buckets.recent_adds.
 *   5. [cap].bytes / effective_ratio override defaults.
 *   6. Malformed config returns -1.
 *   7. Default count for `manual_pins` bucket is 200.
 *   8. Hysteresis ratio defaults to 0.10.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "runner.h"

static const struct bucket_config *find_bucket(const struct nocturne_config *c,
                                               const char *name)
{
    for (size_t i = 0; i < c->buckets_n; i++) {
        if (!strcmp(c->buckets[i].name, name)) return &c->buckets[i];
    }
    return NULL;
}

static char *write_tmp(const char *content)
{
    char tmpl[] = "/tmp/nocturne-cfg-XXXXXX.toml";
    int fd = mkstemps(tmpl, 5);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "w");
    fputs(content, f);
    fclose(f);
    return strdup(tmpl);
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* 1. NULL path → defaults. */
    {
        struct nocturne_config c;
        int rc = config_load(NULL, &c);
        expect(rc == 0, "config_load(NULL) returns 0");
        expect(c.cap_bytes == 12LL * 1024 * 1024 * 1024,
               "default cap_bytes = 12 GiB");
        expect(c.cap_effective_ratio == 0.70,
               "default cap_effective_ratio = 0.70");
        expect(c.hysteresis_ratio == 0.10,
               "default hysteresis_ratio = 0.10");
        expect(c.cold_start_play_threshold == 10,
               "default cold_start_play_threshold = 10");
        const struct bucket_config *pins = find_bucket(&c, "manual_pins");
        expect(pins != NULL && pins->count == 200,
               "default manual_pins bucket count = 200");
        expect(c.buckets_n == 7, "seven default buckets present");
        config_free(&c);
    }

    /* 2. Missing path → defaults (no error). */
    {
        struct nocturne_config c;
        int rc = config_load("/tmp/nocturne-this-does-not-exist", &c);
        expect(rc == 0, "missing config file returns 0 (defaults)");
        expect(c.buckets_n == 7, "missing-path: 7 default buckets");
        config_free(&c);
    }

    /* 3. Example file. */
    {
        struct nocturne_config c;
        int rc = config_load("config/nocturne.toml.example", &c);
        expect(rc == 0, "config/nocturne.toml.example parses cleanly");
        const struct bucket_config *ra = find_bucket(&c, "recent_adds");
        expect(ra != NULL && ra->count == 80 && ra->window_days == 14,
               "recent_adds bucket: count=80 window_days=14");
        config_free(&c);
    }

    /* 4. Custom override file. */
    {
        char *path = write_tmp(
            "[cap]\n"
            "bytes = 1073741824\n"
            "effective_ratio = 0.5\n"
            "[resolver]\n"
            "hysteresis = 0.2\n"
            "[buckets.recent_adds]\n"
            "count = 5\n"
            "weight = 0.123\n"
            "source = \"custom_source\"\n"
            "window_days = 7\n"
        );
        struct nocturne_config c;
        int rc = config_load(path, &c);
        expect(rc == 0, "override config parses cleanly");
        expect(c.cap_bytes == 1073741824, "override: cap_bytes = 1 GiB");
        expect(c.cap_effective_ratio == 0.5, "override: effective_ratio = 0.5");
        expect(c.hysteresis_ratio == 0.2, "override: hysteresis = 0.2");
        const struct bucket_config *ra = find_bucket(&c, "recent_adds");
        expect(ra != NULL && ra->count == 5,
               "override: recent_adds count = 5");
        expect(ra && ra->weight > 0.122 && ra->weight < 0.124,
               "override: recent_adds weight = 0.123");
        expect(ra && ra->source && !strcmp(ra->source, "custom_source"),
               "override: recent_adds source replaced");
        config_free(&c);
        unlink(path); free(path);
    }

    /* 6. Malformed file. */
    {
        char *path = write_tmp("[unterminated\n");
        struct nocturne_config c;
        int rc = config_load(path, &c);
        expect(rc == -1, "malformed section header returns -1");
        config_free(&c);
        unlink(path); free(path);
    }

    return test_finish(__FILE__);
}
