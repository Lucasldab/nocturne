#ifndef NOCTURNE_NOCTURNED_DOWNLOAD_H
#define NOCTURNE_NOCTURNED_DOWNLOAD_H

/*
 * download.h — phone-initiated download dispatcher.
 *
 * Reads `<meta_dir>/downloads-phone-*.jsonl` request streams, dedups against
 * the terminal-state lines already in `<meta_dir>/downloads-desktop.jsonl`,
 * and execs the configured `flacget` binary once per fresh request.
 *
 * Status is reported back to the phone by appending one or more JSONL lines
 * to `<meta_dir>/downloads-desktop.jsonl`:
 *
 *   {"v":1, "id":"<uuid>", "state":"running|done|error",
 *    "ts":<ms>, "msg":"<optional>"}
 *
 * The phone's DownloadStatusReader tails the file and updates its Room table.
 *
 * IMPORTANT: this module does NOT take the single-writer cycle lock.
 * `flacget` invokes `nocturned cycle` itself, which acquires that lock —
 * holding it from here would deadlock. The download dispatcher uses its own
 * lockfile (`paths_download_pidfile()`) so it can run concurrently with a
 * normal cycle.
 */

struct download_stats {
    long requests_seen;            /* total request lines parsed */
    long requests_skipped_done;    /* id already terminal in desktop log */
    long requests_processed_ok;    /* flacget exited 0 */
    long requests_processed_err;   /* flacget exited non-zero or exec failed */
    long lines_skipped_parse_error;
};

/*
 * Scan `meta_dir` for pending download requests and dispatch each one through
 * `flacget_path`. Returns 0 on success, -1 on fatal error (OOM, status file
 * unwritable). Per-request errors are non-fatal and counted in stats.
 *
 *   meta_dir       — absolute path; missing dir returns 0 with zero stats.
 *   flacget_path   — absolute path to the wrapper script; missing/non-exec
 *                    triggers per-request "error" status writes.
 *   stats_out      — caller-owned, filled with run counters. May be NULL.
 */
int download_run(const char *meta_dir,
                 const char *flacget_path,
                 struct download_stats *stats_out);

#endif /* NOCTURNE_NOCTURNED_DOWNLOAD_H */
