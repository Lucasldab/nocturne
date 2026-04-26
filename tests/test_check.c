/*
 * test_check.c — schema check + multi-value heuristic tests.
 *
 * Loads every fixture from $1 (default tests/fixtures), runs
 * check_canonical on it, and asserts the expected pass/fail/flag pattern.
 *
 * Linked against the same .o files as the production binary; no mocks.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <taglib/tag_c.h>

#include "check.h"
#include "tags.h"
#include "runner.h"

/* Helper: locate a fixture path by joining $fixdir + "/" + $name. Returns
 * a heap-allocated path the caller frees. Returns NULL on OOM. */
static char *fix_path(const char *fixdir, const char *name)
{
    size_t n = strlen(fixdir) + 1 + strlen(name) + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s/%s", fixdir, name);
    return p;
}

static int file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Walk cr->issues looking for an issue with the given code (and optional
 * field if field_or_negative >= 0). Returns true if found. */
static bool issue_present(const struct check_result *cr,
                          const char *code,
                          int field_or_negative)
{
    if (!cr || !cr->issues || !code) return false;
    for (size_t i = 0; i < cr->issue_count; i++) {
        if (cr->issues[i].code && strcmp(cr->issues[i].code, code) == 0) {
            if (field_or_negative < 0) return true;
            if ((int)cr->issues[i].field == field_or_negative) return true;
        }
    }
    return false;
}

/* Count CHECK_FAIL issues with code "missing_field". */
static size_t count_missing_fields(const struct check_result *cr)
{
    if (!cr) return 0;
    size_t n = 0;
    for (size_t i = 0; i < cr->issue_count; i++) {
        if (cr->issues[i].severity == CHECK_FAIL &&
            cr->issues[i].code &&
            strcmp(cr->issues[i].code, "missing_field") == 0) {
            n++;
        }
    }
    return n;
}

/* Load fixture, run check, run assertions via callback. Frees everything. */
static void run_one(const char *fixdir, const char *fname,
                    void (*on_loaded)(const char *fname,
                                      const struct check_result *cr))
{
    char *path = fix_path(fixdir, fname);
    if (!path) return;
    if (!file_exists(path)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "fixture %s present", fname);
        expect(false, buf);
        free(path);
        return;
    }
    struct tag_record *rec = tags_extract(path);
    struct check_result cr = {0};
    check_canonical(rec, &cr);
    on_loaded(fname, &cr);
    check_result_free(&cr);
    tag_record_free(rec);
    free(path);
}

/* Per-fixture assertions. */

static void check_clean_id3v24(const char *fname, const struct check_result *cr)
{
    (void)fname;
    expect(!cr->any_fail && !cr->any_flag,
           "clean v2.4 mp3 passes (no fail, no flag)");
}

static void check_dirty_id3v23(const char *fname, const struct check_result *cr)
{
    (void)fname;
    expect(cr->any_fail, "v2.3 mp3 fails canonical schema");
    expect(issue_present(cr, "id3_not_v24", -1),
           "v2.3 mp3 carries id3_not_v24 issue");
}

static void check_no_id3(const char *fname, const struct check_result *cr)
{
    (void)fname;
    expect(cr->any_fail, "no-tag mp3 fails canonical schema");
    /* ffmpeg writes an empty ID3v2.4 header even with -map_metadata -1, so
     * we don't assert id3_not_v24 here — the observable invariant is that
     * every canonical field is missing. */
    expect(count_missing_fields(cr) >= 4,
           "no-tag mp3 has at least 4 missing_field issues");
}

static void check_missing_album_artist(const char *fname,
                                       const struct check_result *cr)
{
    (void)fname;
    expect(cr->any_fail,
           "FLAC missing album_artist fails canonical schema");
    expect(issue_present(cr, "missing_field", FIELD_ALBUM_ARTIST),
           "FLAC missing album_artist reports the album_artist field");
}

static void check_clean_all_flac(const char *fname,
                                 const struct check_result *cr)
{
    (void)fname;
    expect(!cr->any_fail && !cr->any_flag,
           "clean flac passes (no fail, no flag)");
}

static void check_multi_value_canonical(const char *fname,
                                        const struct check_result *cr)
{
    (void)fname;
    /* If opustags is unavailable on this host, the fixture has only a
     * single ARTIST/GENRE entry — see tests/fixtures/.skip-multi-value-canonical
     * marker file. In that case we relax the assertion: the file should at
     * minimum still PASS schema (single-artist Opus is valid). */
    bool skip_marker = file_exists("tests/fixtures/.skip-multi-value-canonical");
    if (skip_marker) {
        expect(!cr->any_fail,
               "canonical-multi-value opus passes schema (single-value fallback "
               "due to missing opustags)");
    } else {
        expect(!cr->any_fail && !cr->any_flag,
               "canonical-multi-value opus passes without flag");
    }
}

static void check_concat_semicolon(const char *fname,
                                   const struct check_result *cr)
{
    (void)fname;
    expect(!cr->any_fail && cr->any_flag,
           "concat-semicolon opus is FLAGged not FAILed");
    expect(issue_present(cr, "concat_multi_value", FIELD_ARTIST),
           "concat-semicolon opus carries concat_multi_value on artist");
}

static void check_acdc_legit(const char *fname, const struct check_result *cr)
{
    (void)fname;
    expect(!cr->any_fail && cr->any_flag,
           "AC/DC slash flagged (known false-positive); never failed");
    expect(issue_present(cr, "concat_multi_value", -1),
           "AC/DC carries concat_multi_value flag");
}

static void check_broken_audio(const char *fname, const struct check_result *cr)
{
    (void)fname;
    /* TagLib 2.x is permissive: random bytes named .mp3 may or may not
     * trigger taglib_file_is_valid()=false. We assert the observable
     * invariant instead: the file MUST fail the canonical schema. */
    expect(cr->any_fail, "broken-audio mp3 fails canonical schema");
}

int main(int argc, char **argv)
{
    const char *fixdir = (argc > 1) ? argv[1] : "tests/fixtures";

    taglib_set_strings_unicode((BOOL)1);

    /* Per-fixture suite. */
    run_one(fixdir, "clean_id3v24.mp3",         check_clean_id3v24);
    run_one(fixdir, "dirty_id3v23.mp3",         check_dirty_id3v23);
    run_one(fixdir, "no_id3.mp3",               check_no_id3);
    run_one(fixdir, "missing_album_artist.flac", check_missing_album_artist);
    run_one(fixdir, "clean_all.flac",           check_clean_all_flac);
    run_one(fixdir, "multi_value_canonical.opus", check_multi_value_canonical);
    run_one(fixdir, "concat_semicolon.opus",    check_concat_semicolon);
    run_one(fixdir, "acdc_legit.opus",          check_acdc_legit);
    run_one(fixdir, "broken_audio.mp3",         check_broken_audio);

    /* Pure-function tests for the heuristic. */
    expect(check_looks_concatenated_multi_value("Solo Artist") == false,
           "no separator -> not flagged");
    expect(check_looks_concatenated_multi_value("A; B") == true,
           "semicolon -> flagged");
    expect(check_looks_concatenated_multi_value("A & B") == true,
           "ampersand-with-spaces -> flagged");
    expect(check_looks_concatenated_multi_value("A and B") == true,
           "case-insensitive 'and' -> flagged");
    expect(check_looks_concatenated_multi_value("A AND B") == true,
           "uppercase AND -> flagged");
    expect(check_looks_concatenated_multi_value("A feat. B") == true,
           "feat. -> flagged");
    expect(check_looks_concatenated_multi_value("A FEAT. B") == true,
           "case-insensitive FEAT. -> flagged");
    expect(check_looks_concatenated_multi_value("Hand") == false,
           "embedded 'and' inside word not flagged (requires surrounding spaces)");
    expect(check_looks_concatenated_multi_value("") == false,
           "empty string -> not flagged");
    expect(check_looks_concatenated_multi_value(NULL) == false,
           "NULL -> not flagged");
    expect(check_looks_concatenated_multi_value("AC/DC") == true,
           "slash -> flagged (AC/DC false-positive — locked behaviour)");

    return test_finish("test_check");
}
