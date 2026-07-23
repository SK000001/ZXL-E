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

# sample.xlsx — minimal Excel-style XLSX (ZIP of XML, default deflate L6).
# Same OOXML shape as sample.docx but with workbook + worksheet + sharedStrings;
# tests M2 fast-path + M3a preflate on XLSX-flavored ZIP content.
if want sample.xlsx; then
    python - <<'PY'
import zipfile, random
ct = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/></Types>'
rels = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>'
wb = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="S1" sheetId="1" r:id="rId1"/></sheets></workbook>'
wbrels = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/></Relationships>'
random.seed(42)
words = "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua".split()
strings = []
for i in range(4000):
    n = 6 + (i * 5) % 20
    strings.append(' '.join(random.choice(words) for _ in range(n)))
ss_parts = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="' + str(len(strings)) + '" uniqueCount="' + str(len(strings)) + '">']
for s in strings:
    ss_parts.append('<si><t xml:space="preserve">' + s + '</t></si>')
ss_parts.append('</sst>')
ss = ''.join(ss_parts).encode()
sh_rows = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>']
for i in range(len(strings)):
    sh_rows.append('<row r="' + str(i+1) + '"><c r="A' + str(i+1) + '" t="s"><v>' + str(i) + '</v></c><c r="B' + str(i+1) + '"><v>' + str(i * 137 % 9973) + '</v></c></row>')
sh_rows.append('</sheetData></worksheet>')
sh = ''.join(sh_rows).encode()
with zipfile.ZipFile('sample.xlsx', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    z.writestr('[Content_Types].xml', ct)
    z.writestr('_rels/.rels', rels)
    z.writestr('xl/workbook.xml', wb)
    z.writestr('xl/_rels/workbook.xml.rels', wbrels)
    z.writestr('xl/worksheets/sheet1.xml', sh)
    z.writestr('xl/sharedStrings.xml', ss)
PY
fi

# sample.pptx — minimal PowerPoint-style PPTX (ZIP of XML, default deflate L6).
# Same OOXML shape: presentation.xml + per-slide parts; tests M2 + M3a path on
# PPTX-flavored ZIP content.
if want sample.pptx; then
    python - <<'PY'
import zipfile, random
N_SLIDES = 40
ct_parts = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>']
for i in range(1, N_SLIDES+1):
    ct_parts.append('<Override PartName="/ppt/slides/slide' + str(i) + '.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>')
ct_parts.append('</Types>')
ct = ''.join(ct_parts).encode()
rels = b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/></Relationships>'
pres_rels_parts = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">']
for i in range(1, N_SLIDES+1):
    pres_rels_parts.append('<Relationship Id="rId' + str(i) + '" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide' + str(i) + '.xml"/>')
pres_rels_parts.append('</Relationships>')
pres_rels = ''.join(pres_rels_parts).encode()
pres_parts = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><p:sldIdLst>']
for i in range(1, N_SLIDES+1):
    pres_parts.append('<p:sldId id="' + str(255+i) + '" r:id="rId' + str(i) + '"/>')
pres_parts.append('</p:sldIdLst></p:presentation>')
pres = ''.join(pres_parts).encode()
random.seed(42)
words = "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua".split()
with zipfile.ZipFile('sample.pptx', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    z.writestr('[Content_Types].xml', ct)
    z.writestr('_rels/.rels', rels)
    z.writestr('ppt/presentation.xml', pres)
    z.writestr('ppt/_rels/presentation.xml.rels', pres_rels)
    for i in range(1, N_SLIDES+1):
        body_runs = []
        for j in range(200):
            n = 8 + (j * 5) % 24
            body_runs.append('<a:p><a:r><a:t xml:space="preserve">Slide ' + str(i) + ' line ' + str(j) + ': ' + ' '.join(random.choice(words) for _ in range(n)) + '.</a:t></a:r></a:p>')
        slide = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"><p:cSld><p:spTree><p:sp><p:txBody>' + ''.join(body_runs) + '</p:txBody></p:sp></p:spTree></p:cSld></p:sld>').encode()
        z.writestr('ppt/slides/slide' + str(i) + '.xml', slide)
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
    if ! command -v ffmpeg >/dev/null 2>&1; then
        echo "skipping synth.mp3 -- ffmpeg not found"
    else
    ffmpeg -y -loglevel error -f lavfi \
        -i "sine=f=220:d=30,asplit=2[a][b];[a]aphaser=type=t[c];[b]chorus=0.7:0.9:55:0.4:0.25:2[d];[c][d]amerge" \
        -ac 2 -b:a 128k synth.mp3
    fi
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

# zip-in.tar — tar containing a ZIP (sample.jar) + a plain DLL. Tests
# OP_ZIP_STORE routing inside pack_tar (v7).
if want zip-in.tar; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f sample.jar ] && [ -f "$CORPUS/kernel32.dll" ]; then
        TMP=$(mktemp -d)
        cp sample.jar "$TMP/"
        cp "$CORPUS/kernel32.dll" "$TMP/"
        (cd "$TMP" && tar cf zip-in.tar sample.jar kernel32.dll)
        mv "$TMP/zip-in.tar" .
        rm -rf "$TMP"
    else
        echo "skipping zip-in.tar -- sample.jar or corpus DLL not found"
    fi
fi

# flate-blob.bin — synthetic asset-pack: binary shell with embedded zlib
# streams (L9 -> redeflate path, L6 -> preflate path), the SWF/WOFF/PSD/
# save-game shape. Tests the generic opaque flate scan (2026-07-17).
if want flate-blob.bin; then
    python - <<'PY'
import zlib, random, json
random.seed(7)
out = bytearray()
out += b"ASSETPAK\x01\x00" + bytes(random.getrandbits(8) for _ in range(4096))
words = "entity sprite shader mesh anim sound level trigger flag state".split()
for i in range(6):
    if i % 3 == 2:
        payload = bytes(random.getrandbits(8) for _ in range(2000)) * 8
    else:
        doc = {f"{random.choice(words)}_{j}": {"id": j, "pos": [j * 3, j * 7, j % 5],
               "props": [random.choice(words) for _ in range(12)]} for j in range(1200)}
        payload = json.dumps(doc).encode()
    level = 9 if i % 2 == 0 else 6
    out += bytes(random.getrandbits(8) for _ in range(512))
    out += zlib.compress(payload, level)
out += bytes(random.getrandbits(8) for _ in range(2048))
open("flate-blob.bin", "wb").write(bytes(out))
PY
fi

# multi.gz — concatenated multi-member gzip (BGZF / pgzip / concatenated-log
# shape). Tests v9 multi-member unwrap: each member is inflated + redeflated/
# preflated and its body shares the solid stream, instead of the whole file
# going opaque. Two compressible members at mixed levels (6 -> preflate,
# 9 -> redeflate). mtime pinned so regen is stable.
if want multi.gz; then
    python - <<'PY'
import gzip, io, json, random
random.seed(11)
words = "alpha beta gamma delta epsilon zeta eta theta iota kappa".split()
def doc(n):
    return json.dumps({f"{random.choice(words)}_{j}": {"id": j, "vals": [j*2, j*3, j%7],
        "tags": [random.choice(words) for _ in range(10)]} for j in range(n)}).encode()
def gz(data, level):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=level, mtime=0) as f:
        f.write(data)
    return buf.getvalue()
open("multi.gz", "wb").write(gz(doc(1500), 6) + gz(doc(1800), 9))
PY
fi

# pdf-in.tar — tar containing a PDF (test.pdf) + a plain DLL. Tests pack_pdf
# dispatch inside pack_tar (nested PDF recipe rides OP_ZIP_STORE, 2026-07-17).
if want pdf-in.tar; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/test.pdf" ] && [ -f "$CORPUS/kernel32.dll" ]; then
        TMP=$(mktemp -d)
        cp "$CORPUS/test.pdf" "$CORPUS/kernel32.dll" "$TMP/"
        (cd "$TMP" && tar cf pdf-in.tar test.pdf kernel32.dll)
        mv "$TMP/pdf-in.tar" .
        rm -rf "$TMP"
    else
        echo "skipping pdf-in.tar -- corpus test.pdf or DLL not found"
    fi
fi

# zip-with-pdf.zip — 1 stored PDF + 1 deflate-9 DLL. Mirrors zip-with-png.zip
# shape; tests pack_pdf dispatch on stored ZIP entries (2026-07-17).
if want zip-with-pdf.zip; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/test.pdf" ] && [ -f "$CORPUS/kernel32.dll" ]; then
        python - "$CORPUS" <<'PY'
import sys, zipfile
c = sys.argv[1]
with zipfile.ZipFile('zip-with-pdf.zip', 'w') as z:
    z.write(f'{c}/test.pdf', 'test.pdf', compress_type=zipfile.ZIP_STORED)
    z.write(f'{c}/kernel32.dll', 'kernel32.dll', compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
PY
    else
        echo "skipping zip-with-pdf.zip -- corpus test.pdf or DLL not found"
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

# xz-in.tar — tar containing a .xz file + a plain DLL. Tests OP_XZ_STORE
# routing inside pack_tar.
if want xz-in.tar; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/ntdll.dll" ] && [ -f "$CORPUS/kernel32.dll" ]; then
        TMP=$(mktemp -d)
        cp "$CORPUS/ntdll.dll" "$TMP/" && xz -9e --threads=1 "$TMP/ntdll.dll"
        cp "$CORPUS/kernel32.dll" "$TMP/"
        (cd "$TMP" && tar cf xz-in.tar ntdll.dll.xz kernel32.dll)
        mv "$TMP/xz-in.tar" .
        rm -rf "$TMP"
    else
        echo "skipping xz-in.tar -- corpus DLLs not found"
    fi
fi

# zst-in.tar — tar containing a .zst file + a plain DLL. Tests OP_ZSTD_STORE
# routing inside pack_tar.
if want zst-in.tar; then
    CORPUS="${ZXLE_CORPUS:-../../../Zxl/tests}"
    if [ -f "$CORPUS/ntdll.dll" ] && [ -f "$CORPUS/kernel32.dll" ]; then
        TMP=$(mktemp -d)
        cp "$CORPUS/ntdll.dll" "$TMP/"
        zstd -19 --long=27 -q -f -o "$TMP/ntdll.dll.zst" "$TMP/ntdll.dll"
        rm "$TMP/ntdll.dll"
        cp "$CORPUS/kernel32.dll" "$TMP/"
        (cd "$TMP" && tar cf zst-in.tar ntdll.dll.zst kernel32.dll)
        mv "$TMP/zst-in.tar" .
        rm -rf "$TMP"
    else
        echo "skipping zst-in.tar -- corpus DLLs not found"
    fi
fi

# mixed.tar.zst — zstd -19 --long=27 wrap of mixed.tar. Tests M3h-zsttar:
# zstd-wrapped tar routed through pack_tar so per-entry payloads get
# format-aware treatment. zstd is weaker than xz, so xz-9e on a .zst is also
# stuck at ~1.25 MB while inner-pack_tar gets to ~1.19 MB — real headline win.
if want mixed.tar.zst; then
    if [ -f mixed.tar ]; then
        zstd -19 --long=27 -q -f -o mixed.tar.zst mixed.tar
    else
        echo "skipping mixed.tar.zst -- mixed.tar not present"
    fi
fi

# mixed.tar.zst3 — default-level (zstd -3) wrap of mixed.tar. Tests M3h
# ladder coverage: distros / general CLI use commonly produces default-level
# .zst, not -19. Without the ladder, this would fall through to KIND_OPAQUE.
if want mixed.tar.zst3; then
    if [ -f mixed.tar ]; then
        zstd -3 -q -f -o mixed.tar.zst3 mixed.tar
    else
        echo "skipping mixed.tar.zst3 -- mixed.tar not present"
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

# mixed.tar.xz — xz -9e wrap of mixed.tar. Tests M3i-xztar: xz-wrapped tar
# routed through pack_tar so per-entry payloads get format-aware treatment.
# xz on a mixed-content tar is already strong, so the headline is closer to
# tie than the bz2/zst variants — the value is correctness coverage of the
# most common Linux source / kernel / .deb-data format.
if want mixed.tar.xz; then
    if [ -f mixed.tar ]; then
        xz -9e -k -c --threads=1 mixed.tar > mixed.tar.xz
    else
        echo "skipping mixed.tar.xz -- mixed.tar not present"
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
