# ZXL-E Roadmap

Single source of truth for where ZXL-E is, where it's going, and what not to retry.

---

## Current state (2026-04-30, M3-preflate shipped)

M1 + M2 + the first M3 sub-milestone (preflate-backed DEFLATE recompressor) ship end-to-end. Round-trip OK across the 8-file corpus and both ZIP fixtures.

**Headline M3a result:** `zxle` is **−15.39% vs xz-9e** on a zlib-L6 ZIP of 3 PE DLLs (2,276,846 B → 1,924,666 B). All 3 entries miss M2's L9-redeflate fast path, get split via preflate (215 B reconstruction info on the largest stream), and the unpacked bytes flow into the solid zstd-19 stream.

**Headline M2 result (preserved):** `zxle` is **−15.04% vs xz-9e** on a DEFLATE-9 ZIP of 3 PE DLLs (2,265,566 B → 1,923,274 B). All 3 entries re-deflate byte-identically with zlib L9/mem8/default-strategy, so they enter the solid zstd-19 stream as raw bytes.

**Bench (2026-04-29, 8-file mixed corpus, 6,910,547 B):**

| Codec | Ratio vs orig |
|---|---|
| zxle (per-file)  | 0.3721 |
| zstd-19          | 0.3721 |
| xz-9e            | 0.3524 |
| zxle solid       | 0.3655 (1.78% smaller than per-file) |

The 8-file corpus contains no containers, so M2 doesn't change these numbers vs M1 — only adds 1 byte of overhead per file (the kind tag).

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

### M1 — Walking skeleton (shipped 2026-04-29)
- `zxle pack` / `zxle unpack` working, manifest + solid zstd-19 payload.
- Magic `ZXLE` + ver + flags header.
- Round-trip OK on full corpus; solid ratio 0.3655 vs xz-9e 0.3524.

### M2 — ZIP-family unwrap handler (shipped 2026-04-29)
- ZIP detection + CD parsing in C, zlib link.
- Per-entry: try re-deflate at zlib L9/mem8/default-strategy, byte-compare to original. Match → raw bytes go to solid; mismatch → original deflate stream verbatim in recipe.
- Container format: bumped to v2. Per-entry kind byte (0=opaque, 1=zip-unwrap). For kind=1 the manifest also carries a recipe of STRUCT/REDEFLATE/STORE ops.
- Out of scope this milestone: ZIP64, encryption, DEFLATE64/BZIP2/LZMA, prefix bytes (self-extractors). All trigger fallback to KIND_OPAQUE so round-trip is preserved.
- Result: **−15.04% vs xz-9e** on pe-deflate.zip; mixed corpus unaffected.

Real-world DEFLATE drift (third-party deflators like 7-zip, kzip, AdvanceCOMP not matching zlib's L9 output) will reduce the win on those archives — store-orig fallback ensures correctness, not size. Confirm with a wider ZIP fixture set in M3.

### M3 — Per-stream format-aware recompressors

Route each stream to its strongest recompressor. Tracked as sub-milestones.

#### M3a — DEFLATE via preflate (shipped 2026-04-30)
- Vendored `third_party/preflate` (deus-libri 0.3.5; patched `<cstdint>` include in `preflate_seq_chain.h`). Built statically, linked via a small C ABI shim (`src/preflate_shim.cpp`).
- New recipe op `OP_PREFLATE` (0x03): per ZIP entry, when zlib-L9 byte-redeflate fails, try `preflate_decode` and verify by `preflate_reencode` cmp. Success → unpacked bytes go to solid; reconstruction blob (typically <300 B for MB-scale streams) goes in the recipe.
- ZIP entries that preflate also can't handle (rare) fall through to the existing STRUCT-store-orig path.
- Measured: −15.39% vs xz-9e on `pe-deflate-l6.zip` (zlib-L6 ZIP that completely misses M2's fast path); existing M2 fixture preserved at −15.04%.
- Build dep: `make preflate-deps` clones + patches + builds preflate via cmake/MinGW. Without it the static lib is missing and the main `make` errors.

#### M3b — pending sub-milestones
**Branch:** `feat/m3-*` · **Expected:** large wins per stream.

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
