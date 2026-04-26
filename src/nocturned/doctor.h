#ifndef NOCTURNE_NOCTURNED_DOCTOR_H
#define NOCTURNE_NOCTURNED_DOCTOR_H

#include <stddef.h>
#include <stdio.h>

struct nocturne_db;

struct doctor_report {
    /* tag health */
    long total_tracks;
    long parse_failed_count;       /* tags_status='parse_failed' */
    long incomplete_count;          /* tags_status='incomplete' */
    char **parse_failed_samples;    /* up to 5 paths, owned */
    size_t parse_failed_samples_n;

    /* orphans */
    long orphan_count;
    char **orphan_samples;
    size_t orphan_samples_n;

    /* inotify */
    long inotify_max_user_watches; /* -1 if unreadable */
    long library_dir_count;        /* -1 if unknown */
    long inotify_headroom;         /* max - dirs; -1 if unknown */

    /* disk */
    long long mount_total_bytes;   /* -1 if unknown */
    long long mount_free_bytes;
    int mount_free_percent;

    /* scan freshness */
    char *library_root;            /* nullable; from scan_meta. owned */
    char *last_scan_at_iso;        /* nullable; owned */
    long last_scan_age_seconds;    /* -1 if no scan */

    /* lockfile */
    int lock_held;                 /* 0 free, 1 held, 2 stale */
    int lock_holder_pid;           /* 0 if unheld */

    /* roll-up */
    int issues_found;              /* 0 = clean; >0 = at least one anomaly */
};

int doctor_collect(struct nocturne_db *db, struct doctor_report *out);
void doctor_report_free(struct doctor_report *r);

void doctor_print_text(const struct doctor_report *r, FILE *f);
void doctor_print_json(const struct doctor_report *r, FILE *f);

/* Test seam: pass an override for /proc/sys/fs/inotify/max_user_watches. */
int doctor_collect_with_override(struct nocturne_db *db,
                                 const char *inotify_override,
                                 const char *pidfile_override,
                                 struct doctor_report *out);

#endif
