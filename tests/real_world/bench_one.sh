#!/usr/bin/env bash
# Bench one real-world artifact: zxle vs xz-9e vs zstd-19, RT, top-level kind,
# and routing summary (stderr from pack).
set -e
cd "$(dirname "$0")/../.."

# Wire local recompressor binaries (same logic as tests/bench.sh).
if [ -x third_party/brunsli/build/artifacts/cbrunsli.exe ]; then
  PATH="$PWD/third_party/brunsli/build/artifacts:$PATH"
fi
if [ -x third_party/packmp3/source/packMP3.exe ]; then
  PATH="$PWD/third_party/packmp3/source:$PATH"
fi
export PATH

f="$1"
[ -z "$f" ] && { echo "usage: $0 <file>"; exit 1; }

src="tests/real_world/$f"
[ ! -f "$src" ] && { echo "missing: $src"; exit 1; }

orig=$(stat -c%s "$src")

mkdir -p tests/real_world/baseline tests/real_world/unpacked
out="tests/real_world/baseline/$f.zxle"

# Capture stderr so we can extract the routing line.
./zxle.exe pack "$out" "$src" 2> "tests/real_world/baseline/$f.pack.log" >/dev/null
zxle_sz=$(stat -c%s "$out")

./zxle.exe unpack "$out" tests/real_world/unpacked >/dev/null 2>&1
if cmp -s "$src" "tests/real_world/unpacked/$f"; then rt=OK; else rt=FAIL; fi

zstd -19 --long=27 -q -f -o "tests/real_world/baseline/$f.zst" "$src"
zstd_sz=$(stat -c%s "tests/real_world/baseline/$f.zst")
xz_sz=$(xz -9e -c "$src" 2>/dev/null | wc -c)

# Top-level kind: parse from the pack log. zxle prints "    <kind>:" lines (zip:, png:, gz:, bz2:, zst:, tar:, ar:).
# If none of those appear, the file went KIND_OPAQUE (or KIND_JPEG/MP3 which print no detail line, but we know via magic).
kind="opaque"
grep -qE "^ +zip:" "tests/real_world/baseline/$f.pack.log" && kind="zip"
grep -qE "^ +png:" "tests/real_world/baseline/$f.pack.log" && kind="png"
grep -qE "^ +gz:"  "tests/real_world/baseline/$f.pack.log" && kind="gzip"
grep -qE "^ +bz2:" "tests/real_world/baseline/$f.pack.log" && kind="bz2"
grep -qE "^ +zst:" "tests/real_world/baseline/$f.pack.log" && kind="zstd"
grep -qE "^ +tar:" "tests/real_world/baseline/$f.pack.log" && kind="tar"
grep -qE "^ +ar:"  "tests/real_world/baseline/$f.pack.log" && kind="ar"

awk -v f="$f" -v o="$orig" -v z="$zxle_sz" -v zs="$zstd_sz" -v xz="$xz_sz" -v rt="$rt" -v k="$kind" '
BEGIN {
  vs_xz = (z - xz) / xz * 100;
  printf "%-22s orig=%10d zxle=%10d xz9e=%10d zstd19=%10d  vs-xz9e=%+7.2f%%  rt=%s  kind=%s\n",
         f, o, z, xz, zs, vs_xz, rt, k;
}'

echo "  routing:"
sed -n 's/^/    /p' "tests/real_world/baseline/$f.pack.log" | grep -E "(zip:|png:|gz:|bz2:|zst:|tar:|ar:)" || echo "    (no container detail — KIND_OPAQUE or single-blob KIND)"
