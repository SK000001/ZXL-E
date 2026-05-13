# ZXL-E

Recursive format-aware transform pipeline for general-purpose compression.

Goal: be the smallest archive across **every** file type, not just one. Sister project to [ZXL](../Zxl) (which targets PE binaries specifically). ZXL-E uses ZXL as one of its backends when it detects PE streams.

## Status (2026-05-14, XLSX + PPTX bench shipped)

Default mode (xz-9e final-step + BCJ-x86 sub-stream for PE/ELF content) ties or beats xz-9e on the standard Silesia corpus and beats it by 6–60% on container-shaped artifacts. Optional `--slow` mode (zpaq -m5 final-step) matches the SOTA general-purpose codec on Silesia (ratio 0.1891) and stacks the gain on top of container unwrap (−29% to −47% vs xz-9e baseline on container fixtures). Optional `--fast` mode (xz -T0 --block-size=8MiB final-step) gives **5.9–9.07× pack speedup** at +3% size cost; speedup grows with input — 9.07× at 1 GB. Measured up to 1 GB (`ZXLE_GIANT=1`); no solid-mode cliff past the 128 MiB long-window.

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

## Headline numbers (2026-05-14)

| Fixture | xz-9e | zxle (default) | zxle --slow |
|---|---|---|---|
| 8-file corpus (per-file ratio) | 0.3524 | **0.3383** | (not measured per-file) |
| 8-file corpus (solid ratio) | — | **0.3326** | — |
| Silesia 211 MB (ratio) | 0.2284 | 0.2269 | **0.1891** (matches zpaq -m5) |
| GIANT 1.06 GB (ratio) | 0.2283 | **0.2283** (+0.00% vs xz-9e) | — (impractical) |
| pe-deflate.zip | — | **−22.44%** | **−32.73%** vs xz-9e |
| pe-deflate-l6.zip | — | **−22.76%** | **−33.00%** vs xz-9e |
| sample.docx | — | −19.23% | **−47.41%** vs xz-9e |
| sample.xlsx | — | **−19.35%** | — |
| sample.pptx | — | **−22.68%** | — |
| ntdll.dll.gz | — | **−19.23%** | **−29.43%** vs xz-9e |
| mixed.tar.gz | — | **−22.92%** | **−30.65%** vs xz-9e |
| mixed.tar.bz2 | — | **−21.79%** | — |
| mixed.tar.zst3 (default-3) | — | **−22.70%** | — |
| mixed.tar.zst (level-19) | — | **−12.95%** | — |
| mixed.deb | — | **−20.70%** | **−31.16%** vs xz-9e |
| zip-with-jpeg.zip | — | **−21.95%** | — |
| zip-with-png.zip | — | **−23.49%** | — |
| gz-in.tar | — | **−17.09%** | — |
| bz2-in.tar | — | **−14.09%** | — |
| xz-in.tar | — | **−4.22%** | — |
| zst-in.tar | — | **−8.47%** | — |
| sample.jar | — | −59.33% | −59.33% (--slow tier picks default) |

`--fast` pack-time speedup (size cost +2.8–3.2%, ratio unchanged in the manifest format):

| Fixture | default pack | --fast pack | speedup |
|---|---:|---:|---:|
| silesia mozilla (51 MB single) | 23.4 s | 4.0 s | **5.9×** |
| Silesia 211 MB | 247 s | 42 s | **5.9×** |
| GIANT 1.06 GB | 617 s | 68 s | **9.07×** |

## Architecture

Five-stage pipeline:

1. **Recursive container unwrap** — peel ZIP / tar / ar / .deb / gzip / bzip2 / zstd / xz down to raw streams plus a recipe to rebuild byte-identical originals.
2. **Per-stream format-aware recompression** — DEFLATE → preflate (or zlib-L9 redeflate fast path), JPEG → brunsli, PNG IDAT → preflate over inflated pixels, MP3 → packMP3.
3. **Per-OP bucket routing (M6 v3)** — every recipe op carries a u8 bucket byte; PE/ELF bytes (sniffed on inflated payload) route to a dedicated sub-stream finalized with `xz -9e --x86` (BCJ filter); PNG pixel data, text, and already-compressed bytes stay in the main bucket. Mixed-content containers (DLL+image inside one tar/deb) split across both buckets per-entry.
4. **Cross-stream solid mode** — main bucket finalized with xz -9e (default) or zpaq -m5 (`--slow`); BCJ bucket always finalized with xz -9e --x86.
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

`make all-deps` is the umbrella target: `preflate-deps brunsli-deps packmp3-deps zpaq-deps precomp-deps real-fixtures`. Each is also runnable individually if you want to skip something (e.g. only `preflate-deps` is required for the build itself; brunsli/packmp3/zpaq/precomp are only needed for their respective format routes / competitor benches).

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
make zpaq-deps       # optional: zpaq for --slow mode
make precomp-deps    # optional: precomp v0.4.7 competitor in bench
make real-fixtures   # optional: silesia + real .deb + real .tar.xz
make
```

brunsli's `cbrunsli`/`dbrunsli`, packMP3, and zpaq must be on `PATH` at runtime for their respective routes to engage; `tests/bench.sh` auto-prepends `third_party/{brunsli/build/artifacts,packmp3/source,zpaq}` so locally-built copies just work.

## Use

```
zxle pack [--slow] [--fast] out.zxle file1 file2 ...
zxle unpack out.zxle outdir/
```

Default mode finalizes the solid stream with `xz -9e --threads=1`. `--slow` finalizes with `zpaq -m5` (cmix-class context mixing); 5–10× slower pack but dense enough to match zpaq -m5 on the standard Silesia corpus while still capturing the container-unwrap wins. `--fast` finalizes with `xz -9e --threads=0 --block-size=8MiB` (one worker per logical CPU, 8 MiB blocks); ~6–9× faster pack at +3% size cost, scales better with input size. `--slow` codec choice rides in the manifest header so `unpack` auto-detects; `--fast` only changes the encoder side and `xz -d` consumes the multi-block stream transparently.

## See also

- [roadmap.md](roadmap.md) — current state, what's shipped, what's next, tried-and-reverted graveyard.
- [graph.md](graph.md) — local-only source-tree index (gitignored).
- [tests/bench.sh](tests/bench.sh) — bench script. Set `ZXLE_SILESIA=1` for the 211 MB Silesia corpus (default + --fast + optional --slow), `ZXLE_SLOW=1` to add `--slow` against headline fixtures, `ZXLE_GIANT=1` for the 1.06 GB constructed bench.
