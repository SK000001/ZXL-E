#!/usr/bin/env bash
# ZXL-E parser fuzz harness.
#
# Contract under test: `zxle pack <ANY_BYTES> out.zxle` exits 0 in bounded time.
# Any parser that fails to inflate a malformed container must fall through to
# KIND_OPAQUE (handled by min_pack's force_opaque tier). A crash, hang, or
# non-zero exit indicates a parser bug.
#
# Per-kind seed fixtures from tests/corpus/ are mutated with bit-flips,
# byte-flips, truncations, and header zeroing. Each mutation is fed to
# `./zxle pack` with a per-iteration timeout. Outcomes are classified as
# ok / fail / crash / hang and summarized.
#
# Crash and hang inputs are saved under tests/fuzz_crashes/<kind>/ for replay.

set -u
cd "$(dirname "$0")/.."

BIN=./zxle
[ -x ./zxle.exe ] && BIN=./zxle.exe
make -s all

# Auto-discover third-party binaries the bench script also uses.
for d in third_party/brunsli/build/artifacts third_party/packmp3/source third_party/zpaq; do
    [ -d "$d" ] && PATH="$PWD/$d:$PATH"
done
export PATH

ITERS="${FUZZ_ITERS:-30}"
TIMEOUT_S="${FUZZ_TIMEOUT:-30}"
SEED="${FUZZ_SEED:-1}"

PYTHON="${PYTHON:-}"
if [ -z "$PYTHON" ]; then
    for cand in python3 python py; do
        if "$cand" -c '' >/dev/null 2>&1; then PYTHON=$cand; break; fi
    done
fi
[ -z "$PYTHON" ] && { echo "fuzz: no python interpreter found" >&2; exit 2; }

CRASH_DIR=tests/fuzz_crashes
TMPDIR_FUZZ=$(mktemp -d -t zxle-fuzz.XXXXXX)
trap 'rm -rf "$TMPDIR_FUZZ"' EXIT

# kind:seed_fixture
KINDS=(
    "zip:tests/corpus/sample.jar"
    "tar:tests/corpus/gz-in.tar"
    "ar:tests/corpus/mixed.deb"
    "gzip:tests/corpus/ntdll.dll.gz"
    "bzip2:tests/corpus/mixed.tar.bz2"
    "xz:tests/corpus/mixed.tar.xz"
    "zstd:tests/corpus/mixed.tar.zst3"
)

# Mutate $1 (input file) into $2 (output file) using strategy $3 and seed $4.
# Strategies: bitflip, byteflip, truncate, zero_header, rand_header.
mutate() {
    local in=$1 out=$2 strat=$3 seed=$4
    "$PYTHON" - "$in" "$out" "$strat" "$seed" <<'PY'
import os, random, sys
inp, outp, strat, seed = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
data = bytearray(open(inp, 'rb').read())
n = len(data)
rng = random.Random(seed)
if strat == 'bitflip':
    for _ in range(rng.randint(1, 8)):
        i = rng.randrange(n)
        data[i] ^= 1 << rng.randrange(8)
elif strat == 'byteflip':
    for _ in range(rng.randint(1, 8)):
        i = rng.randrange(n)
        data[i] = rng.randrange(256)
elif strat == 'truncate':
    cut = rng.randrange(1, max(2, n))
    data = data[:cut]
elif strat == 'zero_header':
    hl = min(64, n)
    for i in range(hl):
        if rng.random() < 0.5:
            data[i] = 0
elif strat == 'rand_header':
    hl = min(64, n)
    for i in range(hl):
        data[i] = rng.randrange(256)
open(outp, 'wb').write(data)
PY
}

# Run a single fuzz iteration. Echoes outcome class.
run_one() {
    local mutated=$1 outfile=$2
    local rc
    timeout --kill-after=2 "$TIMEOUT_S" "$BIN" pack "$outfile" "$mutated" >/dev/null 2>&1
    rc=$?
    case $rc in
        0)        echo ok ;;
        124|137)  echo hang ;;     # SIGTERM/SIGKILL from timeout(1)
        134|139)  echo crash ;;    # SIGABRT / SIGSEGV
        *)        # Other non-zero. Treat signals (>128) as crashes; small codes as fail.
                  if [ "$rc" -gt 128 ]; then echo crash; else echo fail; fi ;;
    esac
}

STRATS=(bitflip byteflip truncate zero_header rand_header)

total_ok=0; total_fail=0; total_crash=0; total_hang=0
overall_status=0

printf "fuzz: %d iterations per kind, %ds timeout, base seed %d\n\n" \
    "$ITERS" "$TIMEOUT_S" "$SEED"
printf "%-8s %6s %6s %6s %6s   seed=%s\n" kind ok fail crash hang "$(basename "$0")"
printf "%-8s %6s %6s %6s %6s\n" -------- ------ ------ ------ ------

for entry in "${KINDS[@]}"; do
    kind=${entry%%:*}
    seed_file=${entry#*:}
    if [ ! -f "$seed_file" ]; then
        printf "%-8s missing seed: %s\n" "$kind" "$seed_file"
        continue
    fi

    ok=0; fail=0; crash=0; hang=0
    for ((i = 0; i < ITERS; i++)); do
        strat=${STRATS[$((i % ${#STRATS[@]}))]}
        seed=$((SEED * 1000 + i))
        mutated="$TMPDIR_FUZZ/$kind-$i.in"
        outfile="$TMPDIR_FUZZ/$kind-$i.zxle"
        mutate "$seed_file" "$mutated" "$strat" "$seed"

        outcome=$(run_one "$mutated" "$outfile")
        case "$outcome" in
            ok)    ok=$((ok+1)) ;;
            fail)  fail=$((fail+1)); save=1 ;;
            crash) crash=$((crash+1)); save=1 ;;
            hang)  hang=$((hang+1)); save=1 ;;
        esac
        if [ "${save:-0}" -eq 1 ]; then
            mkdir -p "$CRASH_DIR/$kind"
            cp "$mutated" "$CRASH_DIR/$kind/$outcome-strat=$strat-seed=$seed.bin"
            save=0
        fi
        rm -f "$mutated" "$outfile"
    done

    printf "%-8s %6d %6d %6d %6d\n" "$kind" "$ok" "$fail" "$crash" "$hang"
    total_ok=$((total_ok + ok))
    total_fail=$((total_fail + fail))
    total_crash=$((total_crash + crash))
    total_hang=$((total_hang + hang))
    [ $((fail + crash + hang)) -gt 0 ] && overall_status=1
done

printf "%-8s %6s %6s %6s %6s\n" -------- ------ ------ ------ ------
printf "%-8s %6d %6d %6d %6d\n" total "$total_ok" "$total_fail" "$total_crash" "$total_hang"

if [ "$overall_status" -ne 0 ]; then
    printf "\nfailing inputs saved under %s/\n" "$CRASH_DIR"
fi
exit "$overall_status"
