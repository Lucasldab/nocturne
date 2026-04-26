/*
 * tags.c — TagLib-backed tag extraction for nocturne-tagcheck.
 *
 * Memory discipline:
 *   - Strings returned from taglib_tag_*() are freed wholesale by
 *     taglib_tag_free_strings() at the end of the function. We strdup what we
 *     want to keep BEFORE calling free_strings.
 *   - taglib_property_get() returns a NULL-terminated char** array that must
 *     be freed via taglib_property_free().
 *   - taglib_complex_property_keys() returns a char** freed via
 *     taglib_complex_property_free_keys().
 *
 * Multi-value detection (TAG-03):
 *   We use the SIMPLE properties API — taglib_property_get(file, "ARTIST") —
 *   which returns a NULL-terminated array of strings, one per text-frame
 *   value (ID3v2 TPE1 multiple frames; Vorbis ARTIST multiple comments).
 *   The complex-properties API in TagLib 2.x is targeted at structured
 *   payloads (PICTURE etc.) and overkill here. (Plan said "complex"; the
 *   simple-property API is the documented primitive for multi-value text
 *   frames — recorded as a deviation in 01-02-SUMMARY.md.)
 *
 * Out of scope (Pitfall 17/580): we never request the PICTURE complex
 *   property. Album-art bytes can be megabytes per file; skipping them
 *   keeps memory bounded by tag-text size.
 */

#define _GNU_SOURCE

#include "tags.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      /* strcasecmp */
#include <sys/stat.h>

#include <taglib/tag_c.h>

/* Static extension → format table. Case-insensitive comparison done in
 * tags_format_from_path. */
struct ext_entry {
    const char *ext;
    enum audio_format format;
};

static const struct ext_entry kExtTable[] = {
    { ".mp3",  AUDIO_FORMAT_MP3 },
    { ".flac", AUDIO_FORMAT_FLAC },
    { ".opus", AUDIO_FORMAT_OPUS },
    { ".ogg",  AUDIO_FORMAT_OGG_VORBIS },
    { ".oga",  AUDIO_FORMAT_OGG_VORBIS },
    { ".m4a",  AUDIO_FORMAT_MP4 },
    { ".m4b",  AUDIO_FORMAT_MP4 },
    { ".mp4",  AUDIO_FORMAT_MP4 },
};

enum audio_format tags_format_from_path(const char *path)
{
    if (!path) return AUDIO_FORMAT_UNKNOWN;
    const char *dot = strrchr(path, '.');
    if (!dot) return AUDIO_FORMAT_UNKNOWN;

    for (size_t i = 0; i < sizeof(kExtTable) / sizeof(kExtTable[0]); i++) {
        if (strcasecmp(dot, kExtTable[i].ext) == 0) {
            return kExtTable[i].format;
        }
    }
    return AUDIO_FORMAT_UNKNOWN;
}

/* Read first 10 bytes of an MP3 to determine ID3v2 major version.
 * Output:
 *   *out_version = 0 (no ID3v2 header), 2, 3, or 4
 *   *out_encoding = TAG_ENCODING_UNKNOWN/UTF8/UTF16
 * v2.4 is the canonical-for-nocturne form; mandates UTF-8 / UTF-16BE.
 * v2.3 supports ISO-8859-1 / UTF-16; we treat it as UTF-16 for the proxy. */
static void mp3_detect_id3v2(const char *path,
                             id3_major_version *out_version,
                             enum tag_encoding *out_encoding)
{
    *out_version = 0;
    *out_encoding = TAG_ENCODING_UNKNOWN;

    FILE *f = fopen(path, "rb");
    if (!f) return;

    unsigned char hdr[10];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n < 10) return;

    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        return;  /* no ID3v2 tag, but the file may still have ID3v1 at the end. */
    }

    *out_version = (id3_major_version)hdr[3];
    if (*out_version == 4) {
        *out_encoding = TAG_ENCODING_UTF8;   /* canonical for nocturne */
    } else if (*out_version == 3) {
        *out_encoding = TAG_ENCODING_UTF16;  /* proxy: v2.3 typically UTF-16 */
    } else if (*out_version == 2) {
        *out_encoding = TAG_ENCODING_OTHER;
    }
}

/* Set a tag_field from a simple-string TagLib getter result. The input string
 * is owned by TagLib (freed by taglib_tag_free_strings); we strdup. Empty
 * string → present=false. */
static void set_field_from_simple(struct tag_field *f, const char *src)
{
    f->value = NULL;
    f->present = false;
    f->is_multi_value_canonical = false;
    f->multi_value_count = 0;

    if (!src || src[0] == '\0') return;

    f->value = strdup(src);
    f->present = (f->value != NULL);
}

/* Set a tag_field from a numeric TagLib getter (track, year). 0 → absent. */
static void set_field_from_uint(struct tag_field *f, unsigned int v)
{
    f->value = NULL;
    f->present = false;
    f->is_multi_value_canonical = false;
    f->multi_value_count = 0;

    if (v == 0) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", v);
    f->value = strdup(buf);
    f->present = (f->value != NULL);
}

/* Look up a multi-value property and, if it has >1 entries, mark the field
 * canonical-multi-value with N as multi_value_count. If it has exactly 1
 * entry and the field's `value` is empty, populate `value` from it (used for
 * ALBUMARTIST/DISCNUMBER which the simple tag API doesn't expose). */
static void enrich_field_from_property(struct tag_field *f,
                                       TagLib_File *file,
                                       const char *prop_key)
{
    char **vals = taglib_property_get(file, prop_key);
    if (!vals) return;

    /* Count non-NULL entries. */
    size_t count = 0;
    while (vals[count] != NULL) count++;

    if (count == 0) {
        taglib_property_free(vals);
        return;
    }

    if (count > 1) {
        f->is_multi_value_canonical = true;
        f->multi_value_count = count;
        /* Keep the first value as the displayed string (caller may already
         * have populated f->value from the simple-tag getter — preserve it). */
        if (!f->value) {
            f->value = strdup(vals[0]);
            f->present = (f->value != NULL);
        }
    } else {
        /* Exactly 1 value. Populate `value` only if not already set, e.g. for
         * fields the simple-tag API doesn't expose (album_artist, disc_number). */
        if (!f->value && vals[0] && vals[0][0] != '\0') {
            f->value = strdup(vals[0]);
            f->present = (f->value != NULL);
        }
    }

    taglib_property_free(vals);
}

/* Build a fresh, zeroed tag_record with strdup'd path. */
static struct tag_record *tag_record_new(const char *path)
{
    struct tag_record *rec = calloc(1, sizeof(*rec));
    if (!rec) return NULL;
    rec->path = path ? strdup(path) : NULL;
    rec->format = tags_format_from_path(path);
    rec->encoding = TAG_ENCODING_UNKNOWN;
    rec->id3_version = 0;
    return rec;
}

/* Mark record as failed to read; copy reason into rec->read_error. */
static void mark_read_failed(struct tag_record *rec, const char *why)
{
    rec->tag_read_failed = true;
    if (why) rec->read_error = strdup(why);
}

struct tag_record *tags_extract(const char *path)
{
    struct tag_record *rec = tag_record_new(path);
    if (!rec) return NULL;  /* OOM — caller will see NULL but spec says "never NULL". */

    if (!path) {
        mark_read_failed(rec, "null path");
        return rec;
    }

    /* Encoding / ID3-version probe (MP3 only — Vorbis/MP4 are spec-UTF-8). */
    if (rec->format == AUDIO_FORMAT_MP3) {
        mp3_detect_id3v2(path, &rec->id3_version, &rec->encoding);
    } else if (rec->format == AUDIO_FORMAT_FLAC ||
               rec->format == AUDIO_FORMAT_OPUS ||
               rec->format == AUDIO_FORMAT_OGG_VORBIS ||
               rec->format == AUDIO_FORMAT_MP4) {
        rec->encoding = TAG_ENCODING_UTF8;
    }

    TagLib_File *file = taglib_file_new(path);
    if (!file) {
        char buf[256];
        snprintf(buf, sizeof(buf), "taglib_file_new returned NULL: %.200s",
                 path);
        mark_read_failed(rec, buf);
        return rec;
    }

    if (!taglib_file_is_valid(file)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "taglib_file_is_valid=false: %.200s",
                 path);
        mark_read_failed(rec, buf);
        taglib_file_free(file);
        return rec;
    }

    TagLib_Tag *tag = taglib_file_tag(file);
    if (tag) {
        char *t_title  = taglib_tag_title(tag);
        char *t_artist = taglib_tag_artist(tag);
        char *t_album  = taglib_tag_album(tag);
        char *t_genre  = taglib_tag_genre(tag);
        unsigned int   t_year  = taglib_tag_year(tag);
        unsigned int   t_track = taglib_tag_track(tag);

        set_field_from_simple(&rec->title,  t_title);
        set_field_from_simple(&rec->artist, t_artist);
        set_field_from_simple(&rec->album,  t_album);
        set_field_from_simple(&rec->genre,  t_genre);
        set_field_from_uint(&rec->year,         t_year);
        set_field_from_uint(&rec->track_number, t_track);

        taglib_tag_free_strings();
    }

    /* Album-artist + disc-number come from the property map; the simple Tag
     * struct doesn't carry them. Also re-run multi-value detection for
     * ARTIST and GENRE here. */
    enrich_field_from_property(&rec->album_artist, file, "ALBUMARTIST");
    enrich_field_from_property(&rec->disc_number,  file, "DISCNUMBER");
    enrich_field_from_property(&rec->artist,       file, "ARTIST");
    enrich_field_from_property(&rec->genre,        file, "GENRE");

    /* Track number / year may also live in properties; only enrich if the
     * simple-tag API didn't already populate them. */
    if (!rec->track_number.present)
        enrich_field_from_property(&rec->track_number, file, "TRACKNUMBER");
    if (!rec->year.present)
        enrich_field_from_property(&rec->year, file, "DATE");

    taglib_file_free(file);
    return rec;
}

static void tag_field_free(struct tag_field *f)
{
    if (!f) return;
    free(f->value);
    f->value = NULL;
    f->present = false;
    f->is_multi_value_canonical = false;
    f->multi_value_count = 0;
}

void tag_record_free(struct tag_record *rec)
{
    if (!rec) return;
    free(rec->path);
    free(rec->read_error);
    tag_field_free(&rec->title);
    tag_field_free(&rec->artist);
    tag_field_free(&rec->album);
    tag_field_free(&rec->album_artist);
    tag_field_free(&rec->track_number);
    tag_field_free(&rec->disc_number);
    tag_field_free(&rec->year);
    tag_field_free(&rec->genre);
    free(rec);
}
