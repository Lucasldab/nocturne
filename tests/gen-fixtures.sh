#!/usr/bin/env bash
# gen-fixtures.sh — generate the audio fixture matrix for nocturne-tagcheck tests.
#
# Idempotent: rerunning produces the same files (or skips if already present).
# Driven entirely by ffmpeg; opustags/metaflac are optional (used when present
# to produce true canonical-multi-value tracks; otherwise a skip-marker file
# is written so test_check.c can $expect-skip the multi-value-canonical case).
#
# The actual fixture audio files are gitignored (.mp3/.flac/.opus). Only
# tests/fixtures/.gitkeep is tracked.

set -euo pipefail

OUTDIR="${1:-tests/fixtures}"
mkdir -p "$OUTDIR"

# --- tooling check ---
command -v ffmpeg >/dev/null \
    || { echo "ffmpeg not found (Arch: pacman -S ffmpeg)" >&2; exit 1; }

HAVE_OPUSTAGS=0
if command -v opustags >/dev/null; then HAVE_OPUSTAGS=1; fi
HAVE_METAFLAC=0
if command -v metaflac >/dev/null; then HAVE_METAFLAC=1; fi

# --- shared 1-second silence source ---
SRC="$OUTDIR/.silence.wav"
if [[ ! -f "$SRC" ]]; then
    ffmpeg -y -loglevel error -f lavfi -i \
        "anullsrc=channel_layout=stereo:sample_rate=44100" \
        -t 1 "$SRC"
fi

note_size() {
    local file="$1" prefix="$2"
    local sz
    sz=$(stat -c '%s' "$file" 2>/dev/null || echo 0)
    echo "[$prefix] $file ($sz bytes)"
}

gen_or_cache() {
    local target="$1"; shift
    if [[ -f "$target" ]]; then
        note_size "$target" "cached"
        return 0
    fi
    "$@"
    note_size "$target" "gen"
}

# --- a) clean ID3v2.4 UTF-8 MP3 (PASSES schema) ---
gen_clean_id3v24() {
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a libmp3lame -id3v2_version 4 -write_id3v1 0 \
        -metadata title="Clean Title" \
        -metadata artist="Clean Artist" \
        -metadata album="Clean Album" \
        -metadata album_artist="Clean Album Artist" \
        -metadata track="1/12" \
        -metadata disc="1/2" \
        -metadata date="2026" \
        -metadata genre="Electronic" \
        "$OUTDIR/clean_id3v24.mp3"
}
gen_or_cache "$OUTDIR/clean_id3v24.mp3" gen_clean_id3v24

# --- b) dirty ID3v2.3 MP3 (FAILS — id3_not_v24) ---
gen_dirty_id3v23() {
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a libmp3lame -id3v2_version 3 -write_id3v1 0 \
        -metadata title="V23 Title" \
        -metadata artist="V23 Artist" \
        -metadata album="V23 Album" \
        -metadata album_artist="V23 Album Artist" \
        -metadata track="2/12" \
        -metadata disc="1/1" \
        -metadata date="2026" \
        -metadata genre="Electronic" \
        "$OUTDIR/dirty_id3v23.mp3"
}
gen_or_cache "$OUTDIR/dirty_id3v23.mp3" gen_dirty_id3v23

# --- c) MP3 with no tag at all (FAILS — multiple missing_field) ---
gen_no_id3() {
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a libmp3lame -map_metadata -1 -write_id3v1 0 \
        "$OUTDIR/no_id3.mp3"
}
gen_or_cache "$OUTDIR/no_id3.mp3" gen_no_id3

# --- d) FLAC missing album_artist (FAILS — single missing_field) ---
gen_missing_album_artist_flac() {
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a flac \
        -metadata title="Almost Clean" \
        -metadata artist="Solo Artist" \
        -metadata album="Almost Album" \
        -metadata track="3" \
        -metadata disc="1" \
        -metadata date="2026" \
        -metadata genre="Ambient" \
        "$OUTDIR/missing_album_artist.flac"
}
gen_or_cache "$OUTDIR/missing_album_artist.flac" gen_missing_album_artist_flac

# --- e) clean FLAC (PASSES) ---
gen_clean_all_flac() {
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a flac \
        -metadata title="Clean FLAC Title" \
        -metadata artist="Clean FLAC Artist" \
        -metadata album="Clean FLAC Album" \
        -metadata album_artist="Clean FLAC Album Artist" \
        -metadata track="4" \
        -metadata disc="1" \
        -metadata date="2026" \
        -metadata genre="Ambient" \
        "$OUTDIR/clean_all.flac"
}
gen_or_cache "$OUTDIR/clean_all.flac" gen_clean_all_flac

# --- f) Opus with canonical multi-value Vorbis comments ---
# ffmpeg alone does NOT reliably emit multi-frame Vorbis comments for a
# single key. We rely on opustags to append a second ARTIST + GENRE.
gen_multi_value_canonical() {
    local target="$OUTDIR/multi_value_canonical.opus"
    # Base file with single artist + genre.
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a libopus \
        -metadata title="Canonical Multi" \
        -metadata album="Multi Album" \
        -metadata album_artist="Joint Album Artist" \
        -metadata artist="Artist A" \
        -metadata genre="Electronic" \
        -metadata track="5" \
        -metadata disc="1" \
        -metadata date="2026" \
        "$target"
    if [[ "$HAVE_OPUSTAGS" -eq 1 ]]; then
        opustags --in-place -a ARTIST="Artist B" -a GENRE="Ambient" "$target"
    else
        # Mark this fixture as not-canonical-multi-value capable on this
        # host: tests will read `.skip-multi-value-canonical` to know to
        # skip the strict assertion.
        touch "$OUTDIR/.skip-multi-value-canonical"
        echo "[warn] opustags missing — multi_value_canonical.opus has only 1 ARTIST/GENRE entry" >&2
    fi
}
if [[ ! -f "$OUTDIR/multi_value_canonical.opus" ]]; then
    gen_multi_value_canonical
    note_size "$OUTDIR/multi_value_canonical.opus" "gen"
else
    note_size "$OUTDIR/multi_value_canonical.opus" "cached"
fi

# --- g) Opus with concatenated semicolon artist (FLAGS) ---
gen_concat_semicolon() {
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a libopus \
        -metadata title="Concat Semi" \
        -metadata artist="Artist A; Artist B" \
        -metadata album="Concat Album" \
        -metadata album_artist="Joint" \
        -metadata genre="Pop" \
        -metadata track="6" \
        -metadata disc="1" \
        -metadata date="2026" \
        "$OUTDIR/concat_semicolon.opus"
}
gen_or_cache "$OUTDIR/concat_semicolon.opus" gen_concat_semicolon

# --- h) Opus with AC/DC-style legitimate slash (FLAGS — false positive) ---
gen_acdc_legit() {
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a libopus \
        -metadata title="Highway" \
        -metadata artist="AC/DC" \
        -metadata album="Highway to Hell" \
        -metadata album_artist="AC/DC" \
        -metadata genre="Rock" \
        -metadata track="7" \
        -metadata disc="1" \
        -metadata date="1979" \
        "$OUTDIR/acdc_legit.opus"
}
gen_or_cache "$OUTDIR/acdc_legit.opus" gen_acdc_legit

# --- i) "broken" MP3 with random bytes (FAILS — taglib_open_failed) ---
gen_broken_audio() {
    head -c 4096 /dev/urandom > "$OUTDIR/broken_audio.mp3"
}
gen_or_cache "$OUTDIR/broken_audio.mp3" gen_broken_audio

# --- j) Two MP3s with identical audio payload but different artist tags.
#       Used by test_hash to verify ID3v2 header skip is identity-stable.
#       We encode the audio with -metadata artist=… in one ffmpeg pass per
#       file; LAME's encoder output is deterministic for identical input,
#       so the audio frames are byte-identical and only the ID3 header
#       changes. ---
gen_same_audio_v1() {
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a libmp3lame -id3v2_version 4 -write_id3v1 0 \
        -metadata artist="Same Audio Artist A" \
        -metadata title="Same Audio" \
        -metadata album="Album" \
        "$OUTDIR/same_audio_v1.mp3"
}
gen_same_audio_v2() {
    ffmpeg -y -loglevel error -i "$SRC" \
        -codec:a libmp3lame -id3v2_version 4 -write_id3v1 0 \
        -metadata artist="Same Audio Artist B (longer to widen header)" \
        -metadata title="Same Audio" \
        -metadata album="Album" \
        "$OUTDIR/same_audio_v2.mp3"
}
gen_or_cache "$OUTDIR/same_audio_v1.mp3" gen_same_audio_v1
gen_or_cache "$OUTDIR/same_audio_v2.mp3" gen_same_audio_v2

echo "Fixture generation complete in $OUTDIR"
