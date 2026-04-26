#ifndef NOCTURNE_TAGCHECK_TAGS_H
#define NOCTURNE_TAGCHECK_TAGS_H

#include <stdbool.h>
#include <stddef.h>

enum audio_format {
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_MP3,         /* ID3v1/v2 (we want v2.4 UTF-8) */
    AUDIO_FORMAT_FLAC,        /* Vorbis comments */
    AUDIO_FORMAT_OPUS,        /* Vorbis comments inside Opus */
    AUDIO_FORMAT_OGG_VORBIS,  /* Vorbis comments */
    AUDIO_FORMAT_MP4          /* iTunes-style atoms */
};

enum tag_encoding {
    TAG_ENCODING_UNKNOWN = 0,
    TAG_ENCODING_UTF8,
    TAG_ENCODING_UTF16,
    TAG_ENCODING_LATIN1,
    TAG_ENCODING_OTHER
};

/* 0 = no ID3v2 / not MP3; 2 = v2.2; 3 = v2.3; 4 = v2.4 (canonical for nocturne). */
typedef int id3_major_version;

struct tag_field {
    char *value;                       /* owned; NULL if absent. UTF-8. */
    bool present;                      /* false → tag missing entirely (NULL value) */
    bool is_multi_value_canonical;     /* true → multi-frame/multi-comment in source file */
    size_t multi_value_count;          /* 0 or 1 if !is_multi_value_canonical; N if canonical */
};

struct tag_record {
    char *path;                        /* owned; absolute path */
    enum audio_format format;
    enum tag_encoding encoding;
    id3_major_version id3_version;     /* 0 if not MP3 or no ID3v2 */

    struct tag_field title;
    struct tag_field artist;
    struct tag_field album;
    struct tag_field album_artist;     /* ID3 TPE2 / Vorbis ALBUMARTIST */
    struct tag_field track_number;     /* string-form (may be "5/12") */
    struct tag_field disc_number;      /* string-form (may be "1/2") */
    struct tag_field year;             /* string-form (TDRC for v2.4) */
    struct tag_field genre;

    bool tag_read_failed;
    char *read_error;                  /* owned when tag_read_failed=true */
};

/* Never returns NULL. On TagLib failure, returns a record with tag_read_failed=true. */
struct tag_record *tags_extract(const char *path);
void tag_record_free(struct tag_record *rec);
enum audio_format tags_format_from_path(const char *path);

#endif
