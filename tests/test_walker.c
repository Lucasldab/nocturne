/*
 * test_walker.c — minimal walker behaviour tests.
 *
 * Sets up a tmp dir with: one MP3 fixture, one .txt file, one .hidden_dir/,
 * one inside-pointing symlink, one outside-pointing symlink. Asserts the
 * walker stats match.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <taglib/tag_c.h>

#include "tags.h"
#include "walker.h"
#include "runner.h"

static int rm_rf(const char *path)
{
    if (!path || strncmp(path, "/tmp/", 5) != 0) return -1;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", path);
    return system(cmd);
}

static char *make_tmpdir(const char *tag)
{
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-walker-%s-XXXXXX", tag);
    char *p = strdup(tmpl);
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
}

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
    fclose(in); fclose(out);
    return ret;
}

struct cb_state {
    size_t calls;
};

static enum walk_result counting_cb(const struct tag_record *rec, void *userdata)
{
    (void)rec;
    struct cb_state *st = userdata;
    if (st) st->calls++;
    return WALK_CONTINUE;
}

/* Test layout:
 *   <tmp>/track.mp3                 — copied from fixture
 *   <tmp>/notes.txt                 — non-audio
 *   <tmp>/.hidden_dir/secret.mp3    — dotfile dir
 *   <tmp>/inside_link.mp3 -> track.mp3       — inside-pointing symlink
 *   <tmp>/escape_link.mp3 -> /tmp/<outside-mp3> — outside-pointing symlink
 */
static void test_full_walk(const char *fixdir)
{
    char *root = make_tmpdir("walk");
    char *outside_root = make_tmpdir("walk-outside");
    if (!root || !outside_root) {
        expect(0, "walker tmpdirs allocated");
        free(root); free(outside_root);
        return;
    }

    char src_mp3[512];
    snprintf(src_mp3, sizeof(src_mp3), "%s/clean_id3v24.mp3", fixdir);

    char inside_mp3[512];
    snprintf(inside_mp3, sizeof(inside_mp3), "%s/track.mp3", root);
    if (copy_file(src_mp3, inside_mp3) != 0) {
        expect(0, "walker: copy fixture into tmp lib");
        free(root); free(outside_root);
        return;
    }

    /* notes.txt */
    char txt[512];
    snprintf(txt, sizeof(txt), "%s/notes.txt", root);
    FILE *f = fopen(txt, "w"); if (f) { fputs("text", f); fclose(f); }

    /* .hidden_dir/secret.mp3 */
    char hidden_dir[512];
    snprintf(hidden_dir, sizeof(hidden_dir), "%s/.hidden_dir", root);
    mkdir(hidden_dir, 0755);
    char hidden_mp3[1024];
    snprintf(hidden_mp3, sizeof(hidden_mp3), "%s/secret.mp3", hidden_dir);
    copy_file(src_mp3, hidden_mp3);

    /* inside_link.mp3 -> track.mp3 */
    char inside_link[512];
    snprintf(inside_link, sizeof(inside_link), "%s/inside_link.mp3", root);
    symlink("track.mp3", inside_link);

    /* outside-pointing target. */
    char outside_target[512];
    snprintf(outside_target, sizeof(outside_target), "%s/outside.mp3", outside_root);
    copy_file(src_mp3, outside_target);

    /* escape_link.mp3 -> outside_target */
    char escape_link[512];
    snprintf(escape_link, sizeof(escape_link), "%s/escape_link.mp3", root);
    symlink(outside_target, escape_link);

    taglib_set_strings_unicode((BOOL)1);

    struct cb_state st = {0};
    struct walk_stats stats;
    int rc = walker_walk(root, counting_cb, &st, &stats);
    expect(rc == 0, "walker_walk on prepared tmp returns 0");

    /* track.mp3 + inside_link.mp3 → 2 audio files seen */
    expect(stats.audio_files_seen >= 2,
           "walker sees >=2 audio entries (track.mp3 + inside_link)");
    expect(st.calls == stats.audio_files_seen,
           "callback call count == audio_files_seen");

    /* notes.txt non-audio counted */
    expect(stats.skipped_non_audio >= 1,
           "walker counted at least one skipped_non_audio (notes.txt)");

    /* .hidden_dir skipped */
    expect(stats.skipped_dotfiles >= 1,
           "walker counted at least one skipped_dotfiles (.hidden_dir)");

    /* escape_link.mp3 skipped via symlink-outside-root */
    expect(stats.skipped_symlinks_outside_root >= 1,
           "walker counted at least one skipped_symlinks_outside_root");

    rm_rf(root);
    rm_rf(outside_root);
    free(root);
    free(outside_root);
}

static void test_missing_path(void)
{
    char missing[256];
    snprintf(missing, sizeof(missing),
             "/tmp/nocturne-walker-definitely-missing-%d", (int)getpid());

    struct walk_stats stats;
    int rc = walker_walk(missing, NULL, NULL, &stats);
    expect(rc != 0, "walker_walk on missing path returns non-zero");
}

int main(int argc, char **argv)
{
    const char *fixdir = (argc > 1) ? argv[1] : "tests/fixtures";

    test_full_walk(fixdir);
    test_missing_path();

    return test_finish("test_walker");
}
