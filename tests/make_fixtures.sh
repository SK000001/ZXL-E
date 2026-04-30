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

# synth.mp3 — 10s 440Hz tone @ 128k. Motivates M3c packMP3 sub-milestone.
if want synth.mp3; then
    ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:duration=10" \
        -b:a 128k synth.mp3
fi

echo "fixtures in tests/corpus/:"
ls -l tests/corpus/ 2>/dev/null || ls -l .
