#!/usr/bin/env bash
# Fetch the real-world fixtures the bench references but does NOT regenerate
# synthetically. Idempotent: skips fixtures that already exist; pass --force
# to refetch.
#
# Files fetched:
#   tests/corpus/silesia/{12 files}              -- Silesia corpus (211 MB)
#   tests/corpus/real_hello.deb                  -- Debian hello_2.10-3 (.tar.xz data)
#   tests/corpus/real_coreutils.deb              -- Ubuntu coreutils 9.5 (.tar.zst data)
#   tests/corpus/real_coreutils_src.tar.xz       -- GNU coreutils 9.11 source tarball
#   tests/corpus/real_requests.whl               -- PyPI wheel, pure-python (ZIP)
#   tests/corpus/real_pydantic_core.whl          -- PyPI wheel, native win_amd64 (ZIP)
#   tests/corpus/real_express.tgz                -- npm package tarball (gzip tar)
#   tests/corpus/real_alpine_layer.tar.gz        -- Docker layer, alpine 3.19 (Go-gzip tar)
#   tests/corpus/real_newpipe.apk                -- Android APK, NewPipe 0.27.0 (signed ZIP)

set -e
cd "$(dirname "$0")/.."
mkdir -p tests/corpus

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

want() { [ $FORCE -eq 1 ] || [ ! -e "$1" ]; }

fetch() {
    local url="$1" dest="$2"
    if want "$dest"; then
        echo "fetching $dest <- $url"
        curl -fsSL -o "$dest" "$url" || { echo "  FAIL: $url"; return 1; }
    else
        echo "skip (exists): $dest"
    fi
}

# 1. Silesia corpus.
SILESIA_DIR=tests/corpus/silesia
if want "$SILESIA_DIR/dickens"; then
    echo "fetching silesia.zip"
    curl -fsSL -o /tmp/silesia.zip "https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip" \
        || { echo "  FAIL silesia.zip"; exit 1; }
    mkdir -p "$SILESIA_DIR"
    unzip -j -o /tmp/silesia.zip -d "$SILESIA_DIR" >/dev/null
    rm -f /tmp/silesia.zip
    echo "  $SILESIA_DIR/ populated ($(ls "$SILESIA_DIR" | wc -l) files)"
else
    echo "skip (exists): $SILESIA_DIR/"
fi

# 2. Real Debian hello (.tar.xz inner).
fetch "http://archive.ubuntu.com/ubuntu/pool/main/h/hello/hello_2.10-3build1_amd64.deb" \
      tests/corpus/real_hello.deb 2>/dev/null \
  || fetch "https://snapshot.debian.org/archive/debian/20240101T000000Z/pool/main/h/hello/hello_2.10-3_amd64.deb" \
           tests/corpus/real_hello.deb \
  || echo "  WARN: real_hello.deb not fetched (mirror down). Bench will skip this fixture."

# 3. Real Ubuntu coreutils (.tar.zst inner).
fetch "http://archive.ubuntu.com/ubuntu/pool/main/c/coreutils/coreutils_9.5-1ubuntu1_amd64.deb" \
      tests/corpus/real_coreutils.deb \
  || echo "  WARN: real_coreutils.deb not fetched. Bench will skip this fixture."

# 4. GNU coreutils source tarball (.tar.xz, dict=32MiB non-preset).
fetch "https://ftp.gnu.org/gnu/coreutils/coreutils-9.11.tar.xz" \
      tests/corpus/real_coreutils_src.tar.xz \
  || echo "  WARN: real_coreutils_src.tar.xz not fetched. Bench will skip this fixture."

# 5. Crown-A corpus expansion (2026-07-17): the world's dominant archive
#    classes. Wheels/APK are ZIPs; npm tarball and Docker layer are gzip tars.
#    All version-pinned so results are comparable across sessions.

whl_url() { # whl_url <pkg> <ver> <filename-suffix>
    curl -fsSL "https://pypi.org/pypi/$1/$2/json" | python -c "
import json, sys
d = json.load(sys.stdin)
for u in d['urls']:
    if u['filename'].endswith(sys.argv[1]):
        print(u['url']); break
" "$3"
}

# Pure-python wheel (text-heavy ZIP).
if want tests/corpus/real_requests.whl; then
    U=$(whl_url requests 2.32.3 "py3-none-any.whl") && \
        fetch "$U" tests/corpus/real_requests.whl \
        || echo "  WARN: real_requests.whl not fetched."
else
    echo "skip (exists): tests/corpus/real_requests.whl"
fi

# Binary-heavy wheel (native extension + python, win_amd64 ZIP).
if want tests/corpus/real_pydantic_core.whl; then
    U=$(whl_url pydantic-core 2.18.4 "cp312-none-win_amd64.whl") && \
        fetch "$U" tests/corpus/real_pydantic_core.whl \
        || echo "  WARN: real_pydantic_core.whl not fetched."
else
    echo "skip (exists): tests/corpus/real_pydantic_core.whl"
fi

# npm package tarball (gzip tar of JS/JSON/MD text).
fetch "https://registry.npmjs.org/express/-/express-4.19.2.tgz" \
      tests/corpus/real_express.tgz \
  || echo "  WARN: real_express.tgz not fetched."

# Docker image layer (gzip tar; alpine 3.19, linux/amd64, layer 0).
if want tests/corpus/real_alpine_layer.tar.gz; then
    TOK=$(curl -fsSL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:library/alpine:pull" \
          | python -c "import json,sys; print(json.load(sys.stdin)['token'])") && \
    LAYER=$(curl -fsSL -H "Authorization: Bearer $TOK" \
                 -H "Accept: application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.oci.image.index.v1+json" \
                 "https://registry-1.registry.docker.io/v2/library/alpine/manifests/3.19" 2>/dev/null \
            || curl -fsSL -H "Authorization: Bearer $TOK" \
                 -H "Accept: application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.oci.image.index.v1+json" \
                 "https://registry-1.docker.io/v2/library/alpine/manifests/3.19") && \
    MDIG=$(printf '%s' "$LAYER" | python -c "
import json, sys
d = json.load(sys.stdin)
for m in d['manifests']:
    p = m.get('platform', {})
    if p.get('architecture') == 'amd64' and p.get('os') == 'linux':
        print(m['digest']); break
") && \
    LDIG=$(curl -fsSL -H "Authorization: Bearer $TOK" \
                -H "Accept: application/vnd.docker.distribution.manifest.v2+json, application/vnd.oci.image.manifest.v1+json" \
                "https://registry-1.docker.io/v2/library/alpine/manifests/$MDIG" \
           | python -c "import json,sys; print(json.load(sys.stdin)['layers'][0]['digest'])") && \
    curl -fsSL -H "Authorization: Bearer $TOK" \
         -o tests/corpus/real_alpine_layer.tar.gz \
         "https://registry-1.docker.io/v2/library/alpine/blobs/$LDIG" && \
    echo "fetched tests/corpus/real_alpine_layer.tar.gz ($LDIG)" \
    || echo "  WARN: real_alpine_layer.tar.gz not fetched."
else
    echo "skip (exists): tests/corpus/real_alpine_layer.tar.gz"
fi

# Android APK (signed ZIP; pinned GitHub release).
fetch "https://github.com/TeamNewPipe/NewPipe/releases/download/v0.27.0/NewPipe_v0.27.0.apk" \
      tests/corpus/real_newpipe.apk \
  || echo "  WARN: real_newpipe.apk not fetched."

echo
echo "Real-world fixtures ready under tests/corpus/."
