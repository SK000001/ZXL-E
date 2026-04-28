# ZXL-E

Recursive format-aware transform pipeline for general-purpose compression.

Goal: be the smallest archive across **every** file type, not just one. Sister project to [ZXL](../Zxl) (which targets PE binaries specifically). ZXL-E uses ZXL as one of its backends when it detects PE streams.

## Status (2026-04-29, M2 shipped)

| Stage | Status |
|---|---|
| M0 Phase-0 measurement | done — ZIP unwrap saves 20.45% over opaque-xz-9e on a 3-DLL test ZIP |
| M1 Walking skeleton (manifest + solid zstd-19) | shipped — solid 0.3655 vs xz-9e 0.3524 on 8-file corpus |
| M2 ZIP-family unwrap (zlib-DEFLATE) | shipped — −15.04% vs xz-9e on pe-deflate.zip |
| M3 Per-stream recompressors (brunsli, cjxl, reflate) | next |
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
make
```

Requires `gcc`, system `zstd`, and `python` (for ZIP unwrap in later milestones).

## Use (M1 only)

```
zxle pack out.zxle file1 file2 ...
zxle unpack out.zxle outdir/
```

M1 packs arbitrary files with a manifest and a single solid zstd-19 payload — no format-aware unwrap yet. That arrives in M2.

## See also

- [roadmap.md](roadmap.md) — current state, what's shipped, what's next, tried-and-reverted graveyard.
- [tests/bench.sh](tests/bench.sh) — bench script.
