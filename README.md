# ZXL-E

Recursive format-aware transform pipeline for general-purpose compression.

Goal: be the smallest archive across **every** file type, not just one. Sister project to [ZXL](../Zxl) (which targets PE binaries specifically). ZXL-E uses ZXL as one of its backends when it detects PE streams.

## Status (2026-05-01, M3c-png shipped)

| Stage | Status |
|---|---|
| M0 Phase-0 measurement | done — ZIP unwrap saves 20.45% over opaque-xz-9e on a 3-DLL test ZIP |
| M1 Walking skeleton (manifest + solid zstd-19) | shipped — solid 0.3655 vs xz-9e 0.3524 on 8-file corpus |
| M2 ZIP-family unwrap (zlib-DEFLATE) | shipped — −15.04% vs xz-9e on pe-deflate.zip; −18.64% on sample.docx |
| M3a DEFLATE recompressor (preflate) | shipped — −15.39% vs xz-9e on zlib-L6 ZIP fixture |
| M3b JPEG recompressor (brunsli) | shipped — −27.15% vs xz-9e on synth.jpg; −15.25% on JPEG-in-ZIP fixture |
| M3c-mp3 MP3 recompressor (packMP3) | shipped — −13.05% vs xz-9e on synth.mp3 |
| M3c-png PNG IDAT recompressor (zlib-L9 / preflate) | shipped — −22.06% vs xz-9e on test.png |
| M4 Cross-stream content-defined ordering | pending |
| M5 Neural residual fallback | pending |

## Architecture (target)

Four-stage pipeline:

1. **Recursive container unwrap** — peel ZIP/tar/7z/MSI/MP4/PE/etc. down to raw streams plus a recipe to rebuild byte-identical originals.
2. **Per-stream format-aware recompression** — DEFLATE → reflate, JPEG → brunsli, PNG → JPEG XL lossless, PE → ZXL, etc.
3. **Cross-stream solid mode with content-defined ordering** — cluster similar streams adjacent, single long-window pass.
4. **Neural-residual fallback** — small autoregressive model on whatever's left.

Each stage is known in isolation; the integrated product does not exist publicly.

## Build

```
make preflate-deps   # one-time: clones + patches + builds third_party/preflate
make brunsli-deps    # one-time: clones + builds third_party/brunsli (cbrunsli/dbrunsli)
make packmp3-deps    # one-time: clones + builds third_party/packmp3 (packMP3)
make
```

Requires `gcc`/`g++`, system `zstd`, `cmake` + `mingw32-make` (for the static libs), `xz`, and `python` (used by some test fixtures). brunsli's `cbrunsli`/`dbrunsli` must be on `PATH` at runtime for JPEG routing to engage; `tests/bench.sh` auto-detects the locally-built copies.

## Use (M1 only)

```
zxle pack out.zxle file1 file2 ...
zxle unpack out.zxle outdir/
```

M1 packs arbitrary files with a manifest and a single solid zstd-19 payload — no format-aware unwrap yet. That arrives in M2.

## See also

- [roadmap.md](roadmap.md) — current state, what's shipped, what's next, tried-and-reverted graveyard.
- [tests/bench.sh](tests/bench.sh) — bench script.
