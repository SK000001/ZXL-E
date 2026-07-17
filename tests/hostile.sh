#!/usr/bin/env bash
# Hostile-container round-trip: malformed ZIP/tar/ar entries that force a
# mid-walk pack_* failure after earlier entries already appended to the solid
# buckets. Guards the bucket-rollback fix (2026-07-17): without it, orphaned
# solid bytes desync every later op and the archive fails crc at unpack.
set -e
cd "$(dirname "$0")/.."
BIN=./zxle.exe
[ -x "$BIN" ] || BIN=./zxle

H=tests/scratch/hostile
rm -rf "$H"; mkdir -p "$H"

python - "$H" <<'PY'
import struct, sys, os, zlib

outdir = sys.argv[1]

# hostile.zip: good stored entry first (pack_zip appends to solid), then a
# stored entry whose comp_size != raw_size -> mid-walk -1.
def lfh(name, comp, raw, crc):
    return (b"PK\x03\x04" + struct.pack("<HHHHH", 20, 0, 0, 0, 0) +
            struct.pack("<IIIHH", crc, comp, raw, len(name), 0) + name)

def cdent(name, comp, raw, crc, off):
    return (b"PK\x01\x02" + struct.pack("<HHHHHH", 20, 20, 0, 0, 0, 0) +
            struct.pack("<IIIHHHHHII", crc, comp, raw, len(name), 0, 0, 0, 0, 0, off) +
            name)

n1, d1 = b"good.txt", b"G" * 4096
n2, d2 = b"bad.txt", b"A" * 4096
part1 = lfh(n1, len(d1), len(d1), zlib.crc32(d1)) + d1
off2 = len(part1)
part2 = lfh(n2, len(d2) - 100, len(d2), zlib.crc32(d2)) + d2[:len(d2) - 100]
cd = (cdent(n1, len(d1), len(d1), zlib.crc32(d1), 0) +
      cdent(n2, len(d2) - 100, len(d2), zlib.crc32(d2), off2))
eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH", 0, 0, 2, 2, len(cd), off2 + len(part2), 0)
open(os.path.join(outdir, "hostile.zip"), "wb").write(part1 + part2 + cd + eocd)

# hostile.tar: valid first entry, corrupt second header (non-octal size).
def tar_hdr(name, size):
    h = bytearray(512)
    h[0:len(name)] = name
    h[100:108] = b"0000644\x00"
    h[108:116] = b"0000000\x00"
    h[116:124] = b"0000000\x00"
    h[124:136] = ("%011o" % size).encode() + b"\x00"
    h[136:148] = b"00000000000\x00"
    h[148:156] = b" " * 8
    h[156] = ord("0")
    h[257:263] = b"ustar\x00"
    h[263:265] = b"00"
    chk = sum(h)
    h[148:156] = ("%06o" % chk).encode() + b"\x00 "
    return bytes(h)

t = tar_hdr(b"good.bin", 1024) + b"B" * 1024
bad = bytearray(tar_hdr(b"bad.bin", 512))
bad[124] = ord("9") + 1
t += bytes(bad) + b"C" * 512 + b"\x00" * 1024
open(os.path.join(outdir, "hostile.tar"), "wb").write(t)

# hostile.ar: valid first entry, corrupt second header terminator.
def ar_hdr(name, size):
    return (name.ljust(16) + "0           0     0     100644  " +
            str(size).ljust(10)).encode() + b"\x60\x0A"

a = b"!<arch>\n" + ar_hdr("one.bin", 64) + b"D" * 64
bad_hdr = bytearray(ar_hdr("two.bin", 64))
bad_hdr[58] = 0x00
a += bytes(bad_hdr) + b"E" * 64
open(os.path.join(outdir, "hostile.ar"), "wb").write(a)
PY

# A second file in each archive so orphaned bucket bytes (if any) desync it.
head -c 8192 tests/bench.sh > "$H/normal.txt"

fail=0
for f in hostile.zip hostile.tar hostile.ar; do
    rm -rf "$H/$f.d"
    "$BIN" pack "$H/$f.zxle" "$H/$f" "$H/normal.txt" >/dev/null 2>&1
    "$BIN" unpack "$H/$f.zxle" "$H/$f.d" >/dev/null 2>&1
    if cmp -s "$H/$f" "$H/$f.d/$f" && cmp -s "$H/normal.txt" "$H/$f.d/normal.txt"; then
        echo "$f: RT OK"
    else
        echo "$f: RT FAIL"; fail=1
    fi
done

# Nested: the hostile ZIP inside a tar exercises the same rollback via pack_tar.
(cd "$H" && tar cf ziphost-in.tar hostile.zip normal.txt)
rm -rf "$H/zh.d"
"$BIN" pack "$H/zh.zxle" "$H/ziphost-in.tar" >/dev/null 2>&1
"$BIN" unpack "$H/zh.zxle" "$H/zh.d" >/dev/null 2>&1
if cmp -s "$H/ziphost-in.tar" "$H/zh.d/ziphost-in.tar"; then
    echo "ziphost-in.tar: RT OK"
else
    echo "ziphost-in.tar: RT FAIL"; fail=1
fi

exit $fail
