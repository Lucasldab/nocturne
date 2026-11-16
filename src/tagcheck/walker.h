#ifndef NOCTURNE_TAGCHECK_WALKER_H
#define NOCTURNE_TAGCHECK_WALKER_H

#include <stddef.h>

#include "tags.h"

enum walk_result {
    WALK_CONTINUE = 0,
    WALK_STOP = 1
};

struct walk_stats {
    size_t dirs_visited;
    size_t files_seen_total;
    size_t audio_files_seen;
    size_t skipped_non_audio;
    size_t skipped_dotfiles;
    size_t skipped_symlinks_outside_root;
    size_t taglib_open_failures;
    size_t skipped_by_prefilter;   /* audio files the prefilter told us to skip */
};

enum walk_prefilter {
    WALK_PRE_EXTRACT = 0,   /* read tags and call on_file */
    WALK_PRE_SKIP    = 1,   /* skip this file without reading tags */
    WALK_PRE_STOP    = 2    /* stop the walk */
};

/* Optional per-audio-file hook run BEFORE tag extraction, so callers that
 * can decide from the path alone (e.g. an unchanged mtime/size) avoid the
 * TagLib open entirely. */
typedef enum walk_prefilter (*walker_prefilter_fn)(const char *path, void *userdata);

/* Per-audio-file callback. Receives the extracted record (owned by walker — do NOT free). */
typedef enum walk_result (*walker_on_file_fn)(const struct tag_record *rec, void *userdata);

/* Walk `root` recursively, invoking `on_file` for each audio file. Returns 0 on success,
 * non-zero on hard error (root inaccessible, etc.). Populates `stats` if non-NULL.
 *
 * Skips: dotfiles, non-audio extensions, symlinks pointing outside root.
 * Follows: symlinks pointing inside root.
 */
int walker_walk(const char *root,
                walker_on_file_fn on_file,
                void *userdata,
                struct walk_stats *stats);

/* walker_walk with a prefilter consulted before tags_extract for every
 * audio file (NULL prefilter = walker_walk). */
int walker_walk_filtered(const char *root,
                         walker_prefilter_fn prefilter,
                         walker_on_file_fn on_file,
                         void *userdata,
                         struct walk_stats *stats);

#endif
