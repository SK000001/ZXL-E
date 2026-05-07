# ZXL-E

Recursive format-aware transform pipeline for general-purpose compression.

Goal: be the smallest archive across **every** file type, not just one. Sister project to [ZXL](../Zxl) (which targets PE binaries specifically). ZXL-E uses ZXL as one of its backends when it detects PE streams.

## Status (2026-05-08, M5 --slow shipped)

Default mode (xz-9e final-step) ties or beats xz-9e on the standard Silesia corpus and beats it by 6–60% on container-shaped artifacts. Optional `--slow` mode (zpaq -m5 final-step) matches the SOTA general-purpose codec on Silesia (ratio 0.1891) and stacks the gain on top of container unwrap (−29% to −47% vs xz-9e baseline on container fixtures).

| Stage | Status |
|---|---|
| M0 Phase-0 measurement | done — ZIP unwrap saves 20.45% over opaque-xz-9e on a 3-DLL test ZIP |
| M1 Walking skeleton (manifest + solid stream) | shipped; per-file 0.3453, solid 0.3391 on 8-file corpus (beats xz-9e 0.3524) |
| M2 ZIP-family unwrap (zlib-DEFLATE) | shipped — −20.45% vs xz-9e on pe-deflate.zip; −19.23% on sample.docx |
| M3a DEFLATE recompressor (preflate) | shipped — −20.78% vs xz-9e on zlib-L6 ZIP fixture |
| M3b JPEG recompressor (brunsli) | shipped — −27.14% vs xz-9e on synth.jpg; −19.85% on JPEG-in-ZIP fixture |
| M3c-mp3 MP3 recompressor (packMP3) | shipped — −13.04% vs xz-9e on synth.mp3 |
| M3c-png PNG IDAT recompressor (zlib-L9 / preflate) | shipped — −34.04% vs xz-9e on test.png; −22.07% on PNG-in-ZIP fixture |
| M3d gzip wrapper (zlib-L9 / preflate) | shipped — −16.69% vs xz-9e on ntdll.dll.gz |
| M3e ustar tar (per-entry format dispatch) | shipped — ties xz-9e on mixed.tar; closes the 5%+ gap to xz-9e that opaque tar would leave |
| M3e-targz gzip-wrapped tar | shipped — −21.66% vs xz-9e on mixed.tar.gz |
| M3e-tar gzip-in-tar (OP_GZIP_STORE) | shipped — −14.68% vs xz-9e on gz-in.tar |
| M3f-ar Unix archive (.a / .deb) | shipped — −18.37% on synthetic .deb (gz inner); ~tie on real `hello_2.10-3` (xz inner, dpkg-deb's lzma2 non-preset) |
| M3g-bz2tar bzip2-wrapped tar | shipped — −20.51% vs xz-9e on mixed.tar.bz2 |
| M3g bz2-in-tar (OP_BZ2_STORE) | shipped — −11.53% vs xz-9e on bz2-in.tar |
| M3h-zsttar zstd-wrapped tar | shipped — −21.44% on default-level mixed.tar.zst3; −11.57% on level-19 mixed.tar.zst |
| M3i-xztar xz-wrapped tar | shipped — −6.68% vs xz-9e on mixed.tar.xz |
| M3j-store-ops in-tar/in-ar `.xz` / `.zst` | shipped — completes the OP_*_STORE family |
| min-pack fallthrough | shipped — runs unwrap + force_opaque per pack and keeps the smaller |
| ZXLE_VER 3 final-step xz-9e | shipped — solid stream is xz -9e --threads=1 instead of zstd-19 long=27 |
| M4 Cross-stream content-defined ordering | parked — solid window already spans the corpus |
| **M5 `--slow` zpaq-m5 final-step** | **shipped** — Silesia 0.1891 (matches zpaq -m5); −29% to −47% vs xz-9e on container fixtures |

## Headline numbers (2026-05-08)

| Fixture | xz-9e | zxle (default) | zxle --slow |
|---|---|---|---|
| 8-file corpus (per-file ratio) | 0.3524 | **0.3453** | (not measured per-file) |
| Silesia 211 MB (ratio) | 0.2284 | 0.2284 | **0.1891** (matches zpaq -m5) |
| pe-deflate.zip | — | −20.45% | **−32.73%** vs xz-9e |
| sample.docx | — | −19.23% | **−47.41%** vs xz-9e |
| ntdll.dll.gz | — | −16.69% | **−29.43%** vs xz-9e |
| mixed.tar.gz | — | −21.66% | **−30.65%** vs xz-9e |
| mixed.deb | — | −18.37% | **−31.16%** vs xz-9e |
| sample.jar | — | −59.58% | −57.07% (small input; default wins) |

## Architecture

Four-stage pipeline:

1. **Recursive container unwrap** — peel ZIP / tar / ar / .deb / gzip / bzip2 / zstd / xz down to raw streams plus a recipe to rebuild byte-identical originals.
2. **Per-stream format-aware recompression** — DEFLATE → preflate (or zlib-L9 redeflate fast path), JPEG → brunsli, PNG IDAT → preflate over inflated pixels, MP3 → packMP3, PE → ZXL.
3. **Cross-stream solid mode** — concatenate the inflated raw bytes of all unwrapped streams; finalize with xz -9e (default) or zpaq -m5 (`--slow`).
4. **min-pack fallthrough** — every pack runs both the unwrap path and an all-opaque path; the smaller wins. Saves us from regressions on tightly-deflated tiny inputs.

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
zxle pack [--slow] out.zxle file1 file2 ...
zxle unpack out.zxle outdir/
```

Default mode finalizes the solid stream with `xz -9e --threads=1`. `--slow` finalizes with `zpaq -m5` (cmix-class context mixing); 5–10× slower pack but dense enough to match zpaq -m5 on the standard Silesia corpus while still capturing the container-unwrap wins. The format flag rides in the manifest header, so `unpack` auto-detects which final-step codec was used.

## See also

- [roadmap.md](roadmap.md) — current state, what's shipped, what's next, tried-and-reverted graveyard.
- [graph.md](graph.md) — local-only source-tree index (gitignored).
- [tests/bench.sh](tests/bench.sh) — bench script. Set `ZXLE_SILESIA=1` to run the 211 MB Silesia corpus; `ZXLE_SLOW=1` to run `--slow` against headline fixtures.
