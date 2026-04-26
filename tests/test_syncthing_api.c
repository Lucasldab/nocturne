/*
 * test_syncthing_api.c — request shape verification for syncthing_api.c.
 *
 * Strategy: spin a tiny TCP listener on 127.0.0.1:ephemeral inside a
 * pthread. Use syncthing_api_set_test_endpoint(host, port) to redirect
 * the wrapper at the listener; speak HTTP/1.1 over plain TCP. The
 * wrapper internally rewrites the URL scheme to http:// when
 * g_use_test_endpoint is in effect — keeps tests hermetic without
 * requiring real TLS plumbing.
 *
 * Behaviours covered (≥ 8):
 *   1. syncthing_get_config(missing dir) → -1.
 *   2. syncthing_get_config(valid synthetic) extracts host=127.0.0.1,
 *      port=8384, apikey.
 *   3. config with <address>192.168.1.1:8384</address> → -1.
 *   4. POST /rest/db/scan?folder=sync-files: URL+method+header verified.
 *   5. syncthing_rescan with no config loaded → 1.
 *   6. folder_id with special chars URL-encoded.
 *   7. Server that accepts but never replies: timeout returns -1
 *      within ~6s.
 *   8. 200 → 0; 401 → -1; 500 → -1.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "syncthing_api.h"
#include "runner.h"

struct mock_listener {
    int  fd;
    int  port;
    int  response_code;
    int  silent;            /* if 1, never reply (timeout test) */
    /* output: */
    char *captured;
    size_t captured_len;
    pthread_t thr;
};

static void *listener_thread(void *arg)
{
    struct mock_listener *m = (struct mock_listener *) arg;
    int c = accept(m->fd, NULL, NULL);
    if (c < 0) return NULL;

    /* Read the request bytes — we don't need the whole stream, just
     * enough to recover the request line + headers. 8 KiB is plenty.
     * Use a short receive timeout so silent-mode doesn't hang. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[8192];
    ssize_t n = recv(c, buf, sizeof(buf), 0);
    if (n > 0) {
        m->captured = malloc((size_t) n + 1);
        if (m->captured) {
            memcpy(m->captured, buf, (size_t) n);
            m->captured[n] = '\0';
            m->captured_len = (size_t) n;
        }
    }

    if (!m->silent) {
        char resp[256];
        const char *status_text = "OK";
        if (m->response_code == 401) status_text = "Unauthorized";
        else if (m->response_code == 500) status_text = "Internal Server Error";
        int rn = snprintf(resp, sizeof(resp),
            "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            m->response_code ? m->response_code : 200, status_text);
        send(c, resp, (size_t) rn, MSG_NOSIGNAL);
    } else {
        /* Sleep a bit so libcurl observes the timeout before we close. */
        sleep(7);
    }
    close(c);
    return NULL;
}

static int start_listener(struct mock_listener *m)
{
    m->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m->fd < 0) return -1;
    int yes = 1;
    setsockopt(m->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(m->fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) return -1;

    socklen_t sl = sizeof(sa);
    if (getsockname(m->fd, (struct sockaddr *) &sa, &sl) != 0) return -1;
    m->port = ntohs(sa.sin_port);

    if (listen(m->fd, 1) != 0) return -1;
    if (pthread_create(&m->thr, NULL, listener_thread, m) != 0) return -1;
    return 0;
}

static void stop_listener(struct mock_listener *m)
{
    pthread_join(m->thr, NULL);
    if (m->fd >= 0) close(m->fd);
    /* DO NOT free m->captured here — caller still inspects it after
     * stop_listener returns. Caller owns the buffer's lifetime; use
     * free_listener_capture() once assertions are done. */
}

static void free_listener_capture(struct mock_listener *m)
{
    free(m->captured);
    m->captured = NULL;
    m->captured_len = 0;
}

static int write_synthetic_config(const char *dir, const char *addr,
                                  const char *apikey)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
    if (system(cmd) != 0) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/config.xml", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f,
        "<configuration version=\"2\">\n"
        "  <gui enabled=\"true\" tls=\"true\">\n"
        "    <address>%s</address>\n"
        "    <apikey>%s</apikey>\n"
        "  </gui>\n"
        "</configuration>\n",
        addr, apikey);
    fclose(f);
    return 0;
}

static char *make_tmpdir(const char *tag)
{
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/nocturne-stapi-%s-XXXXXX", tag);
    char *p = strdup(tmpl);
    if (!p) return NULL;
    if (!mkdtemp(p)) { free(p); return NULL; }
    return p;
}

static int rm_rf(const char *p)
{
    if (!p || strncmp(p, "/tmp/", 5) != 0) return -1;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", p);
    return system(cmd);
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;

    syncthing_api_init();

    /* --- Test 1: missing config dir --- */
    {
        int rc = syncthing_get_config("/nonexistent-syncthing-dir-9876");
        expect(rc == -1, "test1: missing dir → -1");
    }

    /* --- Test 2: valid config loads --- */
    {
        char *dir = make_tmpdir("ok");
        expect(write_synthetic_config(dir, "127.0.0.1:8384", "test-api-key-abc") == 0,
               "test2: synthetic config written");
        int rc = syncthing_get_config(dir);
        expect(rc == 0, "test2: valid config loads (rc=0)");
        rm_rf(dir); free(dir);
    }

    /* --- Test 3: non-loopback rejected at parse time. The previous
     *     load (test 2) had a valid loopback config; we need to make
     *     this load attempt fail without leaving 192.168.1.1 cached.
     *     Easiest: do this BEFORE we load the test-4 fixture, so a
     *     parse failure has no follow-on effect. */
    {
        char *dir = make_tmpdir("nonlb");
        write_synthetic_config(dir, "192.168.1.1:8384", "x");
        int rc = syncthing_get_config(dir);
        expect(rc == -1, "test3: non-loopback host rejected");
        rm_rf(dir); free(dir);
    }

    /* Test 5 must come before we load the request-test config — it
     * needs g_config_loaded == 0. After test 2 it IS loaded, so
     * cleanup the loaded state but DON'T touch curl_global. */
    {
        /* poke internal state via the test helper: setting NULL host
         * does not clear g_config_loaded — we use cleanup which does.
         * curl_global will be re-init'd by the next syncthing_rescan
         * via syncthing_api_init() (idempotent). */
        syncthing_api_cleanup();
        int rc = syncthing_rescan("anything");
        expect(rc == 1, "test5: no config → 1");
    }

    /* Reload a valid config + start a listener for the request-shape tests. */
    char *cfgdir = make_tmpdir("req");
    write_synthetic_config(cfgdir, "127.0.0.1:8384", "test-api-key-abc");
    syncthing_api_init();
    expect(syncthing_get_config(cfgdir) == 0, "setup: cfg load (req)");

    /* --- Test 4: POST /rest/db/scan?folder=sync-files (200) --- */
    {
        struct mock_listener m = { .fd = -1, .response_code = 200 };
        expect(start_listener(&m) == 0, "test4: listener started");
        syncthing_api_set_test_endpoint("127.0.0.1", m.port);
        int rc = syncthing_rescan("sync-files");
        stop_listener(&m);
        expect(rc == 0, "test4: 200 → 0");
        expect(m.captured != NULL, "test4: captured request bytes");
        if (m.captured) {
            expect(strstr(m.captured, "POST /rest/db/scan?folder=sync-files HTTP/") != NULL,
                   "test4: URL+method line correct");
            expect(strstr(m.captured, "X-API-Key: test-api-key-abc") != NULL,
                   "test4: X-API-Key header present");
        }
        free_listener_capture(&m);
    }

    /* --- Test 6: special-char folder id is URL-encoded --- */
    {
        struct mock_listener m = { .fd = -1, .response_code = 200 };
        start_listener(&m);
        syncthing_api_set_test_endpoint("127.0.0.1", m.port);
        int rc = syncthing_rescan("my folder/with spaces");
        stop_listener(&m);
        expect(rc == 0, "test6: rc=0");
        if (m.captured) {
            /* URL-encoded space is %20, slash is %2F */
            expect(strstr(m.captured, "folder=my%20folder%2Fwith%20spaces") != NULL,
                   "test6: folder_id URL-encoded");
        }
        free_listener_capture(&m);
    }

    /* --- Test 8a: 401 response → -1 --- */
    {
        struct mock_listener m = { .fd = -1, .response_code = 401 };
        start_listener(&m);
        syncthing_api_set_test_endpoint("127.0.0.1", m.port);
        int rc = syncthing_rescan("sync-files");
        stop_listener(&m);
        expect(rc == -1, "test8a: 401 → -1");
        free_listener_capture(&m);
    }

    /* --- Test 8b: 500 response → -1 --- */
    {
        struct mock_listener m = { .fd = -1, .response_code = 500 };
        start_listener(&m);
        syncthing_api_set_test_endpoint("127.0.0.1", m.port);
        int rc = syncthing_rescan("sync-files");
        stop_listener(&m);
        expect(rc == -1, "test8b: 500 → -1");
        free_listener_capture(&m);
    }

    /* --- Test 7: silent server → timeout returns -1 within ~6s --- */
    {
        struct mock_listener m = { .fd = -1, .response_code = 200, .silent = 1 };
        start_listener(&m);
        syncthing_api_set_test_endpoint("127.0.0.1", m.port);
        time_t t0 = time(NULL);
        int rc = syncthing_rescan("sync-files");
        time_t t1 = time(NULL);
        stop_listener(&m);
        expect(rc == -1, "test7: silent server → -1");
        expect((t1 - t0) <= 7, "test7: returned within 7s (5s timeout + grace)");
        free_listener_capture(&m);
    }

    syncthing_api_set_test_endpoint(NULL, 0);
    rm_rf(cfgdir); free(cfgdir);
    syncthing_api_cleanup();

    return test_finish("test_syncthing_api");
}
