/*
 * walker.c — recursive library walker for nocturne-tagcheck.
 *
 * Strategy:
 *   - Resolve root via realpath() — symlink-escape detection compares each
 *     candidate file's resolved path to this prefix.
 *   - fts_open(FTS_PHYSICAL | FTS_NOCHDIR) — physical walk so we see symlinks
 *     as FTS_SL/FTS_SLNONE entries and decide ourselves whether to follow.
 *     FTS_NOCHDIR keeps the process cwd stable.
 *   - Per file:
 *       FTS_D    → dotfile dirs are FTS_SKIP'd; otherwise counted.
 *       FTS_F    → run extension filter; on match, tags_extract → on_file().
 *       FTS_SL   → realpath() the link target; if it doesn't begin with the
 *                  resolved root prefix → skip; otherwise treat like FTS_F
 *                  (extension filter → tags_extract → on_file).
 *       FTS_DNR/FTS_ERR/FTS_NS → log + count as taglib_open_failures and
 *                                continue. Partial walk > aborted walk.
 *
 * Pitfall 21 hygiene: statfs() the root, warn on NFS / FUSE / CIFS. The
 *   walker is read-only so we don't refuse — only the future nocturned
 *   daemon (Phase 2) refuses on those filesystems because of inotify.
 *
 * Pitfall 18 (file-replace race): TagLib does its own fstat-equivalent on
 *   open; we don't add redundant guards.
 *
 * Pitfall 22: paths in error messages use %.*s with explicit length.
 */

#define _GNU_SOURCE

#include "walker.h"

#include <errno.h>
#include <fts.h>
#include <linux/magic.h>     /* NFS_SUPER_MAGIC / FUSE_SUPER_MAGIC */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/stat.h>

/* CIFS / SMB magic — not always exposed by linux/magic.h on every distro. */
#ifndef CIFS_MAGIC_NUMBER
#define CIFS_MAGIC_NUMBER 0xff534d42
#endif

/* Test whether `candidate` lies under `root` (both already realpath'd).
 * Both must be absolute. Returns true if candidate == root or is a child. */
static bool path_under_root(const char *candidate, const char *root)
{
    if (!candidate || !root) return false;
    size_t rl = strlen(root);
    if (rl == 0) return false;
    if (strncmp(candidate, root, rl) != 0) return false;

    /* Either exact match, or root must end at a path-separator boundary. */
    if (candidate[rl] == '\0') return true;
    if (candidate[rl] == '/') return true;
    /* root might already end with '/' (rare, realpath strips trailing /). */
    if (rl > 0 && root[rl - 1] == '/' && candidate[rl - 1] == '/') return true;
    return false;
}

/* Return true if the basename of `path` starts with '.'. */
static bool basename_is_dotfile(const char *path)
{
    if (!path) return false;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    return base[0] == '.';
}

/* Warn (stderr only) if root sits on a network filesystem. Walker still runs. */
static void warn_if_network_fs(const char *root)
{
    struct statfs sfs;
    if (statfs(root, &sfs) != 0) return;
    long t = (long)sfs.f_type;
    if (t == NFS_SUPER_MAGIC || t == FUSE_SUPER_MAGIC ||
        (unsigned long)t == CIFS_MAGIC_NUMBER) {
        fprintf(stderr,
                "nocturne-tagcheck: warning: %.*s is on a network/FUSE filesystem "
                "(type 0x%lx); walker will run read-only but Phase 2 daemon "
                "(inotify) will refuse this mount\n",
                (int)strlen(root), root, (unsigned long)t);
    }
}

/* Process a single audio-file path: extract tags, dispatch callback, free.
 * Returns the callback's walk_result (default WALK_CONTINUE on no callback). */
static enum walk_result process_audio_file(const char *path,
                                           walker_on_file_fn on_file,
                                           void *userdata,
                                           struct walk_stats *stats)
{
    if (stats) stats->audio_files_seen++;
    struct tag_record *rec = tags_extract(path);
    if (!rec) {
        /* OOM — skip. */
        if (stats) stats->taglib_open_failures++;
        return WALK_CONTINUE;
    }
    if (rec->tag_read_failed && stats) stats->taglib_open_failures++;

    enum walk_result r = WALK_CONTINUE;
    if (on_file) r = on_file(rec, userdata);

    tag_record_free(rec);
    return r;
}

int walker_walk(const char *root,
                walker_on_file_fn on_file,
                void *userdata,
                struct walk_stats *stats)
{
    if (stats) memset(stats, 0, sizeof(*stats));

    if (!root) {
        fprintf(stderr, "nocturne-tagcheck: walker_walk called with NULL root\n");
        return 1;
    }

    char *root_real = realpath(root, NULL);
    if (!root_real) {
        fprintf(stderr,
                "nocturne-tagcheck: cannot resolve library path: %.*s (%s)\n",
                (int)strlen(root), root, strerror(errno));
        return 1;
    }

    /* Verify it's a directory. */
    struct stat st;
    if (stat(root_real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr,
                "nocturne-tagcheck: library path is not a directory: %.*s\n",
                (int)strlen(root_real), root_real);
        free(root_real);
        return 1;
    }

    warn_if_network_fs(root_real);

    /* fts_open expects a NULL-terminated argv-style array of paths. */
    char *paths[2] = { root_real, NULL };
    FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!fts) {
        fprintf(stderr,
                "nocturne-tagcheck: fts_open failed on %.*s: %s\n",
                (int)strlen(root_real), root_real, strerror(errno));
        free(root_real);
        return 1;
    }

    int ret = 0;
    enum walk_result wr = WALK_CONTINUE;
    FTSENT *ent;
    while (wr == WALK_CONTINUE && (ent = fts_read(fts)) != NULL) {
        switch (ent->fts_info) {
        case FTS_D: {
            /* Pre-order directory. Skip dotfiles unless this IS the root. */
            if (ent->fts_level > 0 && ent->fts_name[0] == '.') {
                fts_set(fts, ent, FTS_SKIP);
                if (stats) stats->skipped_dotfiles++;
                break;
            }
            if (stats) stats->dirs_visited++;
            break;
        }
        case FTS_DP:
            /* Post-order directory entry — ignore. */
            break;
        case FTS_F: {
            if (stats) stats->files_seen_total++;
            if (basename_is_dotfile(ent->fts_path)) {
                if (stats) stats->skipped_dotfiles++;
                break;
            }
            enum audio_format fmt = tags_format_from_path(ent->fts_path);
            if (fmt == AUDIO_FORMAT_UNKNOWN) {
                if (stats) stats->skipped_non_audio++;
                break;
            }
            wr = process_audio_file(ent->fts_path, on_file, userdata, stats);
            break;
        }
        case FTS_SL:
        case FTS_SLNONE: {
            if (stats) stats->files_seen_total++;
            if (basename_is_dotfile(ent->fts_path)) {
                if (stats) stats->skipped_dotfiles++;
                break;
            }
            /* Resolve the symlink target. If it escapes the root → skip. */
            char *target = realpath(ent->fts_path, NULL);
            if (!target) {
                /* Dangling link or unreadable target. */
                if (stats) stats->skipped_symlinks_outside_root++;
                break;
            }
            if (!path_under_root(target, root_real)) {
                if (stats) stats->skipped_symlinks_outside_root++;
                free(target);
                break;
            }
            /* It's inside the tree. Run extension filter. */
            enum audio_format fmt = tags_format_from_path(target);
            if (fmt == AUDIO_FORMAT_UNKNOWN) {
                if (stats) stats->skipped_non_audio++;
                free(target);
                break;
            }
            wr = process_audio_file(target, on_file, userdata, stats);
            free(target);
            break;
        }
        case FTS_DNR:
        case FTS_ERR:
        case FTS_NS:
            fprintf(stderr,
                    "nocturne-tagcheck: cannot read %.*s: %s\n",
                    (int)strlen(ent->fts_path), ent->fts_path,
                    strerror(ent->fts_errno));
            if (stats) stats->taglib_open_failures++;
            break;
        default:
            /* FTS_DC, FTS_DEFAULT, etc. — ignore */
            break;
        }
    }

    fts_close(fts);
    free(root_real);
    return ret;
}
