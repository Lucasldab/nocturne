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
};

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

#endif
