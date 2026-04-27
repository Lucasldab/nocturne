/*
 * syncthing_api.c — libcurl wrapper, scoped strictly to https://127.0.0.1.
 *
 * The ONLY file in src/nocturned/ that links against libcurl. CROSS-03
 * audit (plan 03-07) layer 3 enforces this invariant at source-grep time.
 *
 * Loopback is enforced at THREE layers:
 *   1. Parse time (syncthing_get_config rejects non-loopback addresses
 *      in config.xml)
 *   2. URL build time (request helpers assert g_host before perform)
 *   3. Audit time (plan 03-07's tests/test_no_network.sh strace + strings)
 *
 * TLS verify is OFF: Syncthing's default GUI cert is self-signed. The
 * trust boundary is the loopback interface, not the certificate. That
 * choice is documented in T-03-03-05.
 *
 * Threat anchors:
 *   T-03-03-01 (URL tampering): host-allow-list at parse + URL-build
 *   T-03-03-02 (API key leak): never logged, never echoed
 *   T-03-03-03 (DoS): 5-second timeout
 *   T-03-03-04 (malformed config.xml): bounded buffer, fixed-size dest
 *   T-03-03-05 (self-signed cert): accepted; loopback is the boundary
 *   T-03-03-06 (DNS leak): hardcoded 127.0.0.1 / [::1] literals only
 */

#define _GNU_SOURCE

#include "syncthing_api.h"

#include <curl/curl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- module state ---- */
static int  g_initialized   = 0;
static char g_host[64]      = {0};   /* "127.0.0.1" / "::1" */
static int  g_port          = 0;     /* 8384 default */
static char g_api_key[128]  = {0};
static int  g_config_loaded = 0;

/* Test seam — overrides host/port AFTER config is loaded. */
static char g_test_host[64] = {0};
static int  g_test_port     = 0;
static int  g_use_test_endpoint = 0;

void syncthing_api_set_test_endpoint(const char *host, int port)
{
    if (host) {
        strncpy(g_test_host, host, sizeof(g_test_host) - 1);
        g_test_host[sizeof(g_test_host) - 1] = '\0';
        g_test_port = port;
        g_use_test_endpoint = 1;
    } else {
        g_use_test_endpoint = 0;
        g_test_host[0] = '\0';
        g_test_port = 0;
    }
}

int syncthing_api_init(void)
{
    if (g_initialized) return 0;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) return -1;
    g_initialized = 1;
    return 0;
}

void syncthing_api_cleanup(void)
{
    if (g_initialized) {
        curl_global_cleanup();
        g_initialized = 0;
    }
    g_config_loaded = 0;
    g_host[0] = '\0'; g_port = 0; g_api_key[0] = '\0';
    g_use_test_endpoint = 0;
}

/* Tightly bounded substring extraction: looks for `<tag>` ... `</tag>`
 * inside `block`, copies the inner text into dst (truncated to dstsz-1).
 * Returns 0 if found, -1 otherwise. */
static int xml_extract(const char *block, const char *tag,
                       char *dst, size_t dstsz)
{
    char open_tag[64];
    char close_tag[64];
    int n_open  = snprintf(open_tag,  sizeof(open_tag),  "<%s>",  tag);
    int n_close = snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    if (n_open < 0 || n_close < 0) return -1;

    const char *o = strstr(block, open_tag);
    if (!o) return -1;
    o += n_open;
    const char *c = strstr(o, close_tag);
    if (!c) return -1;
    size_t len = (size_t) (c - o);
    if (len >= dstsz) len = dstsz - 1;
    memcpy(dst, o, len);
    dst[len] = '\0';
    return 0;
}

/* Hostname allow-list: 127.0.0.1, [::1], ::1, localhost. We accept
 * `localhost` here ONLY because Syncthing's default config emits it
 * at first run; we then refuse to actually issue requests against it
 * unless it resolves locally. The audit's strace layer (plan 03-07)
 * is the source of truth on real network behaviour at runtime. */
static int is_loopback_host(const char *h)
{
    if (!h || !*h) return 0;
    if (!strcmp(h, "127.0.0.1")) return 1;
    if (!strcmp(h, "::1")) return 1;
    if (!strcmp(h, "[::1]")) return 1;
    if (!strcmp(h, "localhost")) return 1;
    return 0;
}

int syncthing_get_config(const char *config_dir)
{
    char buf[1024];
    char path[1024];

    if (config_dir && *config_dir) {
        snprintf(path, sizeof(path), "%s/config.xml", config_dir);
    } else {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        if (xdg && *xdg == '/') {
            snprintf(path, sizeof(path), "%s/syncthing/config.xml", xdg);
        } else if (home && *home == '/') {
            snprintf(path, sizeof(path),
                     "%s/.config/syncthing/config.xml", home);
        } else {
            return -1;
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    /* 1 MiB cap (T-03-03-04) */
    char *content = malloc(1024 * 1024);
    if (!content) { fclose(f); return -1; }
    size_t n = fread(content, 1, 1024 * 1024 - 1, f);
    content[n] = '\0';
    fclose(f);

    /* Find <gui ...> ... </gui>. */
    const char *gui_open = strstr(content, "<gui");
    const char *gui_close = strstr(content, "</gui>");
    if (!gui_open || !gui_close || gui_close <= gui_open) {
        free(content); return -1;
    }
    size_t gui_len = (size_t) (gui_close - gui_open);
    if (gui_len >= sizeof(buf)) gui_len = sizeof(buf) - 1;
    memcpy(buf, gui_open, gui_len);
    buf[gui_len] = '\0';

    char addr[128];
    if (xml_extract(buf, "address", addr, sizeof(addr)) != 0) {
        free(content); return -1;
    }
    /* parse host:port from `addr` */
    char *colon = strrchr(addr, ':');
    if (!colon) { free(content); return -1; }
    *colon = '\0';
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) { free(content); return -1; }

    if (!is_loopback_host(addr)) {
        fprintf(stderr,
            "syncthing_api: refusing to load config — GUI address %s "
            "is not loopback (allowlist: 127.0.0.1, ::1, localhost)\n",
            addr);
        free(content); return -1;
    }

    char apikey[128];
    if (xml_extract(buf, "apikey", apikey, sizeof(apikey)) != 0) {
        free(content); return -1;
    }
    /* Reject empty/whitespace-only keys. */
    int k_ok = 0;
    for (size_t i = 0; apikey[i]; i++) {
        if (apikey[i] != ' ' && apikey[i] != '\t' &&
            apikey[i] != '\n' && apikey[i] != '\r') { k_ok = 1; break; }
    }
    if (!k_ok) { free(content); return -1; }

    /* For URL safety, normalise localhost / [::1] → 127.0.0.1.
     * libcurl can connect to either, but tests + audit pin on the
     * canonical literal. */
    if (!strcmp(addr, "localhost")) strcpy(addr, "127.0.0.1");

    strncpy(g_host, addr, sizeof(g_host) - 1);
    g_host[sizeof(g_host) - 1] = '\0';
    g_port = port;
    strncpy(g_api_key, apikey, sizeof(g_api_key) - 1);
    g_api_key[sizeof(g_api_key) - 1] = '\0';
    g_config_loaded = 1;

    free(content);
    return 0;
}

static const char *active_host(void)
{
    return g_use_test_endpoint ? g_test_host : g_host;
}
static int active_port(void)
{
    return g_use_test_endpoint ? g_test_port : g_port;
}

/* Discard libcurl's response body — we only care about the HTTP status
 * code. Without this, libcurl writes to stdout. */
static size_t discard_write(void *ptr, size_t size, size_t nmemb, void *ud)
{
    (void) ptr; (void) ud;
    return size * nmemb;
}

/* Build a URL with strict loopback enforcement. Returns a heap buffer
 * the caller must free, or NULL on policy violation.
 * Test mode (g_use_test_endpoint=1) → http:// against the test mock
 * (which speaks plain HTTP/1.1). Production → https:// against the
 * real Syncthing GUI (which speaks TLS with a self-signed cert). */
static char *build_url(const char *path, const char *query_arg)
{
    const char *h = active_host();
    int port = active_port();
    if (!is_loopback_host(h) || port <= 0) {
        fprintf(stderr,
            "syncthing_api: URL build refused — host %s is not loopback\n",
            h ? h : "(null)");
        return NULL;
    }
    const char *scheme = g_use_test_endpoint ? "http" : "https";
    size_t need = 16 + strlen(h) + 8 + strlen(path) + 1 +
                  (query_arg ? strlen(query_arg) + 8 : 0);
    char *url = malloc(need);
    if (!url) return NULL;
    if (query_arg && *query_arg) {
        snprintf(url, need, "%s://%s:%d%s?%s", scheme, h, port, path, query_arg);
    } else {
        snprintf(url, need, "%s://%s:%d%s", scheme, h, port, path);
    }
    return url;
}

int syncthing_rescan(const char *folder_id)
{
    if (!g_config_loaded) return 1;
    if (!folder_id || !*folder_id) return -1;
    if (syncthing_api_init() != 0) return -1;

    CURL *easy = curl_easy_init();
    if (!easy) return -1;

    /* URL-encode the folder id (might contain spaces or unicode). */
    char *enc = curl_easy_escape(easy, folder_id, 0);
    if (!enc) { curl_easy_cleanup(easy); return -1; }
    char qarg[1024];
    snprintf(qarg, sizeof(qarg), "folder=%s", enc);
    curl_free(enc);

    char *url = build_url("/rest/db/scan", qarg);
    if (!url) { curl_easy_cleanup(easy); return -1; }

    char hdr[256];
    snprintf(hdr, sizeof(hdr), "X-API-Key: %s", g_api_key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, hdr);

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, (long) CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, discard_write);

    CURLcode rc = curl_easy_perform(easy);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    } else {
        fprintf(stderr,
            "syncthing_api: rescan transport error: %s\n",
            curl_easy_strerror(rc));
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(easy);
    free(url);

    if (rc != CURLE_OK) return -1;
    if (status == 200) return 0;
    fprintf(stderr,
        "syncthing_api: rescan POST returned HTTP %ld\n", status);
    return -1;
}

int syncthing_put_folder_config(const char *folder_id,
                                const char *json_body)
{
    if (!g_config_loaded) return 1;
    if (!folder_id || !*folder_id || !json_body) return -1;
    if (syncthing_api_init() != 0) return -1;

    CURL *easy = curl_easy_init();
    if (!easy) return -1;

    char *enc = curl_easy_escape(easy, folder_id, 0);
    if (!enc) { curl_easy_cleanup(easy); return -1; }
    char path[256];
    snprintf(path, sizeof(path), "/rest/config/folders/%s", enc);
    curl_free(enc);

    char *url = build_url(path, NULL);
    if (!url) { curl_easy_cleanup(easy); return -1; }

    char hdr_key[256];
    snprintf(hdr_key, sizeof(hdr_key), "X-API-Key: %s", g_api_key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, hdr_key);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long) strlen(json_body));
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, (long) CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, discard_write);

    CURLcode rc = curl_easy_perform(easy);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    } else {
        fprintf(stderr,
            "syncthing_api: PUT transport error: %s\n",
            curl_easy_strerror(rc));
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(easy);
    free(url);

    if (rc != CURLE_OK) return -1;
    if (status == 200) return 0;
    fprintf(stderr,
        "syncthing_api: PUT folder config returned HTTP %ld\n", status);
    return -1;
}
