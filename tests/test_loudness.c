/*
 * test_loudness.c — unit + ffmpeg-backed tests for src/nocturned/loudness.{h,c}
 * and the levelled path of transcode_audio().
 *
 * Behaviours under test:
 *   1. loudness_gain: loud master is attenuated to the target.
 *   2. loudness_gain: quiet track is boosted to the target when peaks allow.
 *   3. loudness_gain: boost is capped so the true peak stays <= -1 dBTP.
 *   4. loudness_gain: boost clamped to +12 dB, cut clamped to -30 dB.
 *   5. loudness_gain: silence / -inf / NaN / below -70 LUFS -> 0 dB.
 *   6. loudness_parse: reads input_i / input_tp from ffmpeg loudnorm output.
 *   7. loudness_parse: "-inf" values parse as -INFINITY (silent source).
 *   8. loudness_parse: missing JSON block -> -1, outputs untouched.
 *   9. loudness_measure: a generated sine measures in the expected range.
 *  10. transcode_audio with level_lufs=-18: the Opus output measures within
 *      +-1 LU of -18, and the applied gain is reported back.
 *  11. transcode_audio with level_lufs=0: no levelling, gain reported NaN.
 *
 * 9-11 need ffmpeg with libopus on PATH (hearth has it); they are skipped
 * with a note, not failed, when it is absent.
 */

#define _GNU_SOURCE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "loudness.h"
#include "runner.h"
#include "transcode.h"

static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

static void test_gain(void)
{
    expect(near(loudness_gain(-9.0, 0.5, -18.0), -9.0, 1e-9),
           "loud master (-9 LUFS) is cut by 9 dB to reach -18");
    expect(near(loudness_gain(-24.0, -10.0, -18.0), 6.0, 1e-9),
           "quiet track with headroom is boosted by the full 6 dB");
    expect(near(loudness_gain(-24.0, -4.0, -18.0), 3.0, 1e-9),
           "boost capped at -1 dBTP: peak -4 allows only +3 dB");
    expect(near(loudness_gain(-40.0, -20.0, -18.0), 12.0, 1e-9),
           "boost clamped to +12 dB");
    expect(near(loudness_gain(15.0, 0.0, -18.0), -30.0, 1e-9),
           "cut clamped to -30 dB");
    expect(loudness_gain(-INFINITY, -INFINITY, -18.0) == 0.0,
           "-inf integrated loudness (silence) -> 0 dB");
    expect(loudness_gain(-75.0, -40.0, -18.0) == 0.0,
           "below -70 LUFS treated as silence -> 0 dB");
    expect(loudness_gain(NAN, 0.0, -18.0) == 0.0,
           "NaN measurement -> 0 dB");
    expect(near(loudness_gain(-24.0, NAN, -18.0), 6.0, 1e-9),
           "unknown true peak does not block an in-range boost");
}

static const char *SAMPLE =
    "[Parsed_loudnorm_0 @ 0x55d0c0a1b2c0] \n"
    "{\n"
    "\t\"input_i\" : \"-9.32\",\n"
    "\t\"input_tp\" : \"0.48\",\n"
    "\t\"input_lra\" : \"5.20\",\n"
    "\t\"input_thresh\" : \"-19.53\",\n"
    "\t\"output_i\" : \"-18.01\",\n"
    "\t\"normalization_type\" : \"dynamic\",\n"
    "\t\"target_offset\" : \"0.01\"\n"
    "}\n";

static const char *SILENT =
    "{\n\t\"input_i\" : \"-inf\",\n\t\"input_tp\" : \"-inf\",\n}\n";

static void test_parse(void)
{
    double i = 1, tp = 1;
    expect(loudness_parse(SAMPLE, &i, &tp) == 0, "parses a loudnorm JSON block");
    expect(near(i, -9.32, 1e-9), "input_i read");
    expect(near(tp, 0.48, 1e-9), "input_tp read");

    i = 1; tp = 1;
    expect(loudness_parse(SILENT, &i, &tp) == 0, "parses a silent source's block");
    expect(isinf(i) && i < 0, "input_i -inf parses as -INFINITY");

    i = 123; tp = 456;
    expect(loudness_parse("ffmpeg: something went wrong\n", &i, &tp) == -1,
           "no JSON block -> -1");
    expect(i == 123 && tp == 456, "outputs untouched on failure");
    expect(loudness_parse(NULL, &i, &tp) == -1, "NULL text -> -1");
}

static int have_ffmpeg_opus(void)
{
    return system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libopus") == 0;
}

static void test_with_ffmpeg(void)
{
    if (!have_ffmpeg_opus()) {
        printf("[SKIP] tests/test_loudness.c: ffmpeg with libopus not found\n");
        return;
    }
    char dir[] = "/tmp/nocturne-loudness-XXXXXX";
    if (!mkdtemp(dir)) { expect(0, "mkdtemp"); return; }
    char src[256], dst[256], dst2[256], cmd[1024];
    snprintf(src, sizeof(src), "%s/sine.flac", dir);
    snprintf(dst, sizeof(dst), "%s/out.opus", dir);
    snprintf(dst2, sizeof(dst2), "%s/plain.opus", dir);
    /* 1 kHz sine at half amplitude, stereo, 20 s: a "loud master" around -9 LUFS.
     * lavfi's sine source defaults to 1/8 amplitude, hence volume=4. */
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -nostdin -loglevel error -y -f lavfi "
             "-i 'sine=frequency=1000:sample_rate=44100:duration=20' "
             "-af 'volume=4' -ac 2 '%s'", src);
    expect(system(cmd) == 0, "fixture sine generated");

    double i = NAN, tp = NAN;
    expect(loudness_measure(src, &i, &tp) == 0, "loudness_measure succeeds");
    expect(i > -12.0 && i < -6.0, "half-scale sine measures between -12 and -6 LUFS");
    /* Mono -> stereo upmix costs 3 dB per channel: peak -6 becomes -9. */
    expect(tp > -11.0 && tp < -7.0, "true peak of the upmixed sine is about -9 dBTP");

    struct transcode_cfg cfg = { .enabled = true, .format = "opus",
                                 .bitrate_kbps = 128, .level_lufs = -18 };
    struct transcode_result res;
    expect(transcode_audio(src, dst, &cfg, &res) == 0, "levelled transcode succeeds");
    expect(isfinite(res.gain_db) && res.gain_db < -3.0, "a cut was applied and reported");
    expect(near(res.source_lufs, i, 0.2), "source loudness reported");

    double oi = NAN, otp = NAN;
    expect(loudness_measure(dst, &oi, &otp) == 0, "output measured");
    expect(near(oi, -18.0, 1.0), "levelled Opus output is within 1 LU of -18");

    cfg.level_lufs = 0;
    expect(transcode_audio(src, dst2, &cfg, &res) == 0, "unlevelled transcode succeeds");
    expect(isnan(res.gain_db), "no levelling -> gain reported as NaN");
    double pi = NAN, ptp = NAN;
    expect(loudness_measure(dst2, &pi, &ptp) == 0 && near(pi, i, 1.0),
           "unlevelled output keeps the source loudness");

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    (void) system(cmd);
}

int main(void)
{
    test_gain();
    test_parse();
    test_with_ffmpeg();
    return test_finish("test_loudness");
}
