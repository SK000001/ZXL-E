#!/usr/bin/env bash
# ZXL-E benchmark — M1
# Per-file: pack/unpack each file individually, verify round-trip, compare to zstd-19 / xz-9e.
# Solid:    pack the whole corpus together, compare ratio to sum-of-individual.

set -e
cd "$(dirname "$0")/.."

CORPUS="${ZXLE_CORPUS:-../Zxl/tests}"
if [ ! -d "$CORPUS" ]; then
    echo "corpus not found: $CORPUS"
    echo "set ZXLE_CORPUS=<path> or place files in ../Zxl/tests"
    exit 1
fi

# Default file list — mirror ZXL's bench but skip files that don't exist.
DEFAULT_FILES="ntdll.dll kernel32.dll user32.dll test.js test.json test.md test.pdf test.png"
FILES="${ZXLE_FILES:-$DEFAULT_FILES}"

mkdir -p tests/baseline tests/unpacked
make -s all

# Auto-discover locally-built brunsli binaries so JPEG routing engages.
if [ -x third_party/brunsli/build/artifacts/cbrunsli.exe ] || \
   [ -x third_party/brunsli/build/artifacts/cbrunsli ]; then
    PATH="$PWD/third_party/brunsli/build/artifacts:$PATH"
    export PATH
fi

# Auto-discover locally-built packMP3 binary so MP3 routing engages.
if [ -x third_party/packmp3/source/packMP3.exe ] || \
   [ -x third_party/packmp3/source/packMP3 ]; then
    PATH="$PWD/third_party/packmp3/source:$PATH"
    export PATH
fi

BIN=./zxle
[ -x ./zxle.exe ] && BIN=./zxle.exe

ratio() { awk -v a="$1" -v b="$2" 'BEGIN{ if (b==0) print "n/a"; else printf "%.4f", a/b }'; }
elapsed_ms() { awk -v s="$1" -v e="$EPOCHREALTIME" 'BEGIN{printf "%d", (e-s)*1000}'; }

printf "%-16s %10s %10s %10s %10s %10s  %-3s  %6s %6s\n" "file" "orig" "zxle" "zstd-19" "xz-9e" "ratio" "rt" "pk_ms" "un_ms"
printf -- "----                  ----       ----       -------    -----      -----     --   ------ ------\n"

SUM_ORIG=0
SUM_ZXLE=0
SUM_ZSTD=0
SUM_XZ=0

for f in $FILES; do
    src="$CORPUS/$f"
    if [ ! -f "$src" ]; then continue; fi
    orig=$(stat -c%s "$src")

    # zxle pack/unpack
    out="tests/baseline/$f.zxle"
    t0=$EPOCHREALTIME
    "$BIN" pack "$out" "$src" >/dev/null 2>&1
    pack_ms=$(elapsed_ms "$t0")
    zxle_sz=$(stat -c%s "$out")
    t0=$EPOCHREALTIME
    "$BIN" unpack "$out" tests/unpacked >/dev/null 2>&1
    unp_ms=$(elapsed_ms "$t0")
    if cmp -s "$src" "tests/unpacked/$f"; then rt=OK; else rt=FAIL; fi

    # zstd-19 baseline
    zstd_sz=$(zstd -19 --long=27 -q -f -o "tests/baseline/$f.zst" "$src" >/dev/null 2>&1; stat -c%s "tests/baseline/$f.zst")
    # xz-9e baseline
    xz_sz=$(xz -9e -c "$src" 2>/dev/null | wc -c)

    printf "%-16s %10d %10d %10d %10d %10s  %-3s  %6d %6d\n" \
        "$f" "$orig" "$zxle_sz" "$zstd_sz" "$xz_sz" "$(ratio "$zxle_sz" "$orig")" "$rt" "$pack_ms" "$unp_ms"

    SUM_ORIG=$((SUM_ORIG + orig))
    SUM_ZXLE=$((SUM_ZXLE + zxle_sz))
    SUM_ZSTD=$((SUM_ZSTD + zstd_sz))
    SUM_XZ=$((SUM_XZ + xz_sz))
done

printf -- "----                  ----       ----       -------    -----      -----     --   ------ ------\n"
printf "%-16s %10d %10d %10d %10d\n" "sum-individual" "$SUM_ORIG" "$SUM_ZXLE" "$SUM_ZSTD" "$SUM_XZ"
echo
echo "ratios vs orig:"
printf "  zxle (per-file)    %s\n" "$(ratio "$SUM_ZXLE" "$SUM_ORIG")"
printf "  zstd-19            %s\n" "$(ratio "$SUM_ZSTD" "$SUM_ORIG")"
printf "  xz-9e              %s\n" "$(ratio "$SUM_XZ"   "$SUM_ORIG")"

# Solid run: pack all files into one container.
SOLID_INPUTS=""
for f in $FILES; do [ -f "$CORPUS/$f" ] && SOLID_INPUTS="$SOLID_INPUTS $CORPUS/$f"; done
t0=$EPOCHREALTIME
"$BIN" pack tests/baseline/solid.zxle $SOLID_INPUTS >/dev/null 2>&1
SOLID_PACK_MS=$(elapsed_ms "$t0")
SOLID_SZ=$(stat -c%s tests/baseline/solid.zxle)
t0=$EPOCHREALTIME
"$BIN" unpack tests/baseline/solid.zxle tests/unpacked/solid >/dev/null 2>&1
SOLID_UNP_MS=$(elapsed_ms "$t0")
SOLID_RT=OK
for f in $FILES; do
    [ -f "$CORPUS/$f" ] || continue
    if ! cmp -s "$CORPUS/$f" "tests/unpacked/solid/$f"; then SOLID_RT=FAIL; fi
done
echo
echo "solid (one zxle archive, all files):"
printf "  zxle solid         %d  ratio=%s  rt=%s\n" "$SOLID_SZ" "$(ratio "$SOLID_SZ" "$SUM_ORIG")" "$SOLID_RT"
printf "  vs sum-individual  %s smaller\n" "$(awk -v a="$SOLID_SZ" -v b="$SUM_ZXLE" 'BEGIN{printf "%.2f%%", (b-a)*100/b}')"
printf "  perf               pack=%dms unpack=%dms\n" "$SOLID_PACK_MS" "$SOLID_UNP_MS"

bench_zip() {
    local label="$1" ZIP="$2"
    [ -f "$ZIP" ] || return
    local base; base=$(basename "$ZIP")
    echo
    echo "$label ($base):"
    local t0 Z_PACK_MS Z_UNP_MS
    t0=$EPOCHREALTIME
    "$BIN" pack "tests/baseline/$base.zxle" "$ZIP" >/dev/null 2>&1
    Z_PACK_MS=$(elapsed_ms "$t0")
    t0=$EPOCHREALTIME
    "$BIN" unpack "tests/baseline/$base.zxle" "tests/unpacked/$base.d" >/dev/null 2>&1
    Z_UNP_MS=$(elapsed_ms "$t0")
    if cmp -s "$ZIP" "tests/unpacked/$base.d/$base"; then ZRT=OK; else ZRT=FAIL; fi
    local Z_ORIG Z_ZXLE Z_XZ Z_ZSTD
    Z_ORIG=$(stat -c%s "$ZIP")
    Z_ZXLE=$(stat -c%s "tests/baseline/$base.zxle")
    Z_XZ=$(xz -9e -c "$ZIP" 2>/dev/null | wc -c)
    Z_ZSTD=$(zstd -19 --long=27 -q -c "$ZIP" 2>/dev/null | wc -c)
    printf "  orig=%d  zxle=%d  zstd-19=%d  xz-9e=%d  rt=%s\n" "$Z_ORIG" "$Z_ZXLE" "$Z_ZSTD" "$Z_XZ" "$ZRT"
    printf "  zxle vs xz-9e: %s\n" "$(awk -v a="$Z_ZXLE" -v b="$Z_XZ" 'BEGIN{printf "%.2f%%", (a-b)*100/b}')"
    printf "  perf: pack=%dms unpack=%dms\n" "$Z_PACK_MS" "$Z_UNP_MS"
}

bench_zip "M2 ZIP-unwrap"       tests/corpus/pe-deflate.zip
bench_zip "M3 preflate (L6 ZIP)" tests/corpus/pe-deflate-l6.zip
bench_zip "M3b JPEG-in-ZIP"      tests/corpus/zip-with-jpeg.zip
bench_zip "M3c PNG-in-ZIP"       tests/corpus/zip-with-png.zip
bench_zip "DOCX (ZIP/L6 XML)"    tests/corpus/sample.docx
bench_zip "JAR (ZIP/L6 class)"   tests/corpus/sample.jar

bench_file() {
    local label="$1" SRC="$2"
    [ -f "$SRC" ] || return
    local base; base=$(basename "$SRC")
    echo
    echo "$label ($base):"
    local t0 F_PACK_MS F_UNP_MS
    t0=$EPOCHREALTIME
    "$BIN" pack "tests/baseline/$base.zxle" "$SRC" >/dev/null 2>&1
    F_PACK_MS=$(elapsed_ms "$t0")
    rm -rf "tests/unpacked/$base.d" && mkdir -p "tests/unpacked/$base.d"
    t0=$EPOCHREALTIME
    "$BIN" unpack "tests/baseline/$base.zxle" "tests/unpacked/$base.d" >/dev/null 2>&1
    F_UNP_MS=$(elapsed_ms "$t0")
    local F_RT=OK
    cmp -s "$SRC" "tests/unpacked/$base.d/$base" || F_RT=FAIL
    local F_ORIG F_ZXLE F_XZ F_ZSTD
    F_ORIG=$(stat -c%s "$SRC")
    F_ZXLE=$(stat -c%s "tests/baseline/$base.zxle")
    F_XZ=$(xz -9e -c "$SRC" 2>/dev/null | wc -c)
    F_ZSTD=$(zstd -19 -q -c "$SRC" 2>/dev/null | wc -c)
    printf "  orig=%d  zxle=%d  zstd-19=%d  xz-9e=%d  rt=%s\n" "$F_ORIG" "$F_ZXLE" "$F_ZSTD" "$F_XZ" "$F_RT"
    printf "  zxle vs xz-9e: %s\n" "$(awk -v a="$F_ZXLE" -v b="$F_XZ" 'BEGIN{printf "%.2f%%", (a-b)*100/b}')"
    printf "  perf: pack=%dms unpack=%dms\n" "$F_PACK_MS" "$F_UNP_MS"
}

bench_file "M3c packmp3 (MP3)" tests/corpus/synth.mp3
bench_file "M3d gzip (gz)"     tests/corpus/ntdll.dll.gz
bench_file "M3e tar (mixed)"   tests/corpus/mixed.tar
bench_file "M3e-targz (gz of mixed.tar)" tests/corpus/mixed.tar.gz
bench_file "M3e-tar gzip-in-tar"         tests/corpus/gz-in.tar
bench_file "M3f-ar (.deb shape)"          tests/corpus/mixed.deb
[ -f tests/corpus/real_hello.deb ] && bench_file "M3f-ar real .deb (hello_2.10-3)" tests/corpus/real_hello.deb
[ -f tests/corpus/real_coreutils.deb ] && bench_file "M3h real .deb zst (coreutils 9.5)" tests/corpus/real_coreutils.deb
[ -f tests/corpus/real_coreutils_src.tar.xz ] && bench_file "M3i real .tar.xz src (coreutils 9.11)" tests/corpus/real_coreutils_src.tar.xz
bench_file "M3g-bz2tar (bz2 of mixed.tar)" tests/corpus/mixed.tar.bz2
bench_file "M3g bz2-in-tar"                tests/corpus/bz2-in.tar
bench_file "M3h-zsttar (zst of mixed.tar)" tests/corpus/mixed.tar.zst
bench_file "M3h-zsttar level-3 ladder"     tests/corpus/mixed.tar.zst3
bench_file "M3i-xztar (xz -9e of mixed.tar)" tests/corpus/mixed.tar.xz
bench_file "M3j xz-in-tar"                   tests/corpus/xz-in.tar
bench_file "M3j zst-in-tar"                  tests/corpus/zst-in.tar

# M3b: JPEG via brunsli. Same shape as bench_zip, but compares brunsli-routed
# zxle against xz-9e (which hits the already-compressed wall).
JPG=tests/corpus/synth.jpg
if [ -f "$JPG" ]; then
    base=$(basename "$JPG")
    echo
    echo "M3b brunsli (JPEG) ($base):"
    t0=$EPOCHREALTIME
    "$BIN" pack "tests/baseline/$base.zxle" "$JPG" >/dev/null 2>&1
    J_PACK_MS=$(elapsed_ms "$t0")
    rm -rf "tests/unpacked/$base.d" && mkdir -p "tests/unpacked/$base.d"
    t0=$EPOCHREALTIME
    "$BIN" unpack "tests/baseline/$base.zxle" "tests/unpacked/$base.d" >/dev/null 2>&1
    J_UNP_MS=$(elapsed_ms "$t0")
    if cmp -s "$JPG" "tests/unpacked/$base.d/$base"; then JRT=OK; else JRT=FAIL; fi
    J_ORIG=$(stat -c%s "$JPG")
    J_ZXLE=$(stat -c%s "tests/baseline/$base.zxle")
    J_XZ=$(xz -9e -c "$JPG" 2>/dev/null | wc -c)
    J_ZSTD=$(zstd -19 -q -c "$JPG" 2>/dev/null | wc -c)
    printf "  orig=%d  zxle=%d  zstd-19=%d  xz-9e=%d  rt=%s\n" "$J_ORIG" "$J_ZXLE" "$J_ZSTD" "$J_XZ" "$JRT"
    printf "  zxle vs xz-9e: %s\n" "$(awk -v a="$J_ZXLE" -v b="$J_XZ" 'BEGIN{printf "%.2f%%", (a-b)*100/b}')"
    printf "  perf: pack=%dms unpack=%dms\n" "$J_PACK_MS" "$J_UNP_MS"
fi

# Competitor: precomp v0.4.7 (closest peer — also unwrap-and-recompress with
# preflate; uses LZMA2 as final step where we use zstd-19 long=27). We RT-verify
# every result; size compared against zxle and xz-9e on the same fixture.
PRECOMP=""
[ -x third_party/precomp/precomp.exe ] && PRECOMP=third_party/precomp/precomp.exe
[ -z "$PRECOMP" ] && [ -x third_party/precomp/precomp ] && PRECOMP=third_party/precomp/precomp
if [ -n "$PRECOMP" ]; then
    echo
    echo "=== Competitor: precomp v0.4.7 vs zxle (size + RT) ==="
    bench_precomp() {
        local label="$1" SRC="$2"
        [ -f "$SRC" ] || return 0
        local base; base=$(basename "$SRC")
        local pcf="tests/baseline/$base.pcf" rec="tests/unpacked/$base.precomp.bin"
        rm -f "$pcf" "$rec"
        local t0 pc_ms rec_ms PC_RT
        t0=$EPOCHREALTIME
        "$PRECOMP" -o"$pcf" "$SRC" >/dev/null 2>&1 || { echo "  $label: precomp failed"; return; }
        pc_ms=$(elapsed_ms "$t0")
        t0=$EPOCHREALTIME
        "$PRECOMP" -r -o"$rec" "$pcf" >/dev/null 2>&1 || { echo "  $label: precomp -r failed"; return; }
        rec_ms=$(elapsed_ms "$t0")
        cmp -s "$SRC" "$rec" && PC_RT=OK || PC_RT=FAIL
        local SZ ORIG XZ ZX
        ORIG=$(stat -c%s "$SRC")
        SZ=$(stat -c%s "$pcf")
        ZX=$(stat -c%s "tests/baseline/$base.zxle" 2>/dev/null || echo 0)
        XZ=$(xz -9e -c "$SRC" 2>/dev/null | wc -c)
        printf "  %s (%s):\n" "$label" "$base"
        printf "    orig=%d  zxle=%d  precomp=%d  xz-9e=%d  rt=%s\n" "$ORIG" "$ZX" "$SZ" "$XZ" "$PC_RT"
        if [ "$ZX" -gt 0 ]; then
            printf "    precomp vs zxle: %s   precomp vs xz-9e: %s   perf: pack=%dms restore=%dms\n" \
                "$(awk -v a="$SZ" -v b="$ZX" 'BEGIN{printf "%+.2f%%", (a-b)*100/b}')" \
                "$(awk -v a="$SZ" -v b="$XZ" 'BEGIN{printf "%+.2f%%", (a-b)*100/b}')" \
                "$pc_ms" "$rec_ms"
        fi
        rm -f "$pcf" "$rec"
    }
    # Headline-positive zxle fixtures: shapes where we beat xz-9e via unwrap.
    # Limited set chosen to span format families without bloating bench time.
    bench_precomp "ZIP unwrap"            tests/corpus/pe-deflate.zip
    bench_precomp "ZIP/L6 (preflate)"     tests/corpus/pe-deflate-l6.zip
    bench_precomp "DOCX (real ZIP/L6)"    tests/corpus/sample.docx
    bench_precomp "JAR (real ZIP/L6)"     tests/corpus/sample.jar
    bench_precomp "gzip wrapper"          tests/corpus/ntdll.dll.gz
    bench_precomp "gz of mixed.tar"       tests/corpus/mixed.tar.gz
    bench_precomp "deb-shape ar"          tests/corpus/mixed.deb
    bench_precomp "JPEG (brunsli)"        tests/corpus/synth.jpg
    bench_precomp "PNG (IDAT zlib-L9)"    "$CORPUS/test.png"
    bench_precomp "MP3 (packMP3)"         tests/corpus/synth.mp3
fi
