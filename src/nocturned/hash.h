#ifndef NOCTURNE_NOCTURNED_HASH_H
#define NOCTURNE_NOCTURNED_HASH_H

/* Compute sha256 of audio payload (audio bytes only — ID3v2 header skipped
 * for MP3; whole file for FLAC/Opus/OGG/M4A which carry their metadata in
 * containers we do not strip).
 *
 * `out_hex` must be at least 65 bytes (64 hex chars + NUL). Returns 0 on
 * success. On failure returns -1 and writes the underlying errno into
 * `*errno_out` (may be NULL). EAGAIN signals a Pitfall-18 race
 * (file changed mid-read); the caller may retry. */
int hash_audio_payload(const char *path, char *out_hex, int *errno_out);

/* Pure helper: scan an open mp3 fd for the start of audio data after any
 * ID3v2 header. Leaves the fd seeked to the audio-payload start.
 * Returns the byte offset (0 if no ID3v2 header). -1 on read error. */
long hash_skip_id3v2(int fd);

#endif /* NOCTURNE_NOCTURNED_HASH_H */
