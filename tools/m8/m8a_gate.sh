#!/usr/bin/env bash
# M8a step 4 gate measurement.
#
# Runs cpu_lz_optimal_v4 at multiple input sizes, captures (M8a, zstd-19)
# sizes from its output, and separately runs xz -9e on the same prefix of the
# silesia mix (mozilla+webster+nci, concatenated in the same order v4 uses).
# Prints a table with deltas vs zstd-19 (the gate metric) and xz-9e.
#
# Sizes:
#   102 400      ("100 KB")
#   1 000 000    ("1 MB")
#   10 485 760   ("10 MB")
#   30 000 000   ("30 MB")    -- memory-limited probe at scale
# 100 MB is the original gate point but the v4 binary OOMs at that size on
# 8 GB RAM (pool + F[] + SA[] together ~9.6 GB). Trend at measured sizes
# carries the decision.

set -euo pipefail

V4=tools/m8/cpu_lz_optimal_v4.exe
MIX=/tmp/m8a_silesia_mix.bin

if [ ! -f "$V4" ]; then
    echo "build $V4 first" >&2
    exit 1
fi

if [ ! -f "$MIX" ]; then
    echo "building silesia mix at $MIX ..."
    cat tests/corpus/silesia/mozilla tests/corpus/silesia/webster tests/corpus/silesia/nci > "$MIX"
fi

printf "%-10s %12s %12s %12s %10s %10s\n" "size" "M8a_v4" "zstd-19" "xz-9e" "vs_zstd19" "vs_xz9e"
printf "%-10s %12s %12s %12s %10s %10s\n" "----" "------" "-------" "-----" "---------" "-------"

for SZ in 102400 1000000 10485760 30000000; do
    OUT=$("$V4" "$SZ" 2>&1 || true)
    # v4 prints "M8a v4 (2p): RATIO (SIZE / N)" and (later) "zstd-19    : RATIO (SIZE / N)".
    # The earlier "zstd-19 : out=N bytes" line is skipped via the paren filter.
    M8A_SZ=$(echo "$OUT" | sed -n 's/^M8a v4.*(\([0-9]\+\) \/ .*/\1/p')
    Z19_SZ=$(echo "$OUT" | sed -n 's/^zstd-19.*(\([0-9]\+\) \/ .*/\1/p')

    if [ -z "$M8A_SZ" ] || [ -z "$Z19_SZ" ]; then
        printf "%-10s  v4 FAILED at this size\n" "$SZ"
        continue
    fi

    XZ_SZ=$(head -c "$SZ" "$MIX" | xz -9e -T1 -c | wc -c | tr -d ' ')

    D_Z=$(awk -v a="$M8A_SZ" -v b="$Z19_SZ" 'BEGIN{printf "%+.2f%%", (a-b)/b*100}')
    D_X=$(awk -v a="$M8A_SZ" -v b="$XZ_SZ"  'BEGIN{printf "%+.2f%%", (a-b)/b*100}')

    printf "%-10s %12s %12s %12s %10s %10s\n" "$SZ" "$M8A_SZ" "$Z19_SZ" "$XZ_SZ" "$D_Z" "$D_X"
done
