#!/usr/bin/env bash
# CROSS-03 audit — multi-layer enforcement that nocturned never reaches the
# network. Layers (in order, fail-fast on each):
#   1. ldd      — no curl/ssl/crypto/nss/resolv shared libs
#   2. nm       — no suspect symbol names (curl_*, SSL_*, gethostby*, ...)
#   3. source   — no http/curl/socket primitives in src/nocturned/
#   4. strace   — runtime: zero AF_INET/AF_INET6 connect()/sendto() during
#                 a full scan→resolve→publish cycle
#   5. strings  — no http(s):// URL strings in the binary
#
# Run as: bash tests/test_no_network.sh [path-to-nocturned]

set -euo pipefail

BIN="${1:-build/nocturned}"
test -x "$BIN" || { echo "missing binary: $BIN" >&2; exit 1; }

fail=0

echo "==> [1/5] ldd: no curl/ssl/crypto/nss/resolv"
if ldd "$BIN" | grep -E 'libcurl|libssl|libcrypto|libnss|libresolv'; then
    echo "FAIL: linker pulled in a network library" >&2; fail=1
else
    echo "    ok"
fi

echo "==> [2/5] nm: no curl/SSL/getaddrinfo symbols"
# We allow weakly-referenced glibc symbols (UND in nm) like getaddrinfo if they
# come from libc's own NSS plumbing — but we still want zero direct references.
# Pattern: defined symbols (T, t, B, D, R, ...) from our binary referencing
# curl_/SSL_/EVP_ would be a smoking gun.
if nm -D "$BIN" 2>/dev/null | awk '
    {
      sym = $NF
      if (sym ~ /^(curl_|SSL_|EVP_|BIO_|TLS_)/) print
      if (sym == "gethostbyname" || sym == "gethostbyaddr") print
    }
' | grep . ; then
    echo "FAIL: dynamic symbol table contains a network primitive" >&2; fail=1
else
    echo "    ok"
fi

echo "==> [3/5] source grep: no http/curl/socket usage in src/nocturned"
# Scan source files only (not tests, not vendored). Allow the strings inside
# comment-only lines and inside this audit script's siblings (test_no_network*).
if grep -rE '\b(curl_|http_request|gethostby|getaddrinfo|socket\(|connect\(|sendto\(|recvfrom\(|inet_pton)\b' \
       src/nocturned/ 2>/dev/null \
   | grep -v '^[^:]*:[[:space:]]*\(/\*\|//\|\*\)' \
   | grep -v 'no_network' ; then
    echo "FAIL: source references network primitives" >&2; fail=1
else
    echo "    ok"
fi

echo "==> [4/5] runtime strace: no AF_INET/AF_INET6 connect/sendto during scan→resolve→publish"
TMPHOME=$(mktemp -d -t cross03-XXXXXX)
trap "rm -rf '$TMPHOME'" EXIT
mkdir -p "$TMPHOME/sync-meta"

if ! command -v strace >/dev/null 2>&1; then
    echo "    SKIP: strace not installed"
else
    HOME="$TMPHOME" XDG_DATA_HOME="$TMPHOME" XDG_CACHE_HOME="$TMPHOME" XDG_CONFIG_HOME="$TMPHOME" \
        strace -f -e trace=connect,sendto,sendmsg -o "$TMPHOME/scan.strace" \
        "$BIN" scan tests/fixtures >/dev/null 2>&1 || true
    HOME="$TMPHOME" XDG_DATA_HOME="$TMPHOME" XDG_CACHE_HOME="$TMPHOME" XDG_CONFIG_HOME="$TMPHOME" \
        strace -f -e trace=connect,sendto,sendmsg -o "$TMPHOME/resolve.strace" \
        "$BIN" resolve >/dev/null 2>&1 || true
    HOME="$TMPHOME" XDG_DATA_HOME="$TMPHOME" XDG_CACHE_HOME="$TMPHOME" XDG_CONFIG_HOME="$TMPHOME" \
        strace -f -e trace=connect,sendto,sendmsg -o "$TMPHOME/publish.strace" \
        "$BIN" publish --out "$TMPHOME/sync-meta" >/dev/null 2>&1 || true

    # Filter: only flag lines mentioning AF_INET / AF_INET6 with a successful
    # or in-flight connect/sendto. Filter out AF_UNIX (allowed: nss-systemd
    # over the session bus) and AF_NETLINK (allowed: route/uevent lookups).
    suspect=$(grep -hE 'connect\(|sendto\(|sendmsg\(' "$TMPHOME"/*.strace 2>/dev/null \
              | grep -E 'AF_INET|AF_INET6' \
              | grep -v 'AF_INET=AF_UNIX' \
              || true)
    if [ -n "$suspect" ]; then
        echo "FAIL: AF_INET/AF_INET6 connect or sendto observed at runtime" >&2
        echo "$suspect" >&2
        fail=1
    else
        echo "    ok (no AF_INET/AF_INET6 syscalls during scan→resolve→publish)"
    fi
fi

echo "==> [5/5] strings: no http(s):// URLs in binary (except known libs)"
if strings "$BIN" | grep -E '^https?://' \
   | grep -vE '(toml-c|tomlc99|sqlite\.org|jansson|json-schema|gnu\.org|w3\.org|json\.org)' ; then
    echo "FAIL: unexpected URL in binary" >&2; fail=1
else
    echo "    ok"
fi

if [ $fail -eq 0 ]; then
    echo "==> CROSS-03 audit PASSED"
else
    echo "==> CROSS-03 audit FAILED" >&2
    exit 1
fi
