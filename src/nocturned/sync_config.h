#ifndef NOCTURNE_NOCTURNED_SYNC_CONFIG_H
#define NOCTURNE_NOCTURNED_SYNC_CONFIG_H

#include <stdio.h>

struct nocturne_config;

enum sync_config_side {
    SIDE_DESKTOP = 0,
    SIDE_PHONE   = 1
};

#define NOCTURNE_DEFAULT_DESKTOP_NAME    "nocturne-desktop"
#define NOCTURNE_DEFAULT_PHONE_NAME      "nocturne-phone"
#define NOCTURNE_DEFAULT_PHONE_SYNC_META "/storage/emulated/0/sync/nocturne/meta"
#define NOCTURNE_DEFAULT_PHONE_SYNC_FILES "/storage/emulated/0/Music/nocturne"

/* Emit Syncthing folder + relevant <options> XML for the given side.
 * Pure function — no filesystem reads, no network. Returns 0 on success,
 * -1 on write error.
 *
 * Pitfall coverage:
 *   - Pitfall 9 (Send/Receive inversion): SIDE_DESKTOP emits sync-files
 *     type="sendonly"; SIDE_PHONE emits type="receiveonly". sync-meta
 *     is sendreceive on both sides.
 *   - Pitfall 20 (hostname leak): default device names are
 *     nocturne-desktop / nocturne-phone; never gethostname().
 *   - Pitfall 24 (silent versioning): both sides emit
 *     <versioning type="none"/> on sync-files.
 *   - Pitfall 25 (WiFi-only): NOT in this XML — phone-side app setting,
 *     documented in plan 03-05's phone-setup.md.
 *
 * SYNC-05 invariants in <options>:
 *     globalAnnounceEnabled=false, relaysEnabled=false. */
int sync_config_emit(enum sync_config_side side,
                     const struct nocturne_config *cfg,
                     FILE *out);

/* Apply the desktop-side folder config to the local Syncthing via
 * REST PUT /rest/config/folders/<id>. Requires syncthing_get_config
 * to have succeeded.
 *
 * Returns 0 on success (both folders 200 OK), -1 on any failure. */
int sync_config_apply(const struct nocturne_config *cfg);

#endif /* NOCTURNE_NOCTURNED_SYNC_CONFIG_H */
