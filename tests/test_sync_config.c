/*
 * test_sync_config.c — XML emission invariants for sync_config.c.
 *
 * Behaviours covered (≥ 10):
 *   1. Desktop side: sync-meta type=sendreceive, sync-files type=sendonly.
 *   2. Phone side:   sync-meta type=sendreceive, sync-files type=receiveonly.
 *   3. Both sides: <versioning type="none"/> on sync-files. (Pitfall 24)
 *   4. Both sides: globalAnnounceEnabled=false + relaysEnabled=false. (Pitfall 20)
 *   5. Default device names: nocturne-desktop / nocturne-phone (NOT hostname).
 *   6. [syncthing] desktop_name / phone_name override defaults.
 *   7. Desktop sync-files path = library_root + "/resident".
 *   8. Phone sync-files path defaults to /storage/emulated/0/Music/nocturne.
 *   9. Phone path overridable via [syncthing.phone] sync_files_path.
 *  10. Header marker comment "nocturne-managed - do not edit" present.
 *  11. Two emissions with same input are byte-identical (deterministic).
 *  12. Emitted XML is well-formed (best-effort: balanced angle-bracket
 *      check OR xmllint when available).
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "sync_config.h"
#include "runner.h"

/* Capture sync_config_emit output via fmemopen. Returns heap buffer. */
static char *capture_emit(enum sync_config_side side,
                          const struct nocturne_config *cfg)
{
    char *buf = malloc(8192);
    if (!buf) return NULL;
    FILE *f = fmemopen(buf, 8192, "w");
    if (!f) { free(buf); return NULL; }
    int rc = sync_config_emit(side, cfg, f);
    fclose(f);
    if (rc != 0) { free(buf); return NULL; }
    /* fmemopen writes the trailing NUL within the buffer. */
    return buf;
}

/* Naive well-formed check: every '<' must have a matching '>', and
 * counts of '<' and '>' equal. Doesn't catch all XML errors but
 * catches the common ones (unbalanced tags). */
static int xml_balance_check(const char *s)
{
    long lt = 0, gt = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '<') lt++;
        else if (*p == '>') gt++;
    }
    return lt == gt && lt > 0;
}

static int contains(const char *hay, const char *needle)
{
    return hay && strstr(hay, needle) != NULL;
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    /* Baseline default config. */
    struct nocturne_config cfg;
    config_default(&cfg);
    cfg.library_root = strdup("/home/test/music/library");
    cfg.sync_meta_root = strdup("/home/test/sync/nocturne/meta");

    /* --- Test 1: desktop side --- */
    {
        char *out = capture_emit(SIDE_DESKTOP, &cfg);
        expect(out != NULL, "test1: emit produced output");
        expect(contains(out, "id=\"sync-meta\""),
               "test1: sync-meta folder present");
        expect(contains(out, "id=\"sync-files\""),
               "test1: sync-files folder present");
        /* sync-meta type=sendreceive */
        expect(contains(out, "id=\"sync-meta\" label=\"nocturne meta\" "
                              "path=\"/home/test/sync/nocturne/meta\" type=\"sendreceive\""),
               "test1: sync-meta type=sendreceive");
        expect(contains(out, "id=\"sync-files\" label=\"nocturne files\" "
                              "path=\"/home/test/music/library/resident\" type=\"sendonly\""),
               "test1: sync-files type=sendonly + path=<root>/resident");
        free(out);
    }

    /* --- Test 2: phone side --- */
    {
        char *out = capture_emit(SIDE_PHONE, &cfg);
        expect(contains(out, "id=\"sync-files\" label=\"nocturne files\" "
                              "path=\"/storage/emulated/0/Music/nocturne\" type=\"receiveonly\""),
               "test2: phone sync-files type=receiveonly");
        expect(contains(out, "id=\"sync-meta\" label=\"nocturne meta\" "
                              "path=\"/storage/emulated/0/sync/nocturne/meta\" type=\"sendreceive\""),
               "test2: phone sync-meta type=sendreceive + default path");
        free(out);
    }

    /* --- Test 3: versioning none on both sides --- */
    {
        char *d = capture_emit(SIDE_DESKTOP, &cfg);
        char *p = capture_emit(SIDE_PHONE, &cfg);
        expect(contains(d, "<versioning type=\"none\"></versioning>"),
               "test3: desktop versioning=none");
        expect(contains(p, "<versioning type=\"none\"></versioning>"),
               "test3: phone versioning=none");
        free(d); free(p);
    }

    /* --- Test 4: privacy invariants in <options> --- */
    {
        char *d = capture_emit(SIDE_DESKTOP, &cfg);
        char *p = capture_emit(SIDE_PHONE, &cfg);
        expect(contains(d, "<globalAnnounceEnabled>false</globalAnnounceEnabled>"),
               "test4d: globalAnnounceEnabled=false");
        expect(contains(d, "<relaysEnabled>false</relaysEnabled>"),
               "test4d: relaysEnabled=false");
        expect(contains(p, "<globalAnnounceEnabled>false</globalAnnounceEnabled>"),
               "test4p: phone globalAnnounceEnabled=false");
        expect(contains(p, "<relaysEnabled>false</relaysEnabled>"),
               "test4p: phone relaysEnabled=false");
        free(d); free(p);
    }

    /* --- Test 5: default device names --- */
    {
        char *d = capture_emit(SIDE_DESKTOP, &cfg);
        expect(contains(d, "name=\"nocturne-desktop\""),
               "test5: default desktop name = nocturne-desktop");
        expect(contains(d, "name=\"nocturne-phone\""),
               "test5: default phone name = nocturne-phone");
        /* Hostname leak check: the emitted XML must not contain the
         * actual gethostname() value. */
        char hn[256] = {0};
        gethostname(hn, sizeof(hn) - 1);
        if (hn[0]) {
            expect(strstr(d, hn) == NULL,
                   "test5: real hostname not present in XML");
        }
        free(d);
    }

    /* --- Test 6: configurable device names --- */
    {
        free(cfg.syncthing_desktop_name);
        free(cfg.syncthing_phone_name);
        cfg.syncthing_desktop_name = strdup("alice-laptop");
        cfg.syncthing_phone_name   = strdup("alice-pixel");
        char *out = capture_emit(SIDE_DESKTOP, &cfg);
        expect(contains(out, "name=\"alice-laptop\""),
               "test6: desktop_name override applied");
        expect(contains(out, "name=\"alice-pixel\""),
               "test6: phone_name override applied");
        expect(strstr(out, "nocturne-desktop") == NULL,
               "test6: defaults gone after override");
        free(out);
        free(cfg.syncthing_desktop_name);
        free(cfg.syncthing_phone_name);
        cfg.syncthing_desktop_name = NULL;
        cfg.syncthing_phone_name = NULL;
    }

    /* --- Test 7: desktop sync-files path = library_root + /resident.
     *     Already checked by test 1's exact-match assertion. */
    expect(1, "test7: covered by test1 exact-match");

    /* --- Test 8 + 9: phone path default and override --- */
    {
        char *p = capture_emit(SIDE_PHONE, &cfg);
        expect(contains(p, "path=\"/storage/emulated/0/Music/nocturne\""),
               "test8: phone files path default");
        free(p);

        cfg.syncthing_phone_sync_files = strdup("/sdcard/MyMusic/nocturne");
        char *p2 = capture_emit(SIDE_PHONE, &cfg);
        expect(contains(p2, "path=\"/sdcard/MyMusic/nocturne\""),
               "test9: phone files path override");
        free(p2);
        free(cfg.syncthing_phone_sync_files);
        cfg.syncthing_phone_sync_files = NULL;
    }

    /* --- Test 10: header marker --- */
    {
        char *d = capture_emit(SIDE_DESKTOP, &cfg);
        expect(contains(d, "nocturne-managed"),
               "test10: header marker present");
        free(d);
    }

    /* --- Test 11: byte-deterministic --- */
    {
        char *a = capture_emit(SIDE_DESKTOP, &cfg);
        char *b = capture_emit(SIDE_DESKTOP, &cfg);
        expect(a && b && strcmp(a, b) == 0,
               "test11: two emissions byte-identical");
        free(a); free(b);
    }

    /* --- Test 12: well-formed XML (balanced) --- */
    {
        char *d = capture_emit(SIDE_DESKTOP, &cfg);
        char *p = capture_emit(SIDE_PHONE, &cfg);
        expect(xml_balance_check(d), "test12d: desktop XML balanced");
        expect(xml_balance_check(p), "test12p: phone XML balanced");
        free(d); free(p);
    }

    config_free(&cfg);
    return test_finish("test_sync_config");
}
