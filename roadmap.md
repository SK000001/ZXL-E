# ZXL-E Roadmap

Single source of truth for where ZXL-E is, where it's going, and what not to retry.

---

## Current state (2026-04-29, project inception — walking skeleton)

No bench numbers yet. The walking skeleton (M1) ships first; numbers land with it.

**Phase-0 measurements (2026-04-29)** taken before scaffolding:

| Measurement | Result |
|---|---|
| Heterogeneous solid mode (8 mixed files, sum-individual vs solid xz-9e) | −1.80% |
| Similar-file solid mode (3 PE DLLs, sum-individual vs solid xz-9e) | −2.26% |
| **ZIP unwrap + solid vs opaque-xz-9e on a DEFLATE-9 ZIP of 3 PE DLLs** | **−20.45%** |
| Already-compressed wall (xz-9e and zstd-19 on PDF/PNG) | PDF 0.94, PNG 0.97 |

**Reading:** the headline win is container unwrapping (~20% on ZIP-family files). Solid mode alone is small. Already-optimal media cannot be touched without format-specific recompressors. Average across a typical mixed corpus: plausibly 10–20% smaller than xz-9e once M2+M3 land.

---

## Architecture

Four-stage pipeline, each stage known in isolation; integrated product is the novel contribution.

1. **Recursive container unwrap** — peel ZIP/tar/7z/MSI/PE/MP4/etc. to raw streams + byte-identical-rebuild recipe.
2. **Per-stream format-aware recompression** — route each stream to its strongest known recompressor.
3. **Cross-stream solid mode with content-defined ordering** — cluster similar streams adjacent.
4. **Neural-residual fallback** — small autoregressive predictor on whatever's left.

---

## Roadmap

### M1 — Walking skeleton (in progress)
**Branch:** `feat/m1-skeleton` (will be the first feature branch on the new repo).

Scope:
- `zxle pack <out.zxle> <files...>` and `zxle unpack <in.zxle> <outdir>`.
- File format: `[magic "ZXLE"][ver][flags][manifest_size][manifest][zstd-19 payload]`.
- Manifest stores file name, size, mode for each entry; payload is the solid concatenation passed through `zstd -19 --long=27`.
- Round-trip: extract files byte-identical to inputs.
- No format-aware unwrap yet. Treats every input as opaque bytes.

Success criterion: round-trip OK on the existing ZXL test corpus + the python-built `pe-deflate.zip`. Ratio should match xz-9e within a few percent on similar files.

### M2 — ZIP-family unwrap handler
**Branch:** `feat/m2-zip-unwrap` · **Expected:** −15 to −25% on ZIP-family inputs.

Detect ZIP magic. Parse central directory. Extract per-entry compressed bytes + local file headers + CD entries verbatim into the recipe. Decompress entry payloads to raw bytes for the solid pass. On unpack: re-DEFLATE each entry with same params, verify CRC matches CD, fall back to storing original compressed bytes if not byte-identical.

Covers: `.zip`, `.docx`, `.xlsx`, `.pptx`, `.jar`, `.apk`, `.epub`, `.odt`.

Failure mode: DEFLATE encoder nondeterminism. zlib's level-9 output is reproducible from same input given same zlib version, but third-party deflators (7-zip, kzip, AdvanceCOMP) produce different bytes. Mitigation: store original compressed bytes when re-DEFLATE-then-CRC-mismatch.

### M3 — Per-stream format-aware recompressors
**Branch:** `feat/m3-recompressors` · **Expected:** large wins per stream.

Route each stream to its strongest recompressor:
- DEFLATE streams (inside ZIP, PDF, PNG, gzip): reflate / grittibanzli (~25%).
- JPEG streams: brunsli (−22%).
- PNG: cjxl lossless (−10 to −30%).
- PE streams (.exe / .dll detected by MZ + PE signature): shell out to ZXL.
- MP3: packMP3 (−25%).

Each recompressor lives behind an availability check; missing recompressors fall through to opaque-zstd.

### M4 — Cross-stream content-defined ordering
**Branch:** `feat/m4-content-ordering` · **Expected:** −2 to −10% beyond M3 on multi-file inputs.

After unwrap+recompress, compute MinHash / SimHash on each stream. Cluster, then order so similar streams are adjacent before the solid pass. Solid mode + content ordering captures cross-document boilerplate (e.g., DOCX schema across multiple Office docs).

### M5 — Neural residual fallback (optional)
**Branch:** `feat/m5-neural` · **Expected:** −5 to −15% on residuals where zstd is already near optimal.

Small autoregressive byte predictor (NNCP-class). Default-off due to slowdown; user opts in with `--slow`.

---

## Tried and reverted

(empty — project just started)

---

## Constraints / lessons learned

(empty — project just started)

---

## Quick-reference: project specifics

- **Language:** C, gcc -O3.
- **Backend deps (M1):** system `zstd`. M2+ adds `python` for ZIP, then format-specific tools.
- **File extension:** `.zxle`.
- **Magic bytes:** `Z X L E` (4 bytes), version byte, flags byte.
- **Test corpus:** mirrors ZXL's `tests/` plus a few container-format samples added for M2.
