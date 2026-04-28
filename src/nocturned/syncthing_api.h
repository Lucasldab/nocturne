#ifndef NOCTURNE_NOCTURNED_SYNCTHING_API_H
#define NOCTURNE_NOCTURNED_SYNCTHING_API_H

/* The ONLY outbound network surface in nocturned. Hardcoded to speak
 * https://127.0.0.1:<port> (loopback). CROSS-03 audit (plan 03-07)
 * verifies this invariant at five layers (ldd, nm, source, strace,
 * strings). */

#include <stddef.h>

/* Initialize libcurl globally. Idempotent — second call is a no-op.
 * Safe to call before syncthing_get_config. Returns 0 on success. */
int syncthing_api_init(void);

/* Cleanup. Call on graceful shutdown. */
void syncthing_api_cleanup(void);

/* Read GUI host+port + API key from <config_dir>/config.xml.
 *   config_dir == NULL  → defaults to $XDG_CONFIG_HOME/syncthing
 *                          (or $HOME/.config/syncthing).
 * Caches the result for the lifetime of the process. Refuses to load
 * a config that points at a non-loopback host (defense in depth).
 *
 * Returns 0 on success, -1 if config.xml is missing or unparseable
 * (caller logs and continues without rescans). */
int syncthing_get_config(const char *config_dir);

/* POST https://127.0.0.1:<port>/rest/db/scan?folder=<folder_id>
 * with the cached API key in X-API-Key. Empty body. 5-second timeout.
 *
 * Returns:
 *    0 — request returned 200
 *    1 — config not loaded (syncthing_get_config never succeeded);
 *        caller logs a warning and continues
 *   -1 — network/transport error or non-200 response */
int syncthing_rescan(const char *folder_id);

/* PUT https://127.0.0.1:<port>/rest/config/folders/<folder_id> with the
 * given JSON body. Same loopback enforcement as syncthing_rescan, same
 * graceful failure semantics. Used by `nocturned sync-config --apply`
 * (plan 03-04).
 *
 * Returns 0 / 1 / -1 — same convention as syncthing_rescan. */
int syncthing_put_folder_config(const char *folder_id,
                                const char *json_body);

/* GET https://127.0.0.1:<port>/rest/config/options
 * with the cached API key in X-API-Key. 5-second timeout.
 *
 * Body is captured into out[] (NUL-terminated, truncated to out_cap-1
 * if larger). out_len_out (nullable) reports the actual length pre-trunc
 * — useful so callers can detect truncation without scanning out[].
 *
 * Loopback enforcement is inherited from build_url(); this helper cannot
 * be redirected at non-loopback hosts even via the test seam (the test
 * seam itself enforces is_loopback_host).
 *
 * Returns:
 *    0 — request returned 200 (body in out[])
 *    1 — config not loaded (syncthing_get_config never succeeded);
 *        caller falls through to degraded mode
 *   -1 — network/transport error or non-200 response */
int syncthing_get_options(char *out, size_t out_cap, size_t *out_len_out);

/* Test seam: override the host/port AFTER syncthing_get_config has
 * loaded an API key. Used by tests/test_syncthing_api.c to redirect
 * requests at a per-test ephemeral mock listener. NULL host restores
 * the value from the loaded config. */
void syncthing_api_set_test_endpoint(const char *host, int port);

#endif /* NOCTURNE_NOCTURNED_SYNCTHING_API_H */
