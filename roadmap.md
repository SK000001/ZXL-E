# ZXL-E Roadmap

Forward-looking plan: what's still ahead, what we haven't done that others have, planned milestones, and the "Tried and reverted" graveyard so we don't retry failed experiments. Shipped milestones, current-state snapshots, and completed bench measurements live in [delivered.md](delivered.md).

---

## Current state (2026-07-18, Crown-A bench: real-world corpus + freearc/kanzi/xtool measured; loss list found)

M1 + M2 + M3a–j + M3k (KIND_PDF FlateDecode scanner) + M3l (PDF-in-container) + **M3m (generic opaque flate scan, 5%-coverage-gated, bucket-0 only)** + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 + M6 v2 + M6 v3 + parser fuzz harness + M7 steps 1/2/4 + **M7 step 5 (zip/tar/ar entry-loop worker pools)** + large-corpus measurement + XLSX/PPTX bench + M8a steps 1-4 (probes under `tools/m8/`) + ZXLE_VER 7 (xz-compressed manifest + per-entry crc32 + 64-bit IO + OP_ZIP_STORE + merged-manifest layout) + BCJ arch guard + --fast block scaling + pack/unpack hardening (incl. 2026-07-17 bucket rollback + tests/hostile.sh) + GitHub Actions CI (green on Linux since 2026-07-17) + 7z & precomp-cn|xz competitor bench + JPEG codec race ship end-to-end.

**As of 2026-07-15, no benched competitor produces a smaller archive than zxle on any bench fixture, including PDFs** — the adversarial bench (real PDFs, pax tars, ZIP64, encrypted PDF) found exactly one loss class (unencrypted PDFs, 3.8× to precomp|xz) and M3k flipped it (test.pdf −74% vs xz-9e, arxiv.pdf −48.2%, both now beating precomp|xz). It also *corrected* two assumed gaps: pax tars and per-entry-ZIP64 files parse today. 8-file headline improved to per-file 0.3232 / solid 0.3174. Remaining "best" gaps: unmeasured competitors (freearc, kanzi, xtool/razor-class), zpaq-class on long text (--slow only matches -m5), and the narrower coverage items below. Shipped numbers in [delivered.md](delivered.md); surviving review items marked **[2026-07-14 review]**.

Headline numbers and the milestone-by-milestone history are in [delivered.md](delivered.md). Latest reproducible bench numbers live in [README.md](README.md) "Headline numbers" table.

**2026-07-23 deep-research pass + instrument-first probe (no code shipped; plan rewritten):** four web-research rounds, one live experiment, one throwaway probe branch (`feat/preflate-rs-probe`, abandoned). Headlines: **the probe REVERSED the preflate-rs recommendation** — every measured xtool *loss* (wheel/APK/PDF) is plain zlib **level-6**, which a ~40-line stock-zlib ladder reproduces 100% with no Rust/FFI; preflate-rs is demoted to a narrow gated Go-flate experiment (see funded items 2 and 2b, and the per-stream bullet). **git packfiles are a free win class** (measured: −28.5% vs xz-9e with the existing M3m scanner, zero new code, RT OK; ladder would also close its 96 preflate diffs = 7.7 KB); razor binaries located (Ultra F-A bundle); libbsc chosen as the text-middle-tier vehicle; M8 re-scoped around LzmaEnc's pluggable match-finder interface; OpenZL (Meta, Oct 2025) identified as complement-not-rival and a bucket-0 backend candidate for structured raw payloads. Strategic program below is **funded order v4**; per-stream and coverage sections carry **[2026-07-23]** tags.

**M8 scout (2026-05-14):** five probes in `tools/m8/` measured the GPU-match-finder hypothesis end-to-end on CPU before any production GPU plumbing. GPU SA throughput is fine (libcubwt: **315 MB/s** on 100 MB silesia, **178× xz-9e** match-finder); the `zstd_compressSequences` API accepts an externally-supplied LZ77 parse and roundtrips byte-identically. But the **ratio bottleneck is the parser, not the SA build**: greedy SA parse loses to zstd-19 by +2 to +22% across input sizes; optimal-DP over the longest match per position closes that to +1.55 to +14.05%. At 1 MB on this corpus, zstd-19 already matches xz-9e (−0.2%), so the bar to clear is zstd-19's internal parser — and closing the last 1.5% requires multi-length match candidates + repcodes + accurate Huffman/FSE cost model + two-pass refinement. None are research, all are 1–2 weeks of focused work. **M8 is split into M8a (CPU optimal-parse gate) + M8b (GPU SA integration) + M8c (chunked, deferred) below.** M7 step 3 (deferred), M8b-neural (parked research, renamed M8d), and M9 (deferred) are the other unstarted items.

---

## Strategic program — path to "fastest and best" (set 2026-07-17)

"Fastest and best in the world" decomposes into three different crowns with different programs. The winnable claim is **owning the pareto frontier**: nobody smaller at any time budget, nobody faster at our size — plus a differentiator we already hold: every archive is *verified* (byte-identical RT, per-entry crc, hostile-input-proof, CI-tested). "Smallest archive that provably restores" is the headline claim; repacker-scene competitors have known reconstruction-failure classes.

**Crown A — beat every practical archiver on real-world data (closest; weeks).** Opponents left unbenched: freearc, kanzi, xtool/razor-class. razor is the real threat — its LZMA-variant + binary filters could beat the xz --x86 bucket on executables; if it does, the fix is filter work (delta for structured tables, richer branch models) or a stronger bucket-1 backend. Equally important: bench on the world's dominant archive classes we've never touched — **Python wheels, npm tarballs, Docker layer tarballs, APKs, game asset packs** — all zlib/zstd-family containers, i.e. exactly the wheelhouse.

**Crown B — own the max-ratio pareto frontier (1–3 months).** Beat zpaq at zpaq's time budget AND xz at xz's simultaneously: the text middle tier (libbsc/zpaq-m4, the 17% default↔slow gap) plus the M8c→M8b line ("xz-9e ratio at 3–10× xz speed" — see the DP-throughput risk noted in M8b/M8c below; the honest ceiling is 3–10×, not 30×).

**Crown C — absolute ratio crown (LTCB/Hutter-style; optional, decide last).** Held by cmix/nncp neural mixers; beating them = M8e (months, GPU decode, impractical by construction). Only worth it for the trophy, not the product.

**Organizing principle [2026-07-23 research]: generator inference.** Everything that wins here — redeflate, preflate, the xz/zstd ladders, brunsli/jbrd — is one move: *identify the program that produced the bytes, store its parameters (+ corrections), re-run it at decode.* The 2026 AIT challenge winners converged on the same move independently (one entry compressed a "random" file to 14 bytes by brute-forcing the PRNG seed). The roadmap question is therefore: **which byte-producers have (large deployed mass) × (small parameter space)?** Snappy (~1 encoder; Parquet default), klauspost Go-zstd (4 levels, frozen by policy; BuildKit layers), brotli (q×lgwin×mode; every WOFF2 — no public reconstructor exists, world-first candidate), LZ4 (Unity/game/DB), Go std flate (medium × versions), klauspost flate (large × version drift). The world stack is settling into three complementary layers — byte-exact *reproduction* (preflate-rs/jbrd/ladders — our home), *structure* transforms for raw structured data (Meta's OpenZL, BSD, Oct 2025), and *entropy* (xz/CM/neural) — and ZXL-E is the only architecture positioned to compose all three.

**Funded order (v4, rewritten 2026-07-23 after a 4-round deep-research pass; v3 history in git):**
1. ~~Competitor + corpus expansion bench~~ — **DONE 2026-07-18.** freearc/kanzi/xtool benched + wheels/Docker/APK/npm fixtures added. Result: kanzi and FreeArc lose everywhere (no deflate-reconstruction; FreeArc even loses to xz-9e); **xtool|xz is the only competitor that beats us anywhere** — three deflate-reproduction fixtures (PDF −0.98%, pure-py wheel −0.76%, APK −0.07%), same class as the precomp|xz hairline losses. The exact loss list is now item 2's spec. Numbers in delivered.md.
2. **Cheap zlib (level × memLevel × windowBits) ladder — MEASURED as the loss fix; preflate-rs NOT needed for the losses (probe 2026-07-23).** The instrument-first probe (throwaway branch `feat/preflate-rs-probe`, abandoned) settled this against the round-2 preflate-rs recommendation. Per-fixture stream histogram of the current redeflate/preflate/store decision, then a grid-search asking "does any stock zlib param set exact-reproduce each stream that currently pays a preflate diff?":

   | fixture | preflate streams (Σdiff B) | stock-zlib ladder | producer |
   |---|---|---|---|
   | requests.whl (**loss −0.76%**) | 12 (367 B) | **12/12 exact** | zlib **L6** |
   | newpipe.apk (**loss −0.07%**) | 16 (810 B) | **16/16 exact** | zlib **L6** |
   | test.pdf (loss class) | 4 (454 B) | **4/4 exact** | zlib |
   | git packfile (already a win) | 96 (7 721 B) | **96/96 exact** | zlib (git) |
   | arxiv.pdf (already a win) | 40 (701 B) | **0/40 — all MISS** | non-zlib (pdfTeX/GS) |
   | alpine Docker layer | — | preflate `split=0` | Go flate |

   **Every measured xtool *loss* is plain zlib level-6** (memLevel 8/9, wbits −15; a few L4/L5) — our L9-only fast path missed it and fell to a diff. A ~40-line ladder over a tiny param set (dominant winners: (6,default,8/9,−15)) reproduces **all** loss streams exactly. Zero unreproducible streams on the ZIP fixtures (S=0, X=0) → the loss is *purely* diff-cost, nothing to "rescue." Removing the diffs (wheel Σ367 B ≈ the −0.76% margin on a 50 KB archive) flips the losses. **Cost: a v8 wire bump** — OP_REDEFLATE decode re-runs fixed L9 (recipe.c), so the winning params must ride the recipe (a param-index byte over a ~16–32-entry table; +1 B/stream, trivially won back). No Rust, no FFI, no new dependency. This is the actual funded item 2 (next session's milestone; own branch, since it's a format change).
2b. **preflate-rs — demoted to a narrow, gated, unproven experiment (was item 2 in v3).** The 2026-07-23 probe removed its headline justification: it does **not** beat the cheap ladder on any measured loss (ladder already 100% there). Its only differentiated targets are (a) **non-zlib producers** — arxiv.pdf's 40 pdfTeX/Ghostscript streams (701 B) that the ladder MISSES entirely; but those are already a win vs precomp, so it's residual headroom, not a loss; and (b) **Go flate / Docker layers** — probe confirmed the current preflate `split=0` on the 3.4 MB alpine body (Go's tokens don't model), so the ladder can't help either. preflate-rs's "unknown compressor, higher corrections" mode is the *only* candidate that might crack Go-flate, and the budget is generous (inflated 7.69 MB vs deflated 3.42 MB → `xz(inflated)+corrections < 3.42 MB` has ~30%+ slack). But whether preflate-rs produces *any* bounded Go-flate correction is unknown until it's built. **Gate: build preflate-rs (Rust static lib behind the shim ABI) only to run one measurement — Go-flate corrections on the alpine layer.** If it clears the budget, integrate for the Docker/Go class (huge real-world mass); if not, shelve it. Not funded ahead of items 3–5.

3. **Text middle tier — libbsc primary.** libbsc 3.3.12 (Sep 2025; Apache-2.0; AVX512; CUDA BWT+IBWT; same author as libsais/libcubwt we already ship) raced as a bucket-0 candidate under min-pack (shell-out first, like zpaq). References: bzip3 (demoted: LGPL, 2023 CVE cluster, author's own data-loss disclaimer), kanzi high levels, bsc-m03 (highest-ratio BWT known, enwik8 20.49 MB, GPL/experimental — ratio reference only), 7z-PPMd/durilca on text. Independent evidence: bzip3-class ≈7× faster than xz with ~20% better enwik8 ratio; kanzi 8–9 pareto-beats xz on enwik8 compression.
4. **Competitor close-out.** The Ultra F-A archiver 1.04 bundle (supercompression.org, 2025-04-12, 35 MB, free) ships **razor, nanozip 0.09a, durilca 0.5, mcm 0.85** binaries — the razor retrieval path we lacked. Bench all four + one Ultra F-A row (kitchen-sink competitor; no verified-restore story). Refresh xtool 0.7.9 → 0.8.1 (Dec 2023, development since stopped). MCM dropped as a backend candidate (dormant, known x64 crashes) — bench row only.
5. **Cheap ratio stack (mostly zero/near-zero code).** (a) **jbrd third JPEG racer**: `cjxl --lossless_jpeg` transcodes JPEG→JXL bit-exactly (jbrd reconstruction data; ~20% smaller; brunsli's successor inside VarDCT) — u8 codec byte has room. (b) **git packfiles — measured 2026-07-23, zero new code:** kanzi clone pack 421,613 → zxle 301,664 (**−28.5%**; 171 zlib streams: 75 redeflate + 96 preflate; RT OK) while xz-9e ties at +0.0% — packfiles are wall-to-wall zlib and opaque to every conventional tool; add a bench fixture. (c) **BGZF/BAM**: genomics .bam = concatenated gzip members (see multi-member gzip correction below). (d) **TorrentZip** ROM sets: canonical fixed deflate params by spec — existing redeflate likely hits 100%; verify locally. (e) **Minecraft .mca**: zlib-per-chunk region files — M3m should fire as-is; verify locally.
6. **--fast v2: flzma2/fxz raced against xz -T0.** fxz emits standard .xz (decode unchanged, no wire change). flzma2's radix MF threads *without* chunking the input (vs xz -T0's block-split cost of +0.6–2.8%); its own cost is 1–5% vs BT4 at default dict, less in high-compression mode — race decides. v1.0.1 dormant (author moved to Meta zstd) but long-fuzzed; production use in modern-rzip; maintained fork in 7-Zip-zstd. Vendor-and-freeze, not live dependency.
7. Pack-time cleanup: KIND_XZ tie-predictor — **generalized [2026-07-23]:** a learned gate (cheap features: leading-window entropy, magic, size, scan stream-count) predicting "probe won't win", trained on the bench cache we already produce; false positives cost only ratio (min-pack still verifies). Plus liblzma/libzstd in-process. 1–2 sessions.
8. **OpenZL backend race (structure layer).** Meta's OpenZL (BSD, Oct 2025): SDDL/custom-parser → trainer → transform-DAG Plan; every frame embeds its graph; one universal decoder binary. SAO star catalog: 2.06× @ 340 MB/s vs xz-9's 1.64× @ 3.5 MB/s; falls back to zstd on unstructured text; **cannot reproduce existing compressed streams — complement, not rival.** Race it as a bucket-0 backend for structured raw payloads (SQLite/.db files, CSV/columnar in tars, inflated Parquet pages) under min-pack.
9. **M8 line, re-scoped** — see the [2026-07-23 note] under M8b: LzmaEnc's pluggable GetMatches/Skip match-finder interface (standard .xz out) replaces the write-an-entropy-coder plan; flzma2 source dive first; DP-throughput gate stands.
10. **Credibility track, in parallel:** format freeze (v8 + stability promise), structured fuzzing, published reproducible results on recognizable corpora. **Additions [2026-07-23]:** (a) *reproducible archives* — we're already deterministic by default; formalize bit-identical `.zxle` for identical inputs as a documented, CI-verified guarantee no scene competitor can match; (b) *--insane probe*: fx2-cmix (open source, Hutter winner) needs ≤10 GB RAM at ~40–60 h/GB single-thread → Silesia overnight; `precomp|cmix` holds the all-time Silesia #5 slot (28.26 MB) and our unwrap already beats precomp's — a verified-RT archiver in that neighborhood is a leaderboard claim nobody else can make; (c) patent note: the foundational recompression patents (Ocarina US20080050029A1 family, filed 2006–08, Dell-acquired) are expiring 2026–28; Microsoft shipping preflate-rs under Apache-2.0 signals the space is clear.
11. Crown C decision only after 1–10. **Long-horizon world-first flags:** brotli reconstructor ("prebrotli" — WOFF2/web-asset class, small param space, no public tool exists) and H.264/H.265 CABAC re-entropy (15–19% shown in literature; nobody ships it — see Video bullet).

**Explicitly off the table:** lz4-class speed at max ratio (physics of the frontier); beating cmix on text without going neural. **Long-term unclaimed territory (noted, not funded):** lossless video re-entropy (H.264/H.265 CABAC re-coding — the brunsli trick on the world's dominant byte mass); see the Video bullet under per-stream improvements.

---

## Future work (coverage gaps and unknowns)

Roughly ordered by real-world impact-to-effort ratio. The ones near the top should be funded first; the strategic program above sets the cross-section sequencing.

### What we haven't done that others have

Honest list of capabilities present in shipping codecs (zstd, xz, zpaq, 7z, freearc) but absent here. Not all of these are wins worth pursuing — listed so the gap is visible, not implicitly endorsed as a priority.

- **Multi-threaded pack** — **closed 2026-07-17 for the recompression side**: probe ladders and min-pack tiers (M7 steps 1/2), zip/tar/ar entry loops (M7 step 5). What remains serial by design is the final `xz -9e --threads=1` solid pass — determinism-preserving; `--fast` (xz -T0, scaled blocks) is the sanctioned trade at +0.6–2.8% size. Faster-at-equal-ratio needs a different final encoder (M8c track).
- **Pack-time competitive with the codec floor** — improved 2026-07-17 (M7 step 5 entry pools; every recompression loop parallel) but still above the floor. Remaining structure, measured: (1) the final `xz -9e` solid pass — the price of the ratio; `--fast` parallelizes it above ~tens of MB at +0.6–2.8%, and costs only +8 B below that (2026-07-17 sweep); (2) **wrapper-reproduce ladders** — real_coreutils_src.tar.xz packs ~28 s because each pack_xz probe re-encodes the ~40 MB body; for tie-shaped inputs (already-xz source tars) all that work buys nothing — a predictor that skips or defers the unwrap attempt for KIND_XZ inputs unlikely to win is the highest-value pack-time lever left; (3) **[2026-07-14 review]** link liblzma/libzstd instead of shelling per probe (also removes decode-side tool-version fragility). The old sub-item (b) min-pack opaque-skip already ships (95% condition in min_pack_for_tier).
- **Stable wire format** — bumped v3 → … → v7. No version-skew testing; no migration path. Other shipping codecs hold their format for years. Acceptable while unreleased; freeze before any external consumer.
- **Streaming pack/unpack** — `read_whole_file` everywhere; whole input materialized to RAM. zstd / xz are streaming. 64-bit IO fixed 2026-07-14 (was a hard 2 GiB `ftell` cap on Windows); the malloc ceiling ~3–4 GB remains.
- **Random-access / block-mode output** — solid-only. Other formats (`.7z` block mode, zstd seekable, `.zip` per-entry) allow extracting a subset without inflating the whole archive.
- **Corruption tolerance** — *detection* shipped 2026-07-14 (v7 per-entry crc32; xz/zpaq payloads were already internally checksummed; a corrupted archive now fails hard instead of silently emitting wrong bytes). *Recovery* still absent: a single corrupted byte in the solid payload loses everything after it. Per-entry framing would cost ratio; document the failure mode instead.
- **Encryption** — zstd / 7z / xz support encrypted archives. We don't.
- **Fidelity: mode/mtime** — **[2026-07-14 review]** `mode` is stored in the manifest but never applied at unpack (no chmod); mtimes aren't stored at all. Either apply/store them or drop the dead field at the next format bump.
- **Cross-stream dedup** — zstd long-window covers ~256 MiB; we inherit its window inside one bucket but don't dedup *across* the PE/non-PE bucket boundary or across recipe-stored bytes (recipe payloads outside solid).
- **Self-extracting / portable decode** — current decode requires the `zxle` binary plus xz/zstd/bzip2/preflate/brunsli/packMP3 on PATH. zstd / xz produce self-contained streams.
- ~~**Large-corpus measurement**~~ — **closed 2026-05-13.** `ZXLE_GIANT=1` (silesia × 5, 1.06 GB) measured: zxle matches tar+xz-9e at GB scale (+0.00%, ratio 0.2283), --fast scales to 9.07× at 1 GB vs 5.9× at silesia. Memory ceiling: `read_whole_file` model holds ~1 GB RSS, breaks above ~3-4 GB single-allocation on Windows. Real streaming pack/unpack remains pending (under "Validation / quality gaps" below). Details in delivered.md.
- **Fuzz beyond random byte mutation** — `tests/fuzz.sh` (shipped 2026-05-09) does ~700 random mutations and found 1 bug. AFL/libfuzzer with structured corpora would do much better; haven't done that.
- **Real-world deployment signal** — no users, no archives in production, no bug reports beyond what fuzz surfaced. Other codecs have years of millions-of-archives feedback.
- **Cross-platform build polish** — Windows 11 / MinGW + Git-Bash is the dev platform; **Linux builds and benches green in CI since 2026-07-17** (needed a libpreflate portability patch: upstream hardcodes MSVC _ftelli64/_fseeki64). Mac unverified.
- **GPU / SIMD acceleration** — zstd has SIMD entropy decoders; we have none. Tracked as aspirational M8.

Most of these are not novel research problems — they're engineering items that take weeks each. The items genuinely worth doing soon are M7 (multi-threading, free ratio gain), large-corpus measurement (validates everything), and structured fuzzing (safety before any external use).

### Coverage gaps the synthetic corpus doesn't surface

- **Real-world archive classes never benched (program step 1).** Python wheels, npm package tarballs, Docker layer tarballs, APKs, game asset packs — the most economically relevant archive shapes on earth, all zlib/zstd-family containers (our wheelhouse; likely large wins). Add as fixtures with competitor rows; these also make the "best on real-world data" claim checkable by outsiders on data they recognize. *(Wheels/npm/Docker/APK closed 2026-07-18; game asset packs still open — Unity bundles below.)*

- **Generator-inference class backlog [2026-07-23 research]**, roughly by (mass × tractability):
  1. **git packfiles — measured 2026-07-23, already won, zero new code:** kanzi clone pack 421,613 → zxle 301,664 (**−28.5%**, 171 zlib streams: 75 redeflate/96 preflate, RT OK) while xz-9e *ties the original* (+0.0%) — every clone, server repo, and repo backup is wall-to-wall zlib and opaque to conventional tools. Promote to a bench fixture (larger repo pack; the 96 preflate diffs also stack with the preflate-rs lever).
  2. **Parquet / ORC page recompression** — the data-lake format; Snappy default (≈one canonical encoder → near-trivial byte-reproduction), gzip pages → preflate, zstd pages → existing ladder; thrift footer + page-index walker. No archiver touches it.
  3. **RPM** (promoted from below) — Fedora ships zstd-19 bodies (existing ladder class); header + cpio walker is small.
  4. **CAB / MSI (MSZIP only)** — per-block deflate → preflate applies; Windows-world mass. Skip LZX initially.
  5. **Unity asset bundles** — stock whole-stream LZMA or LZ4 chunks (both small param spaces; parsing prior art: UnityPack, Unity3DCompressor). The "game asset packs" fixture class.
  6. **pg_dump -Fc** — zlib-deflate per data chunk; every Postgres backup.
  7. **HDF5 / NetCDF** — gzip-filtered chunks (chunk B-tree walk); scientific-data mass.
  8. **conda packages** — `.conda` = ZIP of zst-tars; unlocked by exactly the "zst-inside-stored-ZIP" nested-dispatch axis below.
  9. **TorrentZip ROM sets** — canonical fixed deflate params *by spec*; existing redeflate likely hits 100%. Verify locally (minutes).
  10. **Minecraft region files (.mca)** — zlib per 4 KB-sector chunk; M3m scanner should fire as-is. Verify locally (minutes).
  11. **NSIS / InnoSetup installers** — zlib/bzip2/LZMA inners; repacker-scene home turf; after 1–5.
  12. **WOFF2 / base64-PEM spans** — blocked on the brotli reconstructor (per-stream section) / a base64 span transform (decode → pipeline → re-encode with line-width param; precomp models it, we don't).
  13. **SQLite / raw structured files** — *not* a walker: files are raw B-tree pages (structure-layer work, PZIP does it commercially); exhibit A for the OpenZL backend race (funded item 8).

- ~~**Real `.deb` re-fixture**~~ — **closed 2026-05-14.** `tests/corpus/real_hello.deb` (data layer `.tar.xz`, hello_2.10-3) and `tests/corpus/real_coreutils.deb` (data layer `.tar.zst`, coreutils 9.5) are wired into `tests/bench.sh` (lines 186-187). M3f-ar headline holds on both shipped layouts.
- **Multi-member gzip / bzip2** — small extension. **[2026-07-23] "Rare in practice" was wrong:** BGZF (the genomics **.bam/.bgz** container — exabyte-scale class) is *concatenated gzip members* by design, and klauspost **pgzip** Docker layers are multi-member too. The same small extension unlocks both. Promoted.
- **GNU tar widening** — **measured 2026-07-15 (adversarial bench): pax tars parse today.** `git archive` output and GNU `tar --format=pax` both unwrap (pax 'x' entries ride as ordinary payloads); zxle beat xz-9e and 7z on both test tars. What actually rejects: base-256 size fields (entries >8 GB) and sparse files — both rare. Deprioritized back down; revisit only if a real corpus hits those.
- **Nested-dispatch remaining axes** — **[2026-07-14 review]** OP_ZIP_STORE (2026-07-14) closed zip-in-tar/ar. Still missing: gz/bz2/xz/zst/tar/zip inside ZIP stored entries (release zips carrying tarballs), zip-in-zip (jar-in-war), and non-tar format dispatch inside gz/bz2/xz/zst bodies. The clean end state is one recursive `dispatch_stream()` shared by all walkers instead of per-kind copy-paste.
- **ZIP variants** — ZIP64 (large archives), encrypted (legacy ZipCrypto + AES), DEFLATE64, BZIP2-in-ZIP, LZMA-in-ZIP, prefix bytes (self-extractors). Fall through to KIND_OPAQUE; common in real Windows-world ZIPs. **Measured 2026-07-15:** python `force_zip64` per-entry files parse fine today (LFH zip64 extras don't trip zip_parse); the rejecting path is the zip64 EOCD (>65,535 entries / >4 GB archives), untested against a real artifact. Corpus-scan before building.
- **Raw-deflate scan inside opaque streams** — **PDF closed 2026-07-15 (M3k KIND_PDF):** zlib-header scan + redeflate/preflate verify + embedded-JPEG race; test.pdf −74% / arxiv.pdf −48.2% vs xz-9e, both beating precomp|xz. **(b) PDFs inside containers closed 2026-07-17 (M3l);** pdf-in.tar −24.3% / zip-with-pdf.zip −34.0% vs xz-9e. **(a) all-KIND_OPAQUE scan closed 2026-07-17 (M3m):** `pack_flate_scan` runs on bucket-0 opaque files with a 5% coverage gate (probe: 130–930 MB/s scan, zero false hits on DLL/text/media corpora); flate-blob.bin −40.45% vs xz-9e. Remaining: (c) encrypted PDFs are ciphertext and stay opaque — physics limit, not a gap; (d) PDFs/flate inside *deflated* ZIP entries aren't dispatched (inflated bytes go to solid via REDEFLATE/PREFLATE; nesting a recipe under those ops is the recursive `dispatch_stream()` end-state above); (e) bucket-1 (x86 PE/ELF) files skip the scan — unwrapping a PE installer's embedded flate payload needs STRUCT bytes to carry bucket routing, a format change; revisit if a real corpus shows PE-with-big-flate-payload mass (NSIS installers use their own framing, so likely rare).
- **APK / IPA / JAR signed-archive specifics** — verify the deflate roundtrip survives JAR signing's specific deflate output. Likely fine via preflate, but unmeasured.
- ~~**OOXML beyond DOCX**~~ — **closed 2026-05-14.** `sample.xlsx` (117,953 B) → −19.35% vs xz-9e; `sample.pptx` (223,470 B) → −22.68% vs xz-9e. Same M2 ZIP unwrap + M3a preflate path as DOCX; no per-format code. Fixtures in `tests/make_fixtures.sh`, bench lines in `tests/bench.sh`.
- **`.7z`** — LZMA2 + filters + delta + BCJ. Reproducibility of LZMA2 across versions is iffy. High effort, low certainty. Defer until everything else is exhausted.
- **`.cab`** — Microsoft cabinet (MSZIP / LZX / Quantum). Niche but real on Windows-world artifacts (MSI installers).
- **`.rpm`** — RPM v3/v4 header + cpio + zstd/xz body. Worth a sub-milestone if the real-world bench shows it matters.
- **`.iso` / `.udf`** — generally KIND_OPAQUE-able (the inner files are what matter, but we'd need an iso walker). Defer.

### Per-stream improvements (when container coverage diminishing returns)

- **zlib (level × memLevel × windowBits) ladder — the Crown-A loss fix, MEASURED 2026-07-23.** **[2026-07-18, resolved by probe 2026-07-23]** The only class where any competitor beats us: PDF −0.98%, pure-py wheel −0.76%, APK −0.07% vs xtool|xz. Root cause confirmed by probe: non-L9 deflate streams fall past our `raw_deflate_l9` fast path to a preflate diff; **the producers are plain zlib at level 6** (Python zipfile, Android, git), not zlib-ng/libdeflate/Go. A stock-zlib grid-search exact-reproduced **12/12 wheel, 16/16 APK, 4/4 test.pdf, 96/96 git-packfile** preflate streams (0 misses); winners cluster hard at (level 6, default strategy, memLevel 8/9, wbits −15). So the fix is a small in-house ladder tried before the preflate fallback, taking the first exact match. **Cost: a v8 wire bump** — OP_REDEFLATE decode re-runs *fixed* L9 (recipe.c), so the winning params ride the recipe as a param-index byte (~16–32-entry table; +1 B/stream, dwarfed by the 3–514 B diffs it removes). Applies everywhere deflate is reproduced: zip.c, png.c, gz.c, pdf.c, tar/ar STORE paths. **What the ladder does NOT cover (→ [microsoft/preflate-rs](https://github.com/microsoft/preflate-rs), demoted funded item 2b):** non-zlib producers — arxiv.pdf's 40 pdfTeX/GS streams were 0/40 ladder-MISS (already a win, so residual only) — and Go flate (alpine layer preflate `split=0`; only preflate-rs's unknown-compressor mode might crack it, unproven until built). preflate-rs is no longer the loss fix; the ladder is.
- **Go-deflate reproducer (new class: Docker layers, Go binary releases).** **[2026-07-18, re-scoped 2026-07-23]** preflate (2018 C++) cannot split Go `compress/flate` output at all (probe: split FAILED on the whole 3.4 MB alpine layer), so Docker layers and Go-built release tarballs stay KIND_OPAQUE and tie xz-9e / lose to 7z. **Harder than assumed:** there are *two* Go encoder families in the wild — std `compress/flate` (levels 1–3 own fast encoder, 4–9 own lazy matcher; docker-save-era layers) and klauspost/compress flate (levels 1–6 multiple fastEnc variants, 7–9 lazy, opportunistic Huffman table reuse, *changes between releases*; BuildKit and most modern Go tooling) — plus klauspost pgzip (independent gzip members per block, which actually helps). A bit-exact reproducer means modeling both families across versions: substantial, ongoing burden. **Probe 2026-07-23:** current preflate returns `split=0` on the whole 3.4 MB alpine body (raw 7,688,192 / def 3,419,797) — confirmed opaque; the stock-zlib ladder (funded item 2) also cannot touch Go tokens. So this class needs *either* preflate-rs's unknown-compressor mode (funded item 2b — build it once to measure Go-flate corrections vs the ~30% budget: `xz(7.69 MB inflated)+corrections < 3.42 MB`) *or* a bespoke reproducer. **Unfunded pending item 2b's Go-flate measurement** and a layer-corpus scan of std-Go vs klauspost vs classic-zlib mass. Counter-trend: OCI v1.1 zstd layers are coming (gzip still default everywhere), and BuildKit's zstd is klauspost pure-Go with **only 4 effective levels, frozen by policy** — the future zstd-layer class is a trivial ladder row, not a research project.
- ~~**JPEG: packJPG vs brunsli**~~ — **closed 2026-07-15.** JPEG codec race shipped: blobs carry a u8 codec byte, pack tries both and keeps the smaller. synth.jpg 116,612 now beats precomp+xz (116,656). lepton is dead (Dropbox deprecated it) — no probe needed.
- **JPEG third racer: JPEG XL / jbrd.** **[2026-07-23]** `cjxl --lossless_jpeg` transcodes JPEG→JXL **bit-exactly** (jbrd = JPEG bitstream reconstruction data; `djxl` restores the original file byte-for-byte), ~20% smaller and typically a few % better than brunsli — jbrd is brunsli's formal successor inside JXL's VarDCT. The u8 codec byte has room: codec 2 = jxl, raced like the others, verified round-trip at pack. Cheap (shell-out to cjxl/djxl, same pattern as brunsli), pure ratio, on a class already benched.
- **Brotli reconstructor ("prebrotli") — world-first candidate.** **[2026-07-23]** No public tool reconstructs brotli streams byte-exactly (confirmed by search — no preflate-equivalent exists). Brotli's parameter surface is small (quality 0–11 × lgwin × mode{generic,text,font}) and its single dominant implementation is far more version-stable than the zlib ecosystem. Mass: **every WOFF2 font on earth** (brotli is mandatory in the W3C spec — fonts hide inside APKs/wheels/app bundles we already unwrap), `.br` web assets, brotli HTTP archives. Build = param ladder + correction model, i.e. the pattern preflate-rs proves. After funded item 2 ships that pattern.
- ~~**JAR gap vs precomp-cn|xz**~~ — **closed 2026-07-15.** Root cause was layout (two xz container overheads vs precomp's one); merged-manifest mode + small-input layout race shipped. sample.jar 2,804 vs precomp+xz 3,012.
- **Text / source code** — currently routed to solid xz-9e (default) / zpaq-m5 (--slow). **[2026-07-14 review]** the default↔slow ratio gap on silesia is 17% (0.2269 vs 0.1891) with nothing in between; a middle tier (libbsc BWT-class, or zpaq -m4) could capture much of it at ~10× less pack time than -m5. Also worth one bench: with --slow, bucket 1 still goes to xz+BCJ — zpaq -m5 has an internal E8/E9 model and may beat it on PE bytes. **[2026-07-23]** Vehicle decided: **libbsc** (3.3.12 Sep 2025, Apache-2.0, AVX512 + CUDA BWT/IBWT — 881 MB/s enwik10 GPU decode — same author as the libsais/libcubwt we already ship). bzip3 demoted to bench-reference (LGPL, 2023 CVE cluster, upstream data-loss disclaimer); bsc-m03 = ratio ceiling reference (best BWT known, enwik8 20.49 MB, GPL/experimental); also bench 7z-PPMd and durilca (binary in the Ultra F-A bundle) on the text bucket. Silesia anchors for the tier: xz-9e ≈48.4 MB, zpaq-m5 39.1 MB, precomp|cmix 28.26 MB (all-time #5), paq8px 27.83 MB (ceiling).
- **Solid window at scale** — **[2026-07-14 review]** the M4 "parked" rationale cites zstd's 128 MiB long-window, but the final step is now xz-9e = 64 MiB dict; at Silesia/GIANT scale cross-file matches past 64 MiB are dropped. Options: probe `--lzma2=preset=9e,dict=192MiB` on the ≥64 MiB path (decode RAM = dict), or an rzip-style long-range dedup pre-pass on bucket 0.
- **PE streams** — see "Tried and reverted" PE-via-ZXL. Revisit only if ZXL gains a solid/multi-stream input mode.
- **JPEG XL (`.jxl`)** — emerging format; brunsli is the JPEG legacy path, jxl is its forward path. No urgency until real-world artifacts contain `.jxl` payloads.
- **Modern audio (Opus, FLAC)** — packMP3 only handles MPEG-1 Layer III. FLAC has lossless reconstruction tooling; Opus does not.
- **Video** — out of scope for the current architecture, but noted as the long-term unclaimed territory (2026-07-17): H.264/H.265 dominate the world's stored bytes and nobody ships lossless re-entropy at scale. CABAC re-coding (the brunsli trick applied to video) is a research project with double-digit upside on the dominant data type; revisit only after the strategic program's funded order. **[2026-07-23]** Literature confirms the headroom: 15–19% bit savings from redesigned entropy coding on H.264 residuals (Image Communication 2010; Springer 2011); a 2026 preprint (NAE-VC) explores neural CABAC replacement. Still nobody shipping — the world-first flag stands.

### Validation / quality gaps

- **Competitor benchmark — precomp, zpaq, 7z, precomp|xz, kanzi, xtool, FreeArc closed; razor retrieval path found.** **[2026-07-18]** `tests/bench.sh` runs precomp v0.4.7, zpaq v7.15 `-m5`, 7-Zip `-mx=9 -ms=on`, `precomp -cn | xz -9e`, **kanzi-cpp 2.5.3 `-l 9`, xtool 0.7.9 `precomp -mzlib | xz -9e`, and FreeArc 0.67 `-mx`** (last three Windows-only, `make kanzi-deps`/`xtool-deps`). Verdict: kanzi and FreeArc lose to zxle on every container/real-world fixture (no deflate-reconstruction; FreeArc even loses to xz-9e). **xtool|xz is the only competitor that beats us anywhere** — PDF −0.98%, pure-py wheel −0.76%, APK −0.07%, all in the zlib-param-reproduction class (preflate-rs above is the counter). **[2026-07-23] razor unblocked:** Martelock's razor ships as a bundled binary inside the **Ultra F-A archiver 1.04** package (supercompression.org, 2025-04-12, 35 MB, free — also carries nanozip 0.09a, durilca 0.5, mcm 0.85); bench all four plus one Ultra F-A row itself (kitchen-sink per-extension tool selection, no verified-restore story) next competitor session. Expect razor's loss shape = xtool's (deflate-reproduction class). Also: **xtool 0.8.1 exists** (Patreon, Dec 2023; development since stopped) — refresh from 0.7.9. Long text still loses to zpaq-class (silesia baseline; text middle tier is the counter).
- ~~**No size-scaling data.**~~ — **closed 2026-05-13.** Measured at 51 MB / 211 MB / 1.06 GB via `ZXLE_SILESIA=1` and `ZXLE_GIANT=1`. No solid-mode cliff at 1 GB; zxle matches tar+xz-9e byte-for-byte on opaque routing. Memory bottleneck is `read_whole_file` at ~3-4 GB on 64-bit Windows. See delivered.md "Large-corpus measurement".
- **No memory numbers reported anywhere.** Pack/unpack wall time now ships in `tests/bench.sh` (per-file `pk_ms`/`un_ms` columns plus a `perf:` line per container case, via bash 5 `EPOCHREALTIME`). Peak RSS still pending — needs a platform-specific wrapper (`/usr/bin/time -v` on Linux; PowerShell `Get-Process` or `wmic` on MSYS2 — neither uniform). First wall-time signals surfaced (2026-05-07): M3h-zsttar level-3 ladder pack ~19.9 s on a 1.4 MB input — the 7-entry `(level, --long)` probe ladder is the dominant pack-time hot spot; M3e-targz / M3f-ar at ~7 s on similar-size inputs.
- **Fuzz coverage is shallow.** `tests/fuzz.sh` (shipped 2026-05-09) does ~50 random mutations × 7 kinds; found 1 bug (`raw_inflate_dyn` truncated-stream hang). A structured AFL/libFuzzer pass on each `pack_*` with format-aware corpora would surface deeper issues.
- **Corruption: detection shipped, recovery absent.** v7 (2026-07-14) added per-entry crc32 verified at unpack — corruption or decode-side tool-version drift now fails hard instead of silently emitting wrong bytes. Recovery still absent (solid stream: one bad byte loses everything after it); per-entry framing would cost ratio, so document rather than build. (The 2026-07-17 hostile-input finding — mid-walk pack_* failures orphaning solid-bucket bytes — is fixed: bucket rollback in all three walkers, guarded by `tests/hostile.sh`.)
- **Decode-side tool-version coupling.** unpack_xz/zst/bz2 re-encode with PATH tools; a version whose output differs from pack-time breaks extraction (now detected by crc, not prevented). **[2026-07-14 review]** record tool version strings in the manifest and warn on mismatch; long-term fix is vendoring liblzma/libzstd statically (same work as the spawn-cost item above).
- **CI green since 2026-07-17.** `.github/workflows/ci.yml`: ubuntu libpreflate+zxle build, self-contained fixtures, `tests/hostile.sh`, bench gated on no rt=FAIL. First run caught a real Linux bug (libpreflate MSVC-only _ftelli64/_fseeki64 — patched in preflate-deps); second run green end-to-end. Mac still unverified; no unit tests beyond bench/fuzz/hostile scripts.
- **No published, third-party-runnable results (program item 10).** All numbers live in this repo's docs, produced on one machine. The "best in the world" claim needs witnesses: a published results table on recognizable corpora (wheels/Docker/APK from the corpus-expansion item) with the reproducible harness (bench.sh + CI already enforce the discipline). Pairs with the format-freeze item above — a claim on an unstable format doesn't stick either. **[2026-07-23 additions]** (a) **Reproducible archives**: pack is already deterministic by default (`--threads=1` everywhere); formalize *bit-identical output for identical inputs* as a documented, CI-verified guarantee — no MT scene tool can match it, and it makes archives diffable like reproducible builds. (b) **--insane leaderboard probe**: fx2-cmix (open source, Hutter winner, ≤10 GB RAM, ~40–60 h/GB single-thread → Silesia overnight) as a bucket-0 codec_id; `precomp|cmix` holds all-time Silesia #5 at 28.26 MB and our unwrap beats precomp's — a *verified-RT* archiver in that neighborhood is a claim no PAQ-family entry has ever made. (c) Patent note, examined once: the foundational recompression patents (Ocarina US20080050029A1 family, filed 2006–08, Dell) expire 2026–28; Microsoft's public Apache-2.0 preflate-rs signals the space is clear.
- **No streaming pack/unpack.** Whole-file `read_whole_file` everywhere (64-bit since 2026-07-14; malloc ceiling ~3–4 GB remains). Fine for ≤1 GB; falls over above. Defer until a real workload demands it.
- **Manifest format unstable.** ZXLE_VER bumped 2 → … → 6 in three days (M5/M6 era) → 7 (2026-07-14: compressed manifest + crc32 + u64 csize + OP_ZIP_STORE). No version-skew testing; no migration path. Cross-version interop will only matter if external tools consume `.zxle`; the current source-of-truth doc is the `kinds.h` header comment.
- **Manifest STRUCT bytes: variant (b) mostly superseded.** **[2026-07-14 review, updated 2026-07-15]** The merged-manifest layout (manifest at the head of bucket 0's xz stream, small-input layout race) captures variant (b)'s main win — structural bytes and content share one stream and one container overhead. What remains of (b) is *placement*: STRUCT bytes sit as one block at the stream head rather than adjacent to their related content, and the concatenated-tars degenerate shape still routes a huge manifest through the stream. Only revisit if a fixture shows placement mattering.

### Architecture-level open questions

- **M4 cross-stream ordering** — parked. Revisit only if the corpus exceeds the long-window or we move to a non-solid block format.
- **M5 neural residual fallback** — pending. Probably only worth shipping after the container-coverage and per-stream story is broadly solid; otherwise the slowdown (NNCP-class) buys little above what's already achievable.
- **Block / streaming output format** — current container is solid-only. A block-mode variant would enable random access and corruption tolerance at a (small) ratio cost. No demand yet.

---

## Architecture

Five-stage pipeline as actually shipped (the original M4 content-defined ordering and M5 neural-residual fallback are parked; M5 was repurposed for the `--slow` zpaq-m5 final step).

1. **Recursive container unwrap** — peel ZIP / tar / ar / .deb / gzip / bzip2 / zstd / xz down to raw streams plus a byte-identical-rebuild recipe (M2 / M3a–j).
2. **Per-stream format-aware recompression** — DEFLATE → preflate (or zlib-L9 redeflate fast path), JPEG → brunsli, PNG IDAT → preflate over inflated pixels, MP3 → packMP3.
3. **Per-OP content-aware bucket routing** (M6 v3) — each recipe op carries a u8 bucket; PE/ELF bytes route to the BCJ sub-stream, everything else to the main bucket. Mixed-content containers split per-entry.
4. **Cross-stream solid mode** — main bucket finalized with `xz -9e` (default) or `zpaq -m5` (`--slow`); BCJ bucket always finalized with `xz -9e --x86`.
5. **min-pack fallthrough** — every pack runs both the unwrap path and an all-opaque path; the smaller wins. With `--slow` on small inputs, also runs the default-tier candidate and keeps the smaller, so `--slow` is strict pareto over default.

---

## Roadmap (unstarted milestones)

Shipped milestones live in [delivered.md](delivered.md).

### M7 — CPU parallelism / multi-threading (partially shipped)

**Branch:** `feat/m7-mt` · **Expected:** 2–8× pack-time speedup on multi-core machines (typical: 4–8 cores) with **zero ratio change** by default and an optional `--fast` flag that trades determinism for additional speed.

**Motivation:** today's pack pipeline is largely serial. The previously-shipped speedups (parallel silesia baselines, skip force_opaque, sha256 cache) were all bench-side or redundancy-side. The pack itself still runs probe ladders sequentially, runs min-pack tiers sequentially, and shells out to single-threaded codecs.

**Plan:**

1. ~~**Parallelize probe ladders in `pack_xz` / `pack_zst`**~~ — **shipped 2026-05-13 (M7 step 1).** All `(level, --extreme)` / `(level, --long)` candidates run concurrently via `try_run_parallel` (pthreads + `system()`); lowest-ladder-index byte-identical match wins. Measured: real_coreutils.deb 15.4 s → 6.4 s (−58%), real_coreutils_src.tar.xz 53.2 s → 33.1 s (−38%). Details in delivered.md.
2. ~~**Parallelize the per-fixture min-pack tier** (slow vs default) when both are needed.~~ — **shipped 2026-05-13 (M7 step 2).** When `--slow` runs the cross-codec tier (total < 1 MB), both tiers now run on their own pthread and the smaller blob wins. Measured: DOCX 13.5 s → 9.4 s (−31%), JAR 293 → 206 ms (−30%), JPEG 204 → 122 ms (−40%), MP3 856 → 423 ms (−51%). Details in delivered.md.
3. **Parallelize the unwrap+force_opaque pair** when both are needed (silesia.mozilla shape) — **deferred.** The existing min-pack opaque-pass skip condition fires on most default-bench fixtures (unwrap is a clean win), so speculatively running opaque in parallel would add work to most fixtures and regress wall time. Would need an upfront heuristic that predicts when both passes will actually run. Not pursued; revisit when there's a corpus where opaque-pass-runs is the common case.
4. ~~**`--fast` flag**~~ — **shipped 2026-05-13 (M7 step 4).** Both final-step xz invocations switch from `--threads=1` to `--threads=0` with an explicit block size when `--fast` is set. Manifest unchanged (`xz -d` handles multi-block streams). Measured on silesia mozilla (51 MB): 23.4 s → 4.0 s (−83%, 5.9×) at +2.77% size. **Re-tuned 2026-07-14:** block size scales with input (`bucket/ncpus`, clamp [8, 64] MiB) instead of fixed 8 MiB; size cost at 1 GB drops +3.24% → +0.58% (5.4× speedup). Details in delivered.md.
5. ~~**Worker pool over per-entry recompression loops**~~ — **shipped 2026-07-17 (M7 step 5, zip + tar + ar).** All three walkers run entry payloads on a pthread pool into per-entry fragments, spliced in order — output byte-identical. zip: pe-deflate-l6 pack −36%, pptx −38%. tar/ar: gz-in.tar −13%, mixed.deb −12%, mixed.tar −9%. libpreflate concurrency-hammered clean first. **Every per-entry recompression loop is now parallel; the remaining pack-time floor is the final xz -9e over the solid payload (--fast trades it) and the wrapper-reproduce ladders (bounded by their slowest probe).**
6. **Multi-threaded sniffer** (M6 prerequisite — already shipped) — content-type sniffing can run per-entry in parallel during the unwrap walker.

**Risks:** subprocess fan-out on Windows is heavier than POSIX `fork`; need to use `posix_spawn` consistently. Determinism gate (`--fast` flag) needs careful manifest-flag plumbing (similar to `--slow`).

### M8 — GPU match-finder (scout 2026-05-14; M8a partial pass 2026-05-15; reframed around entropy-backend swap)

**Branch history:** `feat/m8-gpu-matchfind` (5 commits, FF-merged to master 2026-05-14). All scout probes live under `tools/m8/` on master.

**Original spec:** xz-9e ratio at 10–30× faster pack via GPU SA + CPU entropy coder, output byte-identical to standard xz/zstd. Plan was a 2-week sprint.

**What the scout actually measured:** the scout work answered three questions that the original spec assumed-OK without measuring. Two passed; the third failed and reframes the milestone.

| question | answer | evidence |
|---|---|---|
| 1. Can GPU build a suffix array fast enough? | **Yes — 315 MB/s on RTX 3060 Laptop, 178× xz-9e match-finder.** Gating concern (memory-bandwidth bound) doesn't materialize. | `tools/m8/sa_probe.cu` via libcubwt v1.6.3 on 100 MB silesia mix. VRAM ~2.0 GiB (20.5× input). Ceiling ~300 MB per pass on 6 GiB VRAM. |
| 2. Can zstd consume an externally-supplied LZ77 parse? | **Yes — `ZSTD_compressSequences` accepts whole-input sequence arrays and roundtrips byte-identically through standard decoders.** | `tools/m8/seq_producer_probe.c` (block-level API), `tools/m8/compress_seq_smoke.c` (whole-input API). |
| 3. Does a globally-aware SA-based parse beat zstd-19? | **No. Greedy is +2 to +22% worse; optimal-DP over the longest match per position is +1.55 to +14.05% worse.** At 1 MB on this corpus, zstd-19 already matches xz-9e (−0.2%) so the bar to clear is zstd-19, not xz-9e separately. | `tools/m8/cpu_lz_probe.c` (greedy), `tools/m8/cpu_lz_optimal.c` (optimal-DP). Same silesia mix; measured at 102 400 / 130 000 / 200 000 / 1 000 000 byte prefixes. |

**Structural finding:** the ratio bottleneck is the parser, not the SA build. zstd-19's internal optimal parser already extracts most of the available LZ structure on this corpus. Closing the last ~1.5% gap requires implementing:

- multi-length match candidates per position (we only emit the longest);
- repcode tracking (cheaper-to-encode repeated offsets);
- accurate Huffman/FSE cost model (we approximate literals at 8 bits; real text Huffman is 4–6);
- two-pass cost-model refinement (first parse estimates entropy, second parse uses empirical cost).

None are research. All are documented in zstd-19's source. Together they're 1–2 weeks of focused work. **GPU porting before that work happens only buys SA-build speed on output that is already larger than what plain `zstd -19` produces.**

#### M8a — CPU optimal-parse competitive with zstd-19 (ratio gate)

**Branch history:** `feat/m8a-optimal-parse` (step 1, FF-merged 2026-05-14), `feat/m8a-step2-repcodes` (step 2, FF-merged 2026-05-14), `feat/m8a-step3-twopass` (step 3, FF-merged 2026-05-14), `feat/m8a-step4-gate` (step 4, FF-merged 2026-05-15). **Status: PARTIAL PASS.** SA-based optimal parser meets the +1% gate at 100 KB and 1 MB on the silesia mix; misses at 10 MB (+1.56%) and 30 MB (+2.28%, borderline-triggers the +2% abandon clause). Structural follow-up is **swapping the entropy backend** (see new M8c below), not further M8a iteration -- 9 cost-model / candidate-set variations (v5-v13) characterized the wall.

**Plan:**
1. ~~Multi-length candidates per position~~ — **shipped 2026-05-14 (M8a step 1).** `tools/m8/cpu_lz_optimal_v2.c` walks SA in both directions (depth-bounded to 32 ranks per side) collecting up to 8 distinct (length, smallest-offset) tiers. DP picks freely among (literal) and all candidates. Measured at 1 MB on silesia mix: **+1.25% vs zstd-19** (was +1.55% with longest-match-only). 5 MB: +1.96%. 102 KB: +8.67%. 10 MB hits a multi-block validator bug (deferred — step 4's two-pass rewrite supersedes the matcher anyway). Bonus: end-to-end CPU pipeline runs at ~7 MB/s on 5 MB, already 4× xz-9e's 1.8 MB/s pack speed before any GPU work. Gate at 1 MB missed by 0.25 pp; steps 2/3/4 below are the path to close it.
2. ~~Repcode tracking~~ — **shipped 2026-05-14 (M8a step 2).** `tools/m8/cpu_lz_optimal_v3.c` switches from backward DP to **forward DP carrying state = (cost, rep[3])** per position. At each position the DP tries: literal, every multi-length candidate, and a live byte-by-byte scan of T at each of the 3 current rep offsets (the candidate set keeps smallest-offset per length tier and would miss rep-matching offsets otherwise). Cost model: rep matches priced at `(3 + log2(len)) * 8` (1/8-bit units) vs `(4 + log2(len) + log2(off)) * 8` for fresh offsets, reflecting zstd's FSE offset-code prices. Output sequences carry raw offsets with `rep=0`; `ZSTD_compressSequences` at level ≥10 re-detects repcodes internally (per zstd.h: "Repcodes are, as of now, always re-calculated within this function, ZSTD_Sequence.rep is effectively unused"), so a parse that picks more rep-reusable matches encodes smaller. Measured on silesia mix: **102 KB +1.82%** (was +8.67% in v2, −6.85pp), **1 MB +0.82%** (was +1.25%, **passes the +1% gate**), **5 MB +1.77%** (was +1.96%, −0.19pp). Forward-DP wall time at 1 MB ~290 ms (SA 39 + cand 72 + DP 176); at 5 MB ~1.2 s.
3. ~~Two-pass cost model~~ — **shipped 2026-05-14 (M8a step 3, partial).** `tools/m8/cpu_lz_optimal_v4.c` runs pass 1 with static 8-bit literal cost, builds `byte_cost[256]` from the pass-1 literal frequency distribution (`-log2(p) * 8` in 1/8-bit units, clamped to [1, 12] bits), then re-runs forward DP with per-position cost `byte_cost[T[i]]`. **Side fix:** added `ZSTD_c_windowLog=27` on the encode CCtx — without this, `ZSTD_compressSequences` rejects inputs >~9 MB with "External sequences are not valid" (block-split sequences referencing offsets past a prior block boundary). This unblocks measurement at 10 MB and above. Measured on silesia mix: **102 KB +1.41%** (was v3 +1.82%, −0.41pp), **1 MB +0.80%** (was +0.82%), **5 MB +1.72%** (was +1.77%), **10 MB +2.86%** (first measurement; v2's multi-block validator bug retired). On this corpus the byte distribution is nearly uniform (1 MB lit_cost range 7.88–8.13 bits → quantizes to ~8 bits everywhere), so the two-pass refinement has small headroom; gains would be larger on pure-text inputs. Step 4 (re-measure at 100 MB) and structural follow-ups remain.
4. ~~Re-measure ratio + iterate on cost model / candidate set~~ — **shipped 2026-05-15 (M8a step 4, partial pass).** `tools/m8/m8a_gate.sh` runs M8a + zstd-19 + xz-9e on silesia mozilla+webster+nci at 100 KB / 1 MB / 10 MB / 30 MB. (100 MB was the original gate point but the parser binary OOMs above ~30 MB on 8 GB RAM; trend at measured sizes carries the decision.) Nine variation probes (`tools/m8/cpu_lz_optimal_v5-v13.c`) explored:

   - **v5:** match overhead 4→6 bits + walk_limit 32→128. 30 MB: +3.75% → +2.66%.
   - **v6:** per-offset-class FSE cost learned from pass 1. 30 MB: +2.74%. (1 MB best at +0.30%.)
   - **v7:** + per-ml-class FSE cost. Within noise (mixed corpus has smooth ml distribution).
   - **v8:** per-128KB-block (byte, of, ml) FSE tables. Per-block smoothing introduced noise; regressed v6/v7 slightly.
   - **v9:** max_cands 8→16, walk_limit 128→512. Biggest single jump at scale. 30 MB: +2.32%.
   - **v10:** v9 + v8 per-block tables combined. Per-block noise undid v9 wide-candidate gains.
   - **v11:** per-candidate length truncation in DP (try every L' in [MINMATCH, Lmax] at each candidate offset). 100 KB +0.77% (best), 1 MB +0.20% (best), 10 MB +1.58% (best), 30 MB +2.36% (slight regression vs v9).
   - **v12:** hash-chain match-finder augmentation (16-bit hash, K=8 chain) for short medium-offset matches. 30 MB: +2.28% (best).
   - **v13:** deeper hash chain (18-bit hash, K=32). Output identical to v12 at 10 MB → candidate breadth is not the bottleneck past v12.

   **Diagnostic (`tools/m8/diff_parse_30mb.c`):** at 30 MB compared v12's parse to zstd-19's via `ZSTD_generateSequences`. Findings: v12 uses 50.5% repcodes vs zstd-19's 43.3% (we beat it on repcode usage); offset/ml histograms match zstd's distribution within ~10K matches per class after v12; total match count differs by only ~50K. **The remaining ~2.3% gap is in encoded cost-per-match, not parse-selection quality.** That cost-per-match gap is what zstd-19's per-block FSE retuning + entropy-aware optimal-parse loop deliver -- our offline cost tables (even per-block, v8/v10) can't replicate that because zstd retunes FSE *during* the parse with candidates evaluated against the *current* FSE state. Closing it inside zstd's entropy stage means reimplementing zstd-19's optimal parser, which is the gate-spec abandonment line.

   **Best across all probes:**

   | size | best result | iteration | +1% gate | +2% abandon |
   |---|---:|---|---|---|
   | 100 KB | **+0.77%** | v11 | ✓ PASS | n/a |
   | 1 MB | **+0.20%** | v12 | ✓ PASS | n/a |
   | 10 MB | **+1.56%** | v12/v13 | ✗ miss | ✓ clear |
   | 30 MB | **+2.28%** | v12 | ✗ miss | ✗ trigger (borderline) |

   At 1 MB v12 ties xz-9e (638 378 vs 638 104) -- so M8a is competitive against the universal target at small/medium scale. At 30 MB xz-9e wins by 9% over both M8a and zstd-19, because LZMA's range coder is structurally smaller than zstd's FSE+Huffman on binary data. **The path to "structurally smaller at 30 MB" is swapping entropy backends, not improving the LZ parse -- see new M8c below.**

**Gate verdict:** partial pass. M8a's parser is shipped and characterized; M8b (GPU SA integration) remains gated on a clean +1% pass at all sizes (10 MB and 30 MB still miss). M8a is NOT abandoned -- it's structurally correct LZ77 work that beats zstd-19 at small scale -- but it cannot clear 30 MB without a fundamentally different entropy stage. Reframe captured as new M8c below.

**Multi-week.** Step 4 itself spanned 14 iterations (v4→v13) over 2026-05-14/15, ~6 hours of focused work + extensive measurement.

#### M8b — GPU SA integration into the M8a pipeline (after M8a passes the gate)

**Branch:** `feat/m8b-gpu-sa` · **Expected:** swap libsais (CPU SA, 31 MB/s) for libcubwt (GPU SA, 315 MB/s) inside the M8a pipeline. End-to-end pack wall time gain: roughly the SA-build fraction of M8a's CPU time (probably 30–50%), assuming the optimal-parse step doesn't dominate.

**[2026-07-17 risk note]** The optimal-parse step *will* dominate: the forward DP runs ~1–10 MB/s on CPU (v3 measured ~290 ms at 1 MB). Once the SA is GPU-fast and the entropy stage is swapped (M8c), the DP is the throughput cap. GPU-parallelizing a cost-model DP is wavefront-style work and unpriced. Honest end-to-end landing zone for the M8 line: **xz-9e ratio at 3–10× xz speed**, not 30×. Gate M8b on a DP-throughput measurement the way M8a was gated on ratio.

**[2026-07-23 re-scope — the cheap path]** Three findings collapse M8b/M8c's cost. (1) **LzmaEnc's match finder is pluggable**: the 7-Zip/LZMA-SDK encoder consumes matches only through the `GetMatches`/`Skip` match-finder interface (BT4/HC4 are just two implementations; Inno Setup exposes the choice as a user setting). Implementing that interface over a GPU-built SA keeps the battle-tested optimal parse + range coder and emits **standard LZMA2/.xz** — no bespoke entropy coder (the old M8c plan) needed. Output ≠ byte-identical to xz -9e (different match lists) but we control both sides via codec_id; valid-.xz suffices. BT4 is the dominant cost of xz-class encode → realistic 2–3× at equal-class ratio. (2) **flzma2** (radix MF + LZMA2, v1.0.1 long-fuzzed, production use in modern-rzip, maintained fork in 7-Zip-zstd) already does "swap the match finder" internally — source dive its RMF→encoder boundary before writing anything. (3) zstd's external **sequence-producer API** (`ZSTD_registerSequenceProducer`, official-experimental since 1.5.4) is the sanctioned version of what M8a hacked via `compressSequences`, if the zstd side is ever revisited. Order: flzma2/fxz race as `--fast` v2 (funded item 6) → flzma2 source dive → GetMatches-over-GPU-SA prototype, still gated on DP/throughput measurement.

**Plan:**
1. Patch libcubwt to expose its internal SA (currently only BWT). Either add `int64_t libcubwt_sa(storage, T, SA, n)` or accept GPU→CPU copy of SA after the existing internal sort and skip the BWT permutation.
2. Wire SA buffer into M8a's pipeline; libsais call becomes optional fallback for non-CUDA builds.
3. Build a CUDA PLCP+LCP kernel (libcubwt doesn't ship one). The Kasai algorithm linearizes on CPU but a GPU version exists per Deo/Keely 2013 ("Parallel suffix array and least common prefix for the GPU").
4. Measure end-to-end pack speed vs M8a CPU and vs `zstd -19` and `xz -9e`. Decode unchanged.

#### M8c — Swap entropy backend zstd → LZMA-class range coder (new, 2026-05-15 reframe)

**Motivation:** M8a step 4 (above) demonstrated empirically that our SA-based optimal parser produces a parse essentially equivalent in quality to zstd-19's at scale (matched offset/ml/rep distributions, 50.5% rep usage vs 43.3%), yet our `ZSTD_compressSequences` output stays ~2.3% larger than zstd-19's own output at 30 MB on the silesia mix. The gap is in cost-per-match within zstd's FSE+Huffman entropy stage -- and the *same input* through xz-9e is **9% smaller than zstd-19's own best** (8.74 MB vs 9.64 MB at 30 MB). LZMA's range coder is structurally tighter than zstd FSE on binary data.

**Direction:** keep M8a's GPU-friendly SA+LZ pipeline, but route the parse to an LZMA-format range coder instead of `ZSTD_compressSequences`. Decoder uses standard `xz -d`. Output is byte-identical to a standard `.xz` stream.

**Plan:**
1. Study LZMA stream format (xz-utils `liblzma`, or 7-zip reference). The sequence-to-stream conversion is the new work; everything before (SA, candidate enumeration, optimal parse) is already on master.
2. Build a `ZSTD_compressSequences`-shaped API around `liblzma` -- accepts an externally-supplied LZ77 parse, produces an `.xz`-formatted stream. May require liblzma patches if the public API doesn't accept external sequences (zstd's API was the only known one that did).
3. Re-run the M8a step 4 gate measurement targeting xz-9e instead of zstd-19. Gate: M8a/LZMA within +1% of xz-9e at 100 KB / 1 MB / 10 MB / 30 MB.
4. If gate passes, M8c ships. M8b GPU SA integration would then be evaluated on the M8a+LZMA pipeline (still gated on M8a's part of that pipeline meeting the gate).

**Risk:** liblzma may not expose a "supply-your-own-parse" API in the way zstd does (zstd's API itself is documented as debug-only). The work to write one ourselves is meaningful but bounded -- LZMA range coding is well-documented.

**Multi-week.** Realistic 2-4 weeks.

#### M8d — Chunked output / inputs > VRAM (was M8c, deferred)

Unchanged from original spec. Adds `--gpu-chunked` mode for inputs above the SA ~300 MB VRAM ceiling, partitions match-stream into N chunks with a cross-chunk bridge side-stream. Only ship when there's a named workload that demands it (today: silesia 211 MB and GIANT 1 GB both fit in 6 GiB VRAM at the per-pass ceiling, decode is already 35× faster than pack so parallel-decode is the wrong target). Renumbered to make room for M8c entropy-swap.

#### M8e — Neural / pretrained-model backend (parked; research; was M8d)

Originally bundled into M8, split, renumbered. Renamed M8e here after M8c was repurposed to entropy-backend swap.

**Direction:** bundle a small distilled byte-level model (10–50 MB), use it as the per-byte predictor in a GPU arithmetic coder. Could land at cmix-class ratio (~0.13 on text) at zpaq-class throughput.

**Why parked:**
- Multi-month effort: model selection, distillation/training, fixed-point quantization for cross-hardware determinism, onnxruntime/libtorch integration, model bundle/versioning.
- Ratio win is workload-dependent: cmix beats xz on text by ~30%, ties or loses on binary / already-compressed payloads. Without a target deployment, it's a generic-purpose model bet.
- Compression community has been chasing this for ~15 years; nothing off-the-shelf delivers cmix-class ratio at production throughput.

**Reconsider when:** M8 (match-finder) has shipped and there's a concrete workload where +30% ratio on text-heavy data is worth a model bundle + GPU dep + multi-month build.

### M9 — Specialized model training (research; deferred)

If the project ever has a clear use case (e.g., "compress GitHub source-tree archives" or "compress backup tarballs of OS packages"), a small model trained on that distribution can outperform generic zpaq-m5 at lower cost than M8's universal model. Not worth pursuing without a concrete deployment target.

---

## Tried and reverted

### M4 cross-stream content-defined ordering (2026-05-01, abandoned pre-implementation)

Plan: after unwrap+recompress, MinHash/SimHash each stream, cluster, reorder so similar streams sit adjacent in the solid input.

**Pre-implementation measurement on the 6 MB sub-corpus (3 PE DLLs + 3 text files):**
- DLLs grouped (best case):                 2,233,127
- DLLs interleaved with text (mid case):    2,233,561  (+0.02%)
- Reversed: text first, DLLs last (worst):  2,236,592  (+0.16%)

**Why it doesn't move:** zstd-19 `--long=27` runs with a 128 MiB match window. The entire corpus fits inside a single window, so cross-stream matches already happen regardless of stream order. Worst-case adversarial reordering costs less than 0.2% — there is no headroom for content-defined ordering to capture.

**Don't retry until:** (a) the practical corpus size exceeds the long-window (>128 MiB on the same `--long=27` setting; or use a smaller window deliberately), or (b) we ship a non-solid / per-block payload format where each block's ordering input matters in isolation. Neither applies in the current architecture.

**[2026-07-14 note]** Condition (a)'s threshold is stale: the final step moved from zstd `--long=27` (128 MiB) to xz-9e (64 MiB dict), so Silesia (211 MB) and GIANT (1 GB) already exceed the window. Ordering is still second-order to the bigger dict / long-range-dedup options — see "Solid window at scale" under Per-stream improvements.

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
- **Source layout:** modular under `src/`. Driver `src/zxle.c` (main / do_pack / do_unpack / pack_run / manifest IO); shared infra `src/{util,kinds,deflate,recipe,preflate_shim}.{h,c}`; one module per top-level KIND (`src/{zip,png,gz,bz2,zst,xz,tar,ar,jpeg,mp3}.{h,c}`). The local-only `graph.md` (gitignored) is the navigation index — read that instead of grepping zxle.c.
- **Backend deps (M1):** system `zstd`. M2+ adds `python` for ZIP, then format-specific tools.
- **File extension:** `.zxle`.
- **Magic bytes:** `Z X L E` (4 bytes), version byte, flags byte.
- **Test corpus:** mirrors ZXL's `tests/` plus a few container-format samples added for M2.
