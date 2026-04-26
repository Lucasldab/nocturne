/*
 * test_quarantine.c — quarantine module tests.
 *
 * Each test creates a tmp library + tmp quarantine via mkdtemp(3), copies
 * a fixture in, runs the move, asserts file moved/log written/etc., and
 * cleans up. Tests are isolated.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "check.h"
#include "quarantine.h"
#include "tags.h"
#include "runner.h"

/* Recursive rm — only used on tmpdirs we created via mkdtemp. Safety: the
 * caller must guarantee `path` starts with /tmp/. */
static int rm_rf(const char *path)
{
    if (!path || strncmp(path, "/tmp/", 5) != 0) return -1;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", path);
    return system(cmd);  /* test-only — explicitly limited to /tmp/ */
}

/* Allocate a unique tmpdir under /tmp. Caller frees. Returns NULL on
 * failure. */
static char *make_tmpdir(const char *tag)
{
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-test-%s-XXXXXX", tag);
    char *p = strdup(tmpl);
    if (!p) return NULL;
    if (!mkdtemp(p)) {
        free(p);
        return NULL;
    }
    return p;
}

/* Copy `src` to `dst` byte-for-byte. */
static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192];
    size_t n;
    int ret = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ret = -1; break; }
    }
    fclose(in);
    fclose(out);
    return ret;
}

static int file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Build a fake check_result with a single FAIL issue, given a path string
 * (we strdup it into a fake tag_record and synthesise the result). Caller
 * must call cleanup_fake_cr() afterwards. */
static void make_fake_cr_with_fail(struct tag_record **rec_out,
                                   struct check_result *cr,
                                   const char *path)
{
    struct tag_record *rec = calloc(1, sizeof(*rec));
    rec->path = strdup(path);
    *rec_out = rec;

    memset(cr, 0, sizeof(*cr));
    cr->rec = rec;
    cr->any_fail = true;
    /* Add one synthesised issue so reasons_join has something. */
    cr->issue_capacity = 1;
    cr->issue_count = 1;
    cr->issues = calloc(1, sizeof(*cr->issues));
    cr->issues[0].severity = CHECK_FAIL;
    cr->issues[0].field = FIELD_COUNT;
    cr->issues[0].code = "test_fail";
    cr->issues[0].detail = strdup("synthesised fail for test");
}

static void cleanup_fake_cr(struct tag_record *rec, struct check_result *cr)
{
    if (cr) {
        if (cr->issues) {
            for (size_t i = 0; i < cr->issue_count; i++) {
                free(cr->issues[i].detail);
            }
            free(cr->issues);
        }
        cr->issues = NULL;
    }
    if (rec) {
        free(rec->path);
        free(rec);
    }
}

/* === Tests === */

/* 1. happy path: real-run quarantines a file. */
static void test_happy_path(const char *fixdir)
{
    (void)fixdir;
    char *libdir = make_tmpdir("happy-lib");
    char *qdir   = make_tmpdir("happy-q");
    if (!libdir || !qdir) {
        expect(0, "happy: tmpdirs allocated");
        free(libdir); free(qdir);
        return;
    }

    char src[512];
    snprintf(src, sizeof(src), "%s/track.bin", libdir);
    /* Use copy_file to plant a small file (any bytes work — quarantine
     * doesn't read the file content). */
    FILE *f = fopen(src, "wb");
    if (f) { fputs("dummy", f); fclose(f); }

    struct tag_record *rec;
    struct check_result cr;
    make_fake_cr_with_fail(&rec, &cr, src);

    struct quarantine_ctx qctx;
    int rc = quarantine_init(&qctx, libdir, qdir, false);
    expect(rc == 0, "happy: quarantine_init succeeds");

    rc = quarantine_move(&qctx, &cr);
    expect(rc == 0, "happy: quarantine_move returns 0");
    expect(qctx.moved_count == 1, "happy: moved_count == 1");

    /* src should no longer exist. */
    expect(!file_exists(src), "happy: source removed from library");

    /* target should exist (basename: track.bin since it sat directly under libdir). */
    char target[1024];
    snprintf(target, sizeof(target), "%s/track.bin", qdir);
    expect(file_exists(target), "happy: target appeared in quarantine");

    /* log line written. */
    quarantine_close(&qctx);
    char logp[512];
    snprintf(logp, sizeof(logp), "%s/quarantine.log", qdir);
    expect(file_exists(logp), "happy: quarantine.log written");

    cleanup_fake_cr(rec, &cr);
    rm_rf(libdir);
    rm_rf(qdir);
    free(libdir); free(qdir);
}

/* 2. dry-run: no filesystem mutation. */
static void test_dry_run(void)
{
    char *libdir = make_tmpdir("dry-lib");
    char *qdir   = make_tmpdir("dry-q");
    if (!libdir || !qdir) {
        expect(0, "dry: tmpdirs allocated");
        free(libdir); free(qdir);
        return;
    }

    char src[512];
    snprintf(src, sizeof(src), "%s/track.bin", libdir);
    FILE *f = fopen(src, "wb"); if (f) { fputs("dummy", f); fclose(f); }

    struct tag_record *rec;
    struct check_result cr;
    make_fake_cr_with_fail(&rec, &cr, src);

    struct quarantine_ctx qctx;
    int rc = quarantine_init(&qctx, libdir, qdir, true /* dry-run */);
    expect(rc == 0, "dry: quarantine_init succeeds");
    rc = quarantine_move(&qctx, &cr);
    expect(rc == 0, "dry: quarantine_move returns 0");
    expect(qctx.moved_count == 1, "dry: moved_count == 1 (would-move)");

    /* src still exists, target does NOT. */
    expect(file_exists(src), "dry: source still in library");
    char target[1024];
    snprintf(target, sizeof(target), "%s/track.bin", qdir);
    expect(!file_exists(target), "dry: target NOT created in quarantine");

    /* No log file in dry-run. */
    quarantine_close(&qctx);
    char logp[512];
    snprintf(logp, sizeof(logp), "%s/quarantine.log", qdir);
    expect(!file_exists(logp), "dry: quarantine.log NOT written");

    cleanup_fake_cr(rec, &cr);
    rm_rf(libdir);
    rm_rf(qdir);
    free(libdir); free(qdir);
}

/* 3. collision: target pre-exists. */
static void test_collision(void)
{
    char *libdir = make_tmpdir("coll-lib");
    char *qdir   = make_tmpdir("coll-q");
    if (!libdir || !qdir) {
        expect(0, "coll: tmpdirs allocated");
        free(libdir); free(qdir);
        return;
    }

    char src[512];
    snprintf(src, sizeof(src), "%s/track.bin", libdir);
    FILE *f = fopen(src, "wb"); if (f) { fputs("dummy", f); fclose(f); }

    /* Pre-create the target, forcing collision. */
    char prepop[512];
    snprintf(prepop, sizeof(prepop), "%s/track.bin", qdir);
    FILE *p = fopen(prepop, "wb"); if (p) { fputs("preexisting", p); fclose(p); }

    struct tag_record *rec;
    struct check_result cr;
    make_fake_cr_with_fail(&rec, &cr, src);

    struct quarantine_ctx qctx;
    int rc = quarantine_init(&qctx, libdir, qdir, false);
    expect(rc == 0, "coll: quarantine_init succeeds");
    rc = quarantine_move(&qctx, &cr);
    expect(rc == 0, "coll: quarantine_move returns 0 (resolves collision)");

    /* Original prepop file still exists; a .dup-* sibling exists. */
    expect(file_exists(prepop), "coll: preexisting target still there");
    /* Glob via system+test instead of pulling in glob.h. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "ls %s/track.bin.dup-* >/dev/null 2>&1", qdir);
    int found = (system(cmd) == 0);
    expect(found, "coll: .dup-<ts> sibling exists alongside original");

    quarantine_close(&qctx);
    cleanup_fake_cr(rec, &cr);
    rm_rf(libdir); rm_rf(qdir);
    free(libdir); free(qdir);
}

/* 4. outside-library refusal. */
static void test_outside_library(void)
{
    char *libdir = make_tmpdir("out-lib");
    char *qdir   = make_tmpdir("out-q");
    if (!libdir || !qdir) {
        expect(0, "out: tmpdirs allocated");
        free(libdir); free(qdir);
        return;
    }

    /* Make a fake source path that's NOT under libdir. */
    char src[512];
    snprintf(src, sizeof(src), "/tmp/nocturne-out-of-tree-source.bin");
    FILE *f = fopen(src, "wb"); if (f) { fputs("dummy", f); fclose(f); }

    struct tag_record *rec;
    struct check_result cr;
    make_fake_cr_with_fail(&rec, &cr, src);

    struct quarantine_ctx qctx;
    int rc = quarantine_init(&qctx, libdir, qdir, false);
    expect(rc == 0, "out: quarantine_init succeeds");
    rc = quarantine_move(&qctx, &cr);
    expect(rc != 0, "out: quarantine_move refuses outside-library path");
    expect(file_exists(src), "out: outside source untouched");

    quarantine_close(&qctx);
    cleanup_fake_cr(rec, &cr);
    unlink(src);
    rm_rf(libdir); rm_rf(qdir);
    free(libdir); free(qdir);
}

/* 5. uninitialized quarantine refusal. */
static void test_uninit_quarantine(void)
{
    char *libdir = make_tmpdir("uninit-lib");
    if (!libdir) { expect(0, "uninit: tmpdir"); return; }

    /* Choose a quarantine path that does NOT exist. */
    char qpath[512];
    snprintf(qpath, sizeof(qpath), "/tmp/nocturne-test-not-yet-created-XXXXXX-%d",
             (int)getpid());
    /* No mkdir — this dir must not exist. */

    struct quarantine_ctx qctx;
    int rc = quarantine_init(&qctx, libdir, qpath, false);
    expect(rc != 0, "uninit: quarantine_init refuses missing quarantine dir");

    rm_rf(libdir);
    free(libdir);
}

/* 6. lock contention: hold an exclusive flock externally before init. */
static void test_lock_contention(void)
{
    char *libdir = make_tmpdir("lock-lib");
    char *qdir   = make_tmpdir("lock-q");
    if (!libdir || !qdir) {
        expect(0, "lock: tmpdirs");
        free(libdir); free(qdir); return;
    }

    /* Pre-create the log file and grab an exclusive flock. */
    char logp[512];
    snprintf(logp, sizeof(logp), "%s/quarantine.log", qdir);
    int held_fd = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0600);
    expect(held_fd >= 0, "lock: pre-opened log fd");
    int rc_flock = flock(held_fd, LOCK_EX | LOCK_NB);
    expect(rc_flock == 0, "lock: held LOCK_EX externally");

    struct quarantine_ctx qctx;
    int rc = quarantine_init(&qctx, libdir, qdir, false);
    expect(rc != 0, "lock: quarantine_init refuses while lock held");

    /* Release. */
    if (held_fd >= 0) {
        flock(held_fd, LOCK_UN);
        close(held_fd);
    }

    rm_rf(libdir); rm_rf(qdir);
    free(libdir); free(qdir);
}

/* 7. multi-value FLAG NOT moved. */
static void test_flag_not_moved(void)
{
    struct check_result cr = {0};
    cr.any_flag = true;
    cr.any_fail = false;
    expect(quarantine_should_move(&cr) == false,
           "FLAG-only check_result is NOT quarantined");

    /* And conversely: any_fail=true triggers move. */
    struct check_result cr_fail = {0};
    cr_fail.any_fail = true;
    expect(quarantine_should_move(&cr_fail) == true,
           "FAIL check_result IS quarantined");

    /* tag_read_failed also moves (mirror of any_fail behaviour). */
    struct check_result cr_read = {0};
    cr_read.tag_read_failed = true;
    expect(quarantine_should_move(&cr_read) == true,
           "tag_read_failed check_result IS quarantined");
}

int main(int argc, char **argv)
{
    const char *fixdir = (argc > 1) ? argv[1] : "tests/fixtures";

    test_happy_path(fixdir);
    test_dry_run();
    test_collision();
    test_outside_library();
    test_uninit_quarantine();
    test_lock_contention();
    test_flag_not_moved();

    return test_finish("test_quarantine");
}
