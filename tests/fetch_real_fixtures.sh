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

echo
echo "Real-world fixtures ready under tests/corpus/."
