# ZXL-E Roadmap

Single source of truth for where ZXL-E is, where it's going, and what not to retry.

---

## Current state (2026-05-01, M3c-png shipped)

M1 + M2 + M3a (preflate) + M3b (brunsli) + M3c-mp3 (packMP3) + M3c-png (zlib-L9 / preflate over IDAT) ship end-to-end. Round-trip OK across the 8-file corpus, both ZIP fixtures, the JPEG fixture, the DOCX/JAR fixtures, the MP3 fixture, and `test.png`.

**Headline M3c-png result:** `zxle` is **−22.06% vs xz-9e** on `test.png` (157,441 B → 119,297 B). The IDAT zlib stream is concatenated and inflated; if zlib-L9 default-strategy redeflate matches, the inflated filtered-pixel bytes go straight to the solid zstd-19 stream (mode 0). Otherwise the deflate body is split via preflate (mode 1, with the original zlib header + adler stored in the recipe). PNG entries do contribute to solid (the win is zstd-19 long-window-27 over filtered-pixel bytes); chunk boundaries are reconstructed from per-IDAT lengths in the recipe and CRCs are recomputed. Failures fall through to KIND_OPAQUE.

Corpus per-file average vs orig: 0.3721 → **0.3673**. Solid: 0.3655 → **0.3608**. All non-PNG fixtures unchanged.

**Headline M3c-mp3 result:** `zxle` is **−13.05% vs xz-9e** on a 481,115 B synthesized music MP3 (zxle 398,756 → savings of 59,840 vs xz-9e). Top-level MP3 files are detected by ID3v2 tag or MPEG-1 Layer III sync word, routed through `packMP3`, verified by round-trip `packMP3` decode + cmp at pack time, and stored as a `.pmp` blob in the manifest (bypassing solid mode — MP3 frames hit the already-compressed wall under zstd, so no solid benefit is given up).

**Headline M3b result:** `zxle` is **−27.15% vs xz-9e** on a 162,822 B JPEG (synth.jpg → 117,473 B). Top-level JPEG files are detected by SOI marker, routed through `cbrunsli`, verified by round-trip `dbrunsli` + cmp at pack time, and stored as a brunsli blob in the manifest (bypassing solid mode entirely — JPEGs hit the already-compressed wall under zstd, so no solid benefit is given up).

**M3b-zip:** brunsli routing is also wired into the ZIP unwrap path for STORED JPEG entries. Mixed fixture (1 stored JPEG + 2 deflate-9 DLLs, 1,705,559 B): zxle 1,442,710 → **−15.25% vs xz-9e**.

**Corpus-expansion (2026-05-01):** added `sample.docx`, `sample.jar`, `synth.mp3` via `tests/make_fixtures.sh` to validate M2/M3a/M3b on non-PE ZIP content and to enable M3c-mp3 measurement.

| Fixture | orig | zxle | xz-9e | zxle vs xz-9e |
|---|---|---|---|---|
| sample.docx (8000-para WordML, ZIP/L6) | 585,600 | 476,517 | 585,696 | **−18.64%** |
| sample.jar (30 small classes, ZIP/L6) | 19,717 | 6,540 | 15,392 | **−57.51%** |
| synth.mp3 (30 s synth music, stereo @ 128 k) | 481,115 | 398,756 | 458,596 | **−13.05%** |
| test.png (corpus PNG) | 157,441 | 119,297 | 153,064 | **−22.06%** |

DOCX/JAR results confirm the M2+M3a unwrap path generalizes from PE-DLL ZIPs to real-world XML/class-file ZIPs (DOCX exceeds the PE-DLL win because XML deflates more thoroughly when re-fed to zstd-19 solid). MP3 result is M3c-mp3 (packMP3 routing). Note: the original 10 s 440 Hz tone was replaced by a 30 s synthesized stereo signal (sine + phaser + chorus) because pure-tone MP3 frames are dominated by repetition and compress trivially under xz-9e (−45% with no help), masking packMP3's structural win.

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

#### M3b — JPEG via brunsli (shipped 2026-04-30)
- Vendored `third_party/brunsli` (Google brunsli, with brotli + highway via FetchContent). Built static via cmake/MinGW. `cbrunsli` / `dbrunsli` CLI wrapped by `system()` shell-out (same pattern as `zstd`/`xz`).
- New container kind `KIND_JPEG = 2`. Manifest entry: `(u32 brn_len)(brn_bytes)`. Solid stream is not consumed.
- Detection at top level: SOI marker `FF D8 FF`. Pack: `cbrunsli`, then verify by `dbrunsli` + cmp before committing. Any failure (binary missing, format unsupported, round-trip mismatch, blob ≥ original) → fall through to KIND_OPAQUE.
- Measured: −27.15% vs xz-9e on `synth.jpg` (1024×768 RGB JPEG q=85). All other bench numbers preserved exactly.
- Build dep: `make brunsli-deps`. Runtime dep: `cbrunsli`/`dbrunsli` on PATH (bench.sh auto-prepends the locally-built copies).
- Progressive JPEG variants brunsli rejects fall through automatically.

#### M3b-zip — JPEGs inside ZIP entries (shipped 2026-04-30)
- Extends the M2/M3a ZIP unwrap path. New recipe op `OP_JPEG_STORE = 0x04`: `(u8 op)(u32 raw_len)(u32 brn_len)(brn_bytes)`. Used for STORED ZIP entries (method=0) whose payload starts with the JPEG SOI marker.
- Decode side calls `dbrunsli` via temp files; encode reuses the same `try_brunsli_buf` helper as the top-level path.
- Method=8 (deflated) JPEGs are still routed through the existing redeflate/preflate path; in practice almost no ZIP tools deflate JPEGs (auto-store), so this case is rare and not worth a fourth recipe op variant for now.
- Measured on `tests/corpus/zip-with-jpeg.zip` (1 stored JPEG + 2 deflate-9 DLLs, 1,705,559 B): zxle 1,442,710 → **−15.25% vs xz-9e**. RT OK.

#### M3c-mp3 — MP3 via packMP3 (shipped 2026-05-01)
- Vendored `third_party/packmp3` (packjpg.de packMP3 v1.0g, source distribution from github.com/packjpg/packMP3). Built static via the in-tree Makefile with `RES=` to skip the i386 icons.res (mismatched arch on x86-64 MinGW). `packMP3` CLI is wrapped via `system()` shell-out (same pattern as `cbrunsli`/`dbrunsli`).
- New container kind `KIND_MP3 = 3`. Manifest entry: `(u32 pmp_len)(pmp_bytes)`. Solid stream is not consumed.
- Detection at top level: `ID3v2` tag (bytes `49 44 33`) or MPEG-1 Layer III sync (`FF` + top 3 bits set). Pack: `packMP3 -o -np input.mp3` produces sibling `input.pmp`; verify by re-running `packMP3` on the `.pmp` and `cmp`-ing against the original. Any failure (binary missing, MPEG-2/2.5 input, format issue, round-trip mismatch, blob ≥ original) → fall through to KIND_OPAQUE.
- packMP3 derives output names by extension swap (no flags for explicit output paths), so the shim renames temps between the two passes.
- Measured: **−13.05% vs xz-9e** on `synth.mp3` (synthesized stereo music @ 128 kbps; zxle 398,756 vs xz-9e 458,596). All other bench numbers preserved exactly.
- Build dep: `make packmp3-deps`. Runtime dep: `packMP3` on PATH (bench.sh auto-prepends the locally-built copy).
- Limitation: only MPEG-1 Layer III is supported (packMP3 limitation). MPEG-2 / MPEG-2.5 MP3 files fall through to KIND_OPAQUE.

#### M3c-png — PNG IDAT recompressor (shipped 2026-05-01)
- New container kind `KIND_PNG = 4`. Detection: 8-byte PNG signature.
- Pack: parse chunks, concatenate the IDAT zlib stream, inflate to filtered-pixel bytes, try zlib-L9 default-strategy redeflate (mode 0 — match original byte-for-byte → just store inflated bytes). On mismatch, strip the 2-byte zlib header + 4-byte adler trailer, run preflate over the deflate body (mode 1), verify by re-join + cmp. Either success path puts inflated bytes into the solid zstd-19 stream and a small recipe in the manifest (pre-IDAT chunks verbatim, per-IDAT chunk lengths, mode flag, [mode=1: zhdr/adler/diff], post-IDAT chunks verbatim).
- Unpack: rebuild the zlib stream (deflate-L9 in mode 0; preflate-rejoin + zhdr/adler in mode 1), split by stored per-IDAT lengths, recompute CRC32 per chunk, emit byte-identical PNG.
- Measured: **−22.06% vs xz-9e** on `test.png` (zxle 119,297 vs xz-9e 153,064). Corpus per-file ratio 0.3721 → 0.3673; solid 0.3655 → 0.3608. All other fixtures preserved.
- Limitations: only top-level PNGs (PNGs inside ZIP entries are still routed through redeflate/preflate of the entry's deflate stream, which often misses since the ZIP-deflated zlib-stream isn't the same as the inner PNG's IDAT zlib-stream). Out of scope for this milestone.

#### M3c — pending sub-milestones
**Branch:** `feat/m3-*` · **Expected:** large wins per stream.

- ~~MP3 via packMP3~~ — shipped as M3c-mp3 above.
- ~~PNG byte-exact via IDAT-zlib-L9 / preflate~~ — shipped as M3c-png above.
- ~~PE streams via ZXL~~ — see "Tried and reverted"; revisit once ZXL has a multi-stream/solid mode.
- ~~JPEGs inside ZIP entries~~ — shipped as M3b-zip above.

Each recompressor lives behind an availability check; missing recompressors fall through to opaque-zstd.

### M4 — Cross-stream content-defined ordering
**Branch:** `feat/m4-content-ordering` · **Expected:** −2 to −10% beyond M3 on multi-file inputs.

After unwrap+recompress, compute MinHash / SimHash on each stream. Cluster, then order so similar streams are adjacent before the solid pass. Solid mode + content ordering captures cross-document boilerplate (e.g., DOCX schema across multiple Office docs).

### M5 — Neural residual fallback (optional)
**Branch:** `feat/m5-neural` · **Expected:** −5 to −15% on residuals where zstd is already near optimal.

Small autoregressive byte predictor (NNCP-class). Default-off due to slowdown; user opts in with `--slow`.

---

## Tried and reverted

### PE-via-ZXL routing as an M3 sub-milestone (2026-04-30, abandoned pre-implementation)

Plan: detect PE streams (top-level files and ZIP entries after unwrap) and route the inflated bytes through the sister project ZXL instead of the solid zstd-19 stream.

**Measured per-file ZXL on the 3 corpus DLLs:**
- ntdll.dll: 990,630 (vs zstd-19 1,012,264 → −2.14%)
- kernel32.dll: 336,175 (vs 336,518 → −0.10%)
- user32.dll: 612,924 (vs 620,237 → −1.18%)
- Sum ZXL 1,939,729 vs sum zstd-19 1,969,019 → −1.49% per-file.

**Why it regresses on the headline numbers:** the headlines are solid-mode (whole corpus or whole ZIP fed to zstd-19 `--long=27`), and solid mode captures cross-DLL similarity that per-file ZXL cannot. On the M2 fixture `pe-deflate.zip`, current zxle = 1,923,274 B (3 inflated DLLs concatenated into solid). Routing them out into per-entry ZXL blobs gives 1,939,729 B — **~0.85% regression** vs M3a baseline. Same shape on `pe-deflate-l6.zip` and the 8-file mixed corpus solid (estimated ~0.6% regression).

**Structural reason:** zstd-19 long-window-27 on concatenated similar PE files is already very strong. ZXL's per-file PE modeling beats per-file zstd-19 by 1–2%, but loses to solid-zstd-19 which sees all PEs at once.

**Don't retry until:** ZXL itself gains a solid/multi-stream input mode (a change in `../Zxl`, not here), or the corpus grows past what solid mode can hold in one pass.

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
