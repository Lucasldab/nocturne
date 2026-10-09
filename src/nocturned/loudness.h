#ifndef NOCTURNE_NOCTURNED_LOUDNESS_H
#define NOCTURNE_NOCTURNED_LOUDNESS_H

/* Per-track loudness levelling for resident transcodes.
 *
 * The phone only ever plays the lossy resident copies, so levelling is baked
 * into the transcode rather than tagged onto the FLAC: the archive FLAC is the
 * track id (sha256 of the whole file), and retagging it would detach every
 * pin, like and play. See docs/superpowers/specs/2026-10-10-loudness-and-
 * download-feedback-design.md. */

#define LOUDNESS_SILENCE_LUFS      (-70.0)  /* at or below: treat as silence */
#define LOUDNESS_PEAK_CEILING_DBTP (-1.0)   /* boost never pushes peaks past this */
#define LOUDNESS_MAX_BOOST_DB      (12.0)
#define LOUDNESS_MAX_CUT_DB        (-30.0)

/* Gain in dB that brings integrated loudness `i` (LUFS) to `target`, capped
 * so true peak `tp` (dBTP) stays at or below LOUDNESS_PEAK_CEILING_DBTP and
 * clamped to [MAX_CUT, MAX_BOOST]. No limiter: a peaky quiet track is boosted
 * less, never compressed. Silence, -inf and NaN measurements give 0 dB. An
 * unknown (NaN/inf) true peak does not cap the gain. Pure. */
double loudness_gain(double i, double tp, double target);

/* Parse "input_i" and "input_tp" from the JSON block ffmpeg's
 * `loudnorm=print_format=json` prints on stderr. "-inf" parses as -INFINITY.
 * Returns 0 and writes both outputs, or -1 (outputs untouched) when either
 * key is missing. Pure. */
int loudness_parse(const char *ffmpeg_stderr, double *i_out, double *tp_out);

/* Run one ffmpeg analysis pass over `src`'s first audio stream. Returns 0 on
 * success, -1 if ffmpeg could not run or its output did not parse. */
int loudness_measure(const char *src, double *i_out, double *tp_out);

#endif /* NOCTURNE_NOCTURNED_LOUDNESS_H */
