#!/usr/bin/env bash
# Regenerate gitignored fixtures in tests/corpus/.
# Idempotent: skips fixtures that already exist. Pass --force to rebuild.

set -e
cd "$(dirname "$0")/.."
mkdir -p tests/corpus
cd tests/corpus

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

want() { [ $FORCE -eq 1 ] || [ ! -f "$1" ]; }

# synth.jpg, pe-deflate*.zip, zip-with-jpeg.zip predate this script and are
# referenced by name + size in roadmap.md. Don't regenerate from here.

# sample.docx — minimal Word-style DOCX (ZIP of XML, default deflate L6).
# Tests M2 fast-path miss + M3a preflate path on real-world ZIP content.
if want sample.docx; then
    python - <<'PY'
import zipfile, os
ct = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>'
rels = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'
# Build a substantive document body so deflate has something to chew on.
import random
random.seed(42)
words = "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua enim ad minim veniam quis nostrud exercitation ullamco laboris aliquip ex ea commodo consequat duis aute irure reprehenderit voluptate velit esse cillum fugiat nulla pariatur excepteur sint occaecat cupidatat non proident sunt in culpa qui officia deserunt mollit anim id est laborum".split()
body_paras = []
for i in range(8000):
    n = 30 + (i * 7) % 40
    sent = ' '.join(random.choice(words) for _ in range(n)).capitalize() + '.'
    body_paras.append(f'<w:p><w:r><w:t xml:space="preserve">Section {i}: {sent}</w:t></w:r></w:p>')
doc = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>' + ''.join(body_paras) + '</w:body></w:document>').encode()
with zipfile.ZipFile('sample.docx', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    z.writestr('[Content_Types].xml', ct)
    z.writestr('_rels/.rels', rels)
    z.writestr('word/document.xml', doc)
PY
fi

# sample.jar — small JAR with one compiled class + manifest. Tests JAR/ZIP path.
if want sample.jar; then
    TMP=$(mktemp -d)
    # Generate ~30 small classes to make the JAR substantive (~50-100 KB).
    python - "$TMP" <<'PY'
import sys, os
d = sys.argv[1]
for i in range(30):
    lines = ['public class C' + str(i) + ' {']
    for j in range(20):
        lines.append('    public int m' + str(j) + '(int x) { return x * ' + str(i*7+j) + ' + ' + str(i+j) + '; }')
    lines.append('    public static void main(String[] a) { System.out.println("c' + str(i) + '"); }')
    lines.append('}')
    open(os.path.join(d, 'C' + str(i) + '.java'), 'w').write('\n'.join(lines) + '\n')
PY
    (cd "$TMP" && javac *.java && jar cf sample.jar *.class)
    mv "$TMP/sample.jar" .
    rm -rf "$TMP"
fi

# synth.mp3 — 30 s stereo synthesised "music" (sine + phaser + chorus) @ 128 kbps.
# Pure tones compress trivially under xz-9e and don't show packMP3's win, so we
# use a content-rich signal that produces music-like MP3 frame entropy.
if want synth.mp3; then
    ffmpeg -y -loglevel error -f lavfi \
        -i "sine=f=220:d=30,asplit=2[a][b];[a]aphaser=type=t[c];[b]chorus=0.7:0.9:55:0.4:0.25:2[d];[c][d]amerge" \
        -ac 2 -b:a 128k synth.mp3
fi

# zip-with-png.zip — 1 stored PNG + 2 deflate-9 DLLs. Mirrors zip-with-jpeg.zip
# shape, but exercises OP_PNG_STORE in the ZIP recipe path.
if want zip-with-png.zip; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/test.png" ] && [ -f "$CORPUS/kernel32.dll" ] && [ -f "$CORPUS/user32.dll" ]; then
        python - "$CORPUS" <<'PY'
import sys, zipfile
c = sys.argv[1]
with zipfile.ZipFile('zip-with-png.zip', 'w') as z:
    z.write(f'{c}/test.png', 'test.png', compress_type=zipfile.ZIP_STORED)
    z.write(f'{c}/kernel32.dll', 'kernel32.dll', compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    z.write(f'{c}/user32.dll', 'user32.dll', compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
PY
    else
        echo "skipping zip-with-png.zip -- corpus files not found in $CORPUS"
    fi
fi

# mixed.tar — ustar tar of corpus PNG + JPEG + 2 DLLs. Tests M3e-tar routing
# (PNG -> pack_png, JPEG -> brunsli, DLLs -> solid STORE).
if want mixed.tar; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/test.png" ] && [ -f "$CORPUS/kernel32.dll" ] && \
       [ -f "$CORPUS/user32.dll" ] && [ -f "synth.jpg" ]; then
        TMP=$(mktemp -d)
        cp "$CORPUS/test.png" "$CORPUS/kernel32.dll" "$CORPUS/user32.dll" \
           "synth.jpg" "$TMP/"
        (cd "$TMP" && tar cf mixed.tar test.png synth.jpg kernel32.dll user32.dll)
        mv "$TMP/mixed.tar" .
        rm -rf "$TMP"
    else
        echo "skipping mixed.tar -- corpus or synth.jpg not found"
    fi
fi

# mixed.deb — .deb-shape AR archive: debian-binary + data.tar.gz of two DLLs.
# Tests M3f-ar recursive unwrap (ar -> gzip -> tar -> DLLs).
if want mixed.deb; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/ntdll.dll" ] && [ -f "$CORPUS/kernel32.dll" ]; then
        TMP=$(mktemp -d)
        echo "2.0" > "$TMP/debian-binary"
        cp "$CORPUS/ntdll.dll" "$CORPUS/kernel32.dll" "$TMP/"
        (cd "$TMP" && tar cf data.tar ntdll.dll kernel32.dll && \
            gzip -9 data.tar && \
            ar rc mixed.deb debian-binary data.tar.gz)
        mv "$TMP/mixed.deb" .
        rm -rf "$TMP"
    else
        echo "skipping mixed.deb -- corpus DLLs not found"
    fi
fi

# gz-in.tar — tar containing a .gz file + a plain DLL. Tests OP_GZIP_STORE
# routing inside pack_tar.
if want gz-in.tar; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/ntdll.dll" ] && [ -f "$CORPUS/kernel32.dll" ]; then
        TMP=$(mktemp -d)
        cp "$CORPUS/ntdll.dll" "$TMP/" && gzip -9 "$TMP/ntdll.dll"
        cp "$CORPUS/kernel32.dll" "$TMP/"
        (cd "$TMP" && tar cf gz-in.tar ntdll.dll.gz kernel32.dll)
        mv "$TMP/gz-in.tar" .
        rm -rf "$TMP"
    else
        echo "skipping gz-in.tar -- corpus DLLs not found"
    fi
fi

# mixed.tar.gz — gzip wrap of mixed.tar. Tests M3e-targz: gzip-wrapped tar with
# per-entry payloads getting format-aware routing (PNG -> pack_png, JPEG ->
# brunsli, DLLs -> solid STORE) instead of opaque-to-solid.
if want mixed.tar.gz; then
    if [ -f mixed.tar ]; then
        gzip -9 -k -c mixed.tar > mixed.tar.gz
    else
        echo "skipping mixed.tar.gz -- mixed.tar not present"
    fi
fi

# bz2-in.tar — tar containing a .bz2 file + a plain DLL. Tests OP_BZ2_STORE
# routing inside pack_tar (mirrors gz-in.tar / OP_GZIP_STORE).
if want bz2-in.tar; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/ntdll.dll" ] && [ -f "$CORPUS/kernel32.dll" ]; then
        TMP=$(mktemp -d)
        cp "$CORPUS/ntdll.dll" "$TMP/" && bzip2 -9 "$TMP/ntdll.dll"
        cp "$CORPUS/kernel32.dll" "$TMP/"
        (cd "$TMP" && tar cf bz2-in.tar ntdll.dll.bz2 kernel32.dll)
        mv "$TMP/bz2-in.tar" .
        rm -rf "$TMP"
    else
        echo "skipping bz2-in.tar -- corpus DLLs not found"
    fi
fi

# mixed.tar.bz2 — bzip2 -9 wrap of mixed.tar. Tests M3g-bz2tar: bz2-wrapped tar
# routed through pack_tar so per-entry payloads get format-aware treatment.
if want mixed.tar.bz2; then
    if [ -f mixed.tar ]; then
        bzip2 -9 -k -c mixed.tar > mixed.tar.bz2
    else
        echo "skipping mixed.tar.bz2 -- mixed.tar not present"
    fi
fi

# ntdll.dll.gz — gzip(L6, default) of a corpus DLL. Tests M3d-gzip preflate path
# (GNU gzip uses a different lazy-match policy from zlib so mode 0 misses).
if want ntdll.dll.gz; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/ntdll.dll" ]; then
        gzip -k -c "$CORPUS/ntdll.dll" > ntdll.dll.gz
    else
        echo "skipping ntdll.dll.gz -- $CORPUS/ntdll.dll not found"
    fi
fi

echo "fixtures in tests/corpus/:"
ls -l tests/corpus/ 2>/dev/null || ls -l .
