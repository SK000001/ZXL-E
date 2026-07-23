# ZXL-E

Recursive format-aware transform pipeline for general-purpose compression.

Goal: the smallest archive across **every** file type that **provably restores** — every result is byte-identical-verified (per-entry crc32, hostile-input-hardened, CI-tested), which distinguishes it from repacker-scene tools with known reconstruction-failure classes. Sister project to [ZXL](../Zxl) (which targets PE binaries specifically). ZXL-E uses ZXL as one of its backends when it detects PE streams.

## Status (2026-07-23 — v8 redeflate ladder: Crown-A competitor losses flipped)

Default mode (xz-9e final-step + BCJ-x86 sub-stream for x86/x64 PE/ELF content) ties or beats xz-9e on the standard Silesia corpus and beats it by 5–82% on container-shaped artifacts — now including **PDFs** (FlateDecode scanner: −74% on the corpus PDF, −48% on a real arXiv paper), both standalone and **inside tar/ar/ZIP containers** (−24% to −34% on the PDF-in-container fixtures, −48% on an arXiv paper in a tar) — and the same scanner now sweeps **any opaque non-PE file** for embedded zlib streams (−40% on the asset-pack fixture). pack_zip's entry loop is parallel as of M7 step 5 (pe-deflate-l6 pack −36%), hostile malformed containers can no longer corrupt the solid stream (tests/hostile.sh), and a GitHub Actions workflow builds and benches the self-contained subset on Linux. Optional `--slow` mode (zpaq -m5 final-step) matches the SOTA general-purpose codec on Silesia (ratio 0.1891) and stacks the gain on top of container unwrap. Optional `--fast` mode (xz -T0 with input-scaled blocks) gives **5.4–12× pack speedup** at +0.6% (1 GB) to +2.1% (211 MB) size cost. Measured up to 1 GB (`ZXLE_GIANT=1`). The v7 wire format compresses the manifest — merged into the solid stream when that wins — and crc32-verifies every entry at unpack. Real-world data wins: **PyPI wheels −21% to −29%, Android APK −23%, npm tarball −11% vs xz-9e**. Competitor bench (2026-07-18, incl. kanzi-cpp, xtool, FreeArc): 7-Zip `-mx=9 -ms=on`, kanzi `-l 9`, and FreeArc `-mx` lose on every container (none unwraps deflate; FreeArc even loses to xz-9e). The one class where xtool (repacker-scene, preflate-based like us) beat us — deflate-parameter reproduction: PDF −0.98%, pure-python wheel −0.76%, APK −0.07% — is **closed as of 2026-07-23**: a probe showed those streams are all plain zlib **level-6** (Python zipfile / Android / git) that our L9-only fast path missed, and the shipped **v8 redeflate ladder** (`OP_REDEFLATE_P`) reproduces them exactly and stores two param bytes instead of a diff. Flipped: wheel −404 B, APK −9,932 B, test.pdf −420 B (now beats precomp+xz), plus a new free win class — **git packfiles −7,776 B** (wall-to-wall zlib, opaque to xz). **zxle now produces the smallest archive on every benched fixture.** Still opaque to us and everyone: Go `compress/flate` (Docker layers) — preflate can't split it (roadmap'd for preflate-rs). Remaining gaps: razor (binary now located in the Ultra F-A bundle, unbenched) and zpaq-class density on long text — see roadmap.

| Stage | Status |
|---|---|
| M0 Phase-0 measurement | done — ZIP unwrap saves 20.45% over opaque-xz-9e on a 3-DLL test ZIP |
| M1 Walking skeleton (manifest + solid stream) | shipped; per-file 0.3383, solid 0.3326 on 8-file corpus (beats xz-9e 0.3524 by −5.6%) |
| M2 ZIP-family unwrap (zlib-DEFLATE) | shipped — −22.44% vs xz-9e on pe-deflate.zip |
| M3a DEFLATE recompressor (preflate) | shipped — −22.76% vs xz-9e on zlib-L6 ZIP fixture |
| M3b JPEG recompressor (brunsli) | shipped — −27.15% vs xz-9e on synth.jpg; −21.95% on JPEG-in-ZIP fixture |
| M3c-mp3 MP3 recompressor (packMP3) | shipped — −13.05% vs xz-9e on synth.mp3 |
| M3c-png PNG IDAT recompressor (zlib-L9 / preflate) | shipped — −34.04% vs xz-9e on test.png; −23.26% on PNG-in-ZIP fixture |
| M3d gzip wrapper (zlib-L9 / preflate) | shipped — −19.23% vs xz-9e on ntdll.dll.gz |
| M3e ustar tar (per-entry format dispatch) | shipped — −6.67% vs xz-9e on mixed.tar |
| M3e-targz gzip-wrapped tar | shipped — −21.66% vs xz-9e on mixed.tar.gz |
| M3e-tar gzip-in-tar (OP_GZIP_STORE) | shipped — −17.10% vs xz-9e on gz-in.tar |
| M3f-ar Unix archive (.a / .deb) | shipped — −18.37% on synthetic .deb (gz inner); ~tie on real `hello_2.10-3` (xz inner, dpkg-deb's lzma2 non-preset) |
| M3g-bz2tar bzip2-wrapped tar | shipped — −20.51% vs xz-9e on mixed.tar.bz2 |
| M3g bz2-in-tar (OP_BZ2_STORE) | shipped — −14.09% vs xz-9e on bz2-in.tar |
| M3h-zsttar zstd-wrapped tar | shipped — −21.44% on default-level mixed.tar.zst3; −11.53% on level-19 mixed.tar.zst |
| M3i-xztar xz-wrapped tar | shipped — −6.68% vs xz-9e on mixed.tar.xz |
| M3j-store-ops in-tar/in-ar `.xz` / `.zst` | shipped — completes the OP_*_STORE family |
| min-pack fallthrough | shipped — runs unwrap + force_opaque per pack and keeps the smaller |
| ZXLE_VER 3 final-step xz-9e | shipped — solid stream is xz -9e --threads=1 instead of zstd-19 long=27 |
| M4 Cross-stream content-defined ordering | parked — solid window already spans the corpus |
| M5 `--slow` zpaq-m5 final-step | shipped — Silesia 0.1891 (matches zpaq -m5); −29% to −47% vs xz-9e on container fixtures |
| min-pack `--slow` per-fixture tier | shipped — `--slow` is now a strict pareto improvement over default |
| **M6 v1 BCJ x86 routing for KIND_OPAQUE** | **shipped** — PE DLLs gain ~3% on per-file pack (ZXLE_VER 3 → 4) |
| **M6 v2 container-aware BCJ routing** | **shipped** — +2.0–2.8 pp on pure-PE containers (ZIP/TAR/AR/GZIP/BZIP2/ZSTD/XZ); ZXLE_VER 4 → 5 |
| **Parser fuzz harness (`tests/fuzz.sh`)** | **shipped** — 700 mutations × 7 kinds clean; uncovered + fixed `raw_inflate_dyn` truncated-stream infinite-realloc hang |
| **M6 v3 per-OP bucket routing** | **shipped** — recipes carry per-OP bucket bytes; mixed-content fixtures gain 1.3–8.2 pp; ZXLE_VER 5 → 6 |
| **M7 step 1 parallel probe ladders** | **shipped** — pack_xz / pack_zst run candidates concurrently; real_coreutils.deb −58%, real_coreutils_src.tar.xz −38% pack time |
| **M7 step 2 parallel --slow + default tier** | **shipped** — cross-codec tier runs both tiers on threads; DOCX −31%, MP3 −51% pack time on --slow path |
| **M7 step 4 --fast flag** | **shipped** — `xz -T0 --block-size=8MiB` final-step; 5.9× at 51 MB, 9.07× at 1 GB; +3% size cost |
| **Large-corpus measurement (1 GB)** | **shipped** — `ZXLE_GIANT=1` validates no solid-mode cliff at 1 GB; zxle matches tar+xz-9e +0.00% |
| **XLSX + PPTX bench** | **shipped** — M2 ZIP path validated on OOXML beyond DOCX; sample.xlsx −19.35%, sample.pptx −22.68% vs xz-9e |
| **M8a CPU optimal-parse (scout)** | **partial pass** — SA-based parse ties zstd-19 at ≤1 MB, +1.6–2.3% at 10–30 MB; reframed as M8c entropy-backend swap (see roadmap) |
| **ZXLE_VER 7: compressed manifest + crc32 + 64-bit IO** | **shipped** — manifest xz'd (was raw; 65% of sample.jar archive), per-entry crc32 verified at unpack, 2 GiB Windows cap fixed; jar −51% vs v6, pptx −2.8%, tar family −1% |
| **OP_ZIP_STORE (zip-in-tar/ar recursion)** | **shipped** — JAR/ZIP inside tar/ar unwraps recursively; zip-in.tar −3.83% vs prior, −5.61% vs xz-9e |
| **BCJ arch guard** | **shipped** — only x86/x64 PE/ELF route to the xz --x86 bucket; ARM64 binaries no longer get the wrong filter |
| **--fast input-scaled blocks** | **shipped** — block = bucket/ncpus clamp [8,64] MiB; 1 GB size cost +3.24% → +0.58% at 5.4× speedup |
| **Pack/unpack hardening** | **shipped** — duplicate basenames refused at pack; zip-slip names refused at unpack |
| **7z + precomp-cn\|xz competitor bench** | **shipped** — 7z ties xz-9e on containers (zxle +15–47%); precomp\|xz is the close peer (zxle wins 5, ties 3, loses JAR/JPEG) |
| **JPEG codec race (brunsli vs packJPG)** | **shipped** — blobs carry a codec byte, both tried, smaller wins; synth.jpg 116,612 beats precomp+xz |
| **Merged-manifest layout + small-input layout race** | **shipped** — manifest rides in bucket 0's xz stream when that wins; sample.jar 2,804 beats precomp+xz 3,012; zxle now beats precomp\|xz on all 10 combo fixtures |
| **M3k KIND_PDF (FlateDecode scanner)** | **shipped** — zlib-scan + redeflate/preflate verify + embedded-JPEG race; test.pdf −74.0%, real arxiv.pdf −48.2% vs xz-9e, both beat precomp\|xz; corpus headline per-file 0.3383 → 0.3232 |
| **M3l PDF-in-container** | **shipped** — pack_pdf dispatch in tar/ar walkers + stored ZIP entries, nested recipe rides OP_ZIP_STORE (no format bump); pdf-in.tar −24.3%, zip-with-pdf.zip −34.0% vs xz-9e, both beat precomp\|xz and 7z |
| **M3m generic opaque flate scan** | **shipped** — M3k scanner runs on bucket-0 opaque files (5% coverage gate, PE/ELF excluded to keep BCJ); flate-blob.bin −40.45% vs xz-9e; scan cost <1% of pack (130–930 MB/s) |
| **Hostile-input bucket rollback** | **shipped** — mid-walk pack_zip/tar/ar failures no longer orphan solid bytes (crafted-ZIP repro: pre-fix RT FAIL → post-fix OK); guarded by tests/hostile.sh |
| **GitHub Actions CI** | **shipped** — ubuntu build + self-contained fixtures + hostile.sh + bench gate; forced Linux portability fixes in preflate-deps and make_fixtures.sh; **green on Linux** — first run caught the libpreflate _ftelli64 portability bug, patched in preflate-deps |
| **M7 step 5: entry-loop worker pools (zip + tar + ar)** | **shipped** — per-entry fragments spliced in order, output byte-identical; zip: pe-deflate-l6 −36% / pptx −38% pack time; tar/ar: gz-in.tar −13% / mixed.deb −12%; every per-entry recompression loop now parallel |
| **Crown-A bench (real-world corpus + freearc/kanzi/xtool)** | **shipped** — PyPI/npm/Docker/APK fixtures; kanzi & FreeArc lose everywhere, xtool beat us on 3 deflate-reproduction fixtures (−0.07% to −0.98%) |
| **v8 redeflate ladder (`OP_REDEFLATE_P`)** | **shipped** — probe found the 3 xtool losses are all zlib level-6; a bounded (level×memLevel×windowBits) ladder reproduces them exactly, storing 2 param bytes vs a preflate diff. Flips all losses (wheel −404 B, APK −9,932 B, test.pdf −420 B) + git packfiles −7,776 B; ZXLE_VER 7→8, 121/121 RT OK, no regressions |

## Headline numbers (2026-07-23, ZXLE_VER 8)

| Fixture | xz-9e | zxle (default) | zxle --slow |
|---|---|---|---|
| 8-file corpus (per-file ratio) | 0.3524 | **0.3232** | (not measured per-file) |
| 8-file corpus (solid ratio) | — | **0.3174** | — |
| test.pdf (in corpus) | — | **−74.0%** | — |
| arxiv.pdf 1706.03762 (real-world, local-only) | — | **−48.2%** | — |
| Silesia 211 MB (ratio) | 0.2284 | 0.2269 | **0.1891** (matches zpaq -m5) |
| GIANT 1.06 GB (ratio) | 0.2283 | **0.2283** (+0.00% vs xz-9e) | — (impractical) |
| pe-deflate.zip | — | **−22.45%** | **−32.73%** vs xz-9e |
| pe-deflate-l6.zip | — | **−22.76%** | **−33.00%** vs xz-9e |
| sample.docx | — | −19.29% | **−47.41%** vs xz-9e |
| sample.xlsx | — | **−19.88%** | — |
| sample.pptx | — | **−24.85%** | — |
| ntdll.dll.gz | — | **−19.23%** | **−29.43%** vs xz-9e |
| mixed.tar | — | **−9.12%** | — |
| mixed.tar.gz | — | **−23.72%** | **−30.65%** vs xz-9e |
| mixed.tar.bz2 | — | **−22.60%** | — |
| mixed.tar.zst3 (default-3) | — | **−23.50%** | — |
| mixed.tar.zst (level-19) | — | **−13.85%** | — |
| mixed.deb | — | **−21.38%** | **−31.16%** vs xz-9e |
| synth.jpg | — | **−27.68%** | — |
| zip-with-jpeg.zip | — | **−22.00%** | — |
| zip-with-png.zip | — | **−23.51%** | — |
| gz-in.tar | — | **−17.82%** | — |
| bz2-in.tar | — | **−14.28%** | — |
| xz-in.tar | — | **−4.56%** | — |
| zst-in.tar | — | **−8.83%** | — |
| zip-in.tar (OP_ZIP_STORE) | — | **−5.69%** | — |
| pdf-in.tar (M3l) | — | **−24.29%** | — |
| zip-with-pdf.zip (M3l) | — | **−34.04%** | — |
| flate-blob.bin (M3m opaque flate scan) | — | **−40.45%** | — |
| git packfile (kanzi clone, local-only, v8 ladder) | +0.02% | **−30.3%** (293,888 B vs xz-9e 421,700) | — |
| real_requests.whl (PyPI wheel, pure-py) | — | **−21.83%** (v8 ladder) | — |
| real_pydantic_core.whl (PyPI wheel, native) | — | **−28.58%** | — |
| real_newpipe.apk (Android APK) | — | **−22.75%** (v8 ladder) | — |
| real_express.tgz (npm tarball) | — | **−10.58%** | — |
| real_alpine_layer.tar.gz (Docker, Go-gzip) | — | −0.00% (opaque; Go-flate) | — |
| sample.jar | — | **−81.78%** (2,804 B) | −81.78% (--slow tier picks default) |

(--slow percentages are from the 2026-05-14 measurement on the v6 format; v7 shifts them slightly in zxle's favor.)

`--fast` pack-time speedup (input-scaled blocks, 2026-07-14, 16 logical CPUs, single-input packs):

| Fixture | default pack | --fast pack | speedup | size cost |
|---|---:|---:|---:|---:|
| silesia mozilla (51 MB single) | 23.4 s | 4.0 s | **5.9×** | +2.77% |
| silesia.tar 211 MB | 247 s | 20.6 s | **12×** | +2.10% |
| giant 1.06 GB | 617 s | 113 s | **5.4×** | **+0.58%** |

Per-fixture sweep (2026-07-17, `ZXLE_FAST=1 bash tests/bench.sh`): at container-fixture scale (≤10 MB) `--fast` costs **+8 bytes flat** (+0.00% to +0.29%) and changes pack time negligibly — buckets under the 8 MiB block clamp encode as one block either way. Its speedups begin once the solid bucket spans multiple blocks (tens of MB). Practical rule: `--fast` is always safe to use, and pays off on large inputs.

## Architecture

Five-stage pipeline:

1. **Recursive container unwrap** — peel ZIP / tar / ar / .deb / gzip / bzip2 / zstd / xz down to raw streams plus a recipe to rebuild byte-identical originals. Since v7, ZIP/JAR entries *inside* tar/ar recurse through the ZIP unwrapper too (OP_ZIP_STORE), and PDFs are scanned for embedded FlateDecode zlib streams and DCTDecode JPEGs (M3k KIND_PDF) — including PDFs inside tar/ar and stored ZIP entries (M3l).
2. **Per-stream format-aware recompression** — DEFLATE → zlib-L9 redeflate fast path, then a bounded zlib parameter ladder (v8 `OP_REDEFLATE_P`: level×memLevel×windowBits, catches level-6 producers like Python zipfile / Android / git), else preflate; JPEG → brunsli *and* packJPG raced per-image (smaller verified result wins), PNG IDAT → preflate over inflated pixels, MP3 → packMP3.
3. **Per-OP bucket routing (M6 v3)** — every recipe op carries a u8 bucket byte; x86/x64 PE/ELF bytes (machine field parsed, not just magic) route to a dedicated sub-stream finalized with `xz -9e --x86` (BCJ filter); PNG pixel data, text, non-x86 binaries, and already-compressed bytes stay in the main bucket. Mixed-content containers (DLL+image inside one tar/deb) split across both buckets per-entry.
4. **Cross-stream solid mode** — main bucket finalized with xz -9e (default) or zpaq -m5 (`--slow`); BCJ bucket always finalized with xz -9e --x86. The manifest (recipes + structural bytes) rides at the head of the main bucket's xz stream when that layout wins (below 8 MB both layouts are raced, smaller kept; v7 flags bit 1) or as its own xz block otherwise; every entry carries a crc32 that unpack verifies.
5. **min-pack fallthrough** — every pack runs both the unwrap path and an all-opaque path; the smaller wins. Saves us from regressions on tightly-deflated tiny inputs. With `--slow`, also tiers default-mode candidate on small inputs and keeps the smaller — guaranteeing pareto optimality.

Each stage is known in isolation; the integrated product does not exist publicly.

## Getting started

From a fresh clone, the one-command path is:

```
git clone <this-repo> ZXL-E && cd ZXL-E
make all-deps                         # ~10 min: fetches third_party libs, tools, real-world fixtures
make                                  # builds zxle.exe
bash tests/make_fixtures.sh           # generates synthetic fixtures (sample.docx, .jar, etc.)
bash tests/bench.sh                   # default bench (~5 min)
ZXLE_SILESIA=1 ZXLE_SLOW=1 bash tests/bench.sh   # full bench incl. Silesia + --slow (~30-60 min)
```

`make all-deps` is the umbrella target: `preflate-deps brunsli-deps packmp3-deps packjpg-deps zpaq-deps precomp-deps 7zip-deps real-fixtures`. Each is also runnable individually if you want to skip something (e.g. only `preflate-deps` is required for the build itself; brunsli/packmp3/packjpg/zpaq are only needed for their respective format routes, precomp/7zip only for the competitor bench sections).

Host requirements:
- `gcc` + `g++` (C11 / C++14)
- `cmake` + `mingw32-make` (used by preflate and brunsli builds)
- `git`, `curl`, `unzip`, `python`
- system `zstd`, `xz`, `bzip2`
- ~6 GB disk after `make all-deps` (most of it is the brunsli + preflate clones)

The 8-file headline corpus (per-file table + solid mode at the top of the bench) lives in the sister project [ZXL](../Zxl)'s `tests/` dir. If `../Zxl/tests/` doesn't exist, `bench.sh` skips that section and runs everything else; container fixtures, Silesia, real-world fixtures, and the competitor sections all work standalone.

## Build (manually, without `all-deps`)

```
make preflate-deps   # required: third_party/preflate (libpreflate.a)
make brunsli-deps    # optional: cbrunsli/dbrunsli for JPEG routing
make packmp3-deps    # optional: packMP3 for MP3 routing
make packjpg-deps    # optional: packJPG raced against brunsli for JPEGs
make zpaq-deps       # optional: zpaq for --slow mode
make precomp-deps    # optional: precomp v0.4.7 competitor in bench
make 7zip-deps       # optional: standalone 7zr for the 7-Zip competitor bench
make real-fixtures   # optional: silesia + real .deb + real .tar.xz
make
```

brunsli's `cbrunsli`/`dbrunsli`, packJPG, packMP3, and zpaq must be on `PATH` at runtime for their respective routes to engage; `tests/bench.sh` auto-prepends `third_party/{brunsli/build/artifacts,packjpg/source,packmp3/source,zpaq}` so locally-built copies just work (zxle itself also auto-discovers them next to its binary).

## Use

```
zxle pack [--slow] [--fast] out.zxle file1 file2 ...
zxle unpack out.zxle outdir/
```

Default mode finalizes the solid stream with `xz -9e --threads=1`. `--slow` finalizes with `zpaq -m5` (cmix-class context mixing); 5–10× slower pack but dense enough to match zpaq -m5 on the standard Silesia corpus while still capturing the container-unwrap wins. `--fast` finalizes with `xz -9e --threads=0` and an input-scaled block size (`bucket/ncpus`, clamped [8 MiB, 64 MiB]); 5–12× faster pack at +0.6% (1 GB) to +2.8% (51 MB) size cost. `--slow` codec choice rides in the manifest header so `unpack` auto-detects; `--fast` only changes the encoder side and `xz -d` consumes the multi-block stream transparently. Unpack verifies a per-entry crc32 and refuses archives whose entry names would escape the output directory; pack refuses duplicate basenames.

## Direction

The target is owning the pareto frontier — nobody smaller at any time budget, nobody faster at our size — not a single "fastest" point (lz4-class speed at max ratio is off the table by physics). The strategic program (roadmap.md, set 2026-07-17), in funded order: (1) bench the remaining serious competitors (freearc, kanzi, xtool/razor) on the world's dominant archive classes (Python wheels, Docker layers, APKs, game assets); (2) fix whatever loses; (3) a text middle tier between default and `--slow`; (4) remaining pack-time cleanup; (5) the M8 line — own LZ pipeline + LZMA-class entropy + GPU match-finding, targeting xz-9e ratio at 3–10× xz speed; (6) format freeze + published reproducible results so the claim is checkable by anyone. The absolute-ratio crown (cmix-class neural, impractical by construction) is consciously deferred.

## See also

- [roadmap.md](roadmap.md) — current state, what's shipped, what's next, tried-and-reverted graveyard.
- [graph.md](graph.md) — local-only source-tree index (gitignored).
- [tests/bench.sh](tests/bench.sh) — bench script. Set `ZXLE_SILESIA=1` for the 211 MB Silesia corpus (default + --fast + optional --slow), `ZXLE_SLOW=1` to add `--slow` against headline fixtures, `ZXLE_GIANT=1` for the 1.06 GB constructed bench. Deterministic tool results (xz/zstd baselines, competitor sizes + RT) are cached in `tests/baseline/.cache` keyed by fixture and tool version — warm runs only re-measure zxle itself (~172 s vs ~300 s cold); `ZXLE_FRESH=1` forces a full recompute.
