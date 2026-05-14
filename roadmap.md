# ZXL-E Roadmap

Forward-looking plan: what's still ahead, what we haven't done that others have, planned milestones, and the "Tried and reverted" graveyard so we don't retry failed experiments. Shipped milestones, current-state snapshots, and completed bench measurements live in [delivered.md](delivered.md).

---

## Current state (2026-05-14, XLSX + PPTX bench shipped; M8 scout-work measured + reframed)

M1 + M2 + M3a–j + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 + M6 v2 + M6 v3 + parser fuzz harness + M7 steps 1/2/4 + large-corpus measurement + **XLSX/PPTX bench (M2 ZIP path validated on OOXML beyond DOCX: −19.35% / −22.68% vs xz-9e)** ship end-to-end.

Headline numbers and the milestone-by-milestone history are in [delivered.md](delivered.md). Latest reproducible bench numbers live in [README.md](README.md) "Headline numbers" table.

**M8 scout (2026-05-14):** five probes in `tools/m8/` measured the GPU-match-finder hypothesis end-to-end on CPU before any production GPU plumbing. GPU SA throughput is fine (libcubwt: **315 MB/s** on 100 MB silesia, **178× xz-9e** match-finder); the `zstd_compressSequences` API accepts an externally-supplied LZ77 parse and roundtrips byte-identically. But the **ratio bottleneck is the parser, not the SA build**: greedy SA parse loses to zstd-19 by +2 to +22% across input sizes; optimal-DP over the longest match per position closes that to +1.55 to +14.05%. At 1 MB on this corpus, zstd-19 already matches xz-9e (−0.2%), so the bar to clear is zstd-19's internal parser — and closing the last 1.5% requires multi-length match candidates + repcodes + accurate Huffman/FSE cost model + two-pass refinement. None are research, all are 1–2 weeks of focused work. **M8 is split into M8a (CPU optimal-parse gate) + M8b (GPU SA integration) + M8c (chunked, deferred) below.** M7 step 3 (deferred), M8b-neural (parked research, renamed M8d), and M9 (deferred) are the other unstarted items.

---

## Future work (coverage gaps and unknowns)

Roughly ordered by real-world impact-to-effort ratio. The ones near the top should be funded first.

### What we haven't done that others have

Honest list of capabilities present in shipping codecs (zstd, xz, zpaq, 7z, freearc) but absent here. Not all of these are wins worth pursuing — listed so the gap is visible, not implicitly endorsed as a priority.

- **Multi-threaded pack** — single-threaded throughout (probe ladders, min-pack passes, final-step encode). zstd / xz / zpaq all parallelize trivially. Highest-impact engineering item; no ratio cost. Tracked as M7.
- **Pack-time competitive with the codec floor** — currently 5–10× the comparable xz/zstd run even in default mode (probe ladders + double-pack via min-pack). Real-world adoption gate.
- **Stable wire format** — bumped v3 → v4 → v5 → v6 in 3 days. No version-skew testing; no migration path. Other shipping codecs hold their format for years.
- **Streaming pack/unpack** — `read_whole_file` everywhere; whole input materialized to RAM. zstd / xz are streaming. Falls over above ~1 GB.
- **Random-access / block-mode output** — solid-only. Other formats (`.7z` block mode, zstd seekable, `.zip` per-entry) allow extracting a subset without inflating the whole archive.
- **Corruption tolerance** — single byte corrupted in solid payload loses everything. zstd content-checksum + xz block CRC give finer-grained failure boundaries.
- **Encryption** — zstd / 7z / xz support encrypted archives. We don't.
- **Cross-stream dedup** — zstd long-window covers ~256 MiB; we inherit its window inside one bucket but don't dedup *across* the PE/non-PE bucket boundary or across recipe-stored bytes (recipe payloads outside solid).
- **Self-extracting / portable decode** — current decode requires the `zxle` binary plus xz/zstd/bzip2/preflate/brunsli/packMP3 on PATH. zstd / xz produce self-contained streams.
- ~~**Large-corpus measurement**~~ — **closed 2026-05-13.** `ZXLE_GIANT=1` (silesia × 5, 1.06 GB) measured: zxle matches tar+xz-9e at GB scale (+0.00%, ratio 0.2283), --fast scales to 9.07× at 1 GB vs 5.9× at silesia. Memory ceiling: `read_whole_file` model holds ~1 GB RSS, breaks above ~3-4 GB single-allocation on Windows. Real streaming pack/unpack remains pending (under "Validation / quality gaps" below). Details in delivered.md.
- **Fuzz beyond random byte mutation** — `tests/fuzz.sh` (shipped 2026-05-09) does ~700 random mutations and found 1 bug. AFL/libfuzzer with structured corpora would do much better; haven't done that.
- **Real-world deployment signal** — no users, no archives in production, no bug reports beyond what fuzz surfaced. Other codecs have years of millions-of-archives feedback.
- **Cross-platform build polish** — built and tested on Windows 11 / MinGW + Git-Bash. Linux/Mac builds are likely fine but unverified.
- **GPU / SIMD acceleration** — zstd has SIMD entropy decoders; we have none. Tracked as aspirational M8.

Most of these are not novel research problems — they're engineering items that take weeks each. The items genuinely worth doing soon are M7 (multi-threading, free ratio gain), large-corpus measurement (validates everything), and structured fuzzing (safety before any external use).

### Coverage gaps the synthetic corpus doesn't surface

- ~~**Real `.deb` re-fixture**~~ — **closed 2026-05-14.** `tests/corpus/real_hello.deb` (data layer `.tar.xz`, hello_2.10-3) and `tests/corpus/real_coreutils.deb` (data layer `.tar.zst`, coreutils 9.5) are wired into `tests/bench.sh` (lines 186-187). M3f-ar headline holds on both shipped layouts.
- **Multi-member gzip / bzip2** — small extension; rare in practice; add only if the real-world bench shows a hit.
- **GNU tar widening** — base-256 size encoding, pax extended headers, sparse files, long-name records. Currently rejected → KIND_OPAQUE on real GNU-formatted tars. Real-world coverage, no headline.
- **ZIP variants** — ZIP64 (large archives), encrypted (legacy ZipCrypto + AES), DEFLATE64, BZIP2-in-ZIP, LZMA-in-ZIP, prefix bytes (self-extractors). All currently fall through to KIND_OPAQUE; common in real Windows-world ZIPs.
- **APK / IPA / JAR signed-archive specifics** — verify the deflate roundtrip survives JAR signing's specific deflate output. Likely fine via preflate, but unmeasured.
- ~~**OOXML beyond DOCX**~~ — **closed 2026-05-14.** `sample.xlsx` (117,953 B) → −19.35% vs xz-9e; `sample.pptx` (223,470 B) → −22.68% vs xz-9e. Same M2 ZIP unwrap + M3a preflate path as DOCX; no per-format code. Fixtures in `tests/make_fixtures.sh`, bench lines in `tests/bench.sh`.
- **`.7z`** — LZMA2 + filters + delta + BCJ. Reproducibility of LZMA2 across versions is iffy. High effort, low certainty. Defer until everything else is exhausted.
- **`.cab`** — Microsoft cabinet (MSZIP / LZX / Quantum). Niche but real on Windows-world artifacts (MSI installers).
- **`.rpm`** — RPM v3/v4 header + cpio + zstd/xz body. Worth a sub-milestone if the real-world bench shows it matters.
- **`.iso` / `.udf`** — generally KIND_OPAQUE-able (the inner files are what matter, but we'd need an iso walker). Defer.

### Per-stream improvements (when container coverage diminishing returns)

- **Text / source code** — currently routed to solid zstd-19. Real headroom likely small (zstd-19 long-window is strong on text), but worth measuring on a real source-tarball corpus before declaring done.
- **PE streams** — see "Tried and reverted" PE-via-ZXL. Revisit only if ZXL gains a solid/multi-stream input mode.
- **JPEG XL (`.jxl`)** — emerging format; brunsli is the JPEG legacy path, jxl is its forward path. No urgency until real-world artifacts contain `.jxl` payloads.
- **Modern audio (Opus, FLAC)** — packMP3 only handles MPEG-1 Layer III. FLAC has lossless reconstruction tooling; Opus does not.
- **Video** — nontrivial. Out of scope for the current architecture.

### Validation / quality gaps

- **Competitor benchmark — precomp + zpaq closed; freearc + kanzi pending.** `tests/bench.sh` runs precomp v0.4.7 and zpaq v7.15 `-m5` against zxle on the headline-positive fixtures (lines 221-329) plus zpaq -m5 in the silesia baselines (lines 480-557). freearc and kanzi remain unbenched — neither is bundled or on Windows MinGW PATH; would need a setup ship before measurement. Likely outcome on the unmeasured pair: zxle wins on container-heavy artifacts, loses to zpaq-class on long-text (already visible in the silesia zpaq baseline).
- ~~**No size-scaling data.**~~ — **closed 2026-05-13.** Measured at 51 MB / 211 MB / 1.06 GB via `ZXLE_SILESIA=1` and `ZXLE_GIANT=1`. No solid-mode cliff at 1 GB; zxle matches tar+xz-9e byte-for-byte on opaque routing. Memory bottleneck is `read_whole_file` at ~3-4 GB on 64-bit Windows. See delivered.md "Large-corpus measurement".
- **No memory numbers reported anywhere.** Pack/unpack wall time now ships in `tests/bench.sh` (per-file `pk_ms`/`un_ms` columns plus a `perf:` line per container case, via bash 5 `EPOCHREALTIME`). Peak RSS still pending — needs a platform-specific wrapper (`/usr/bin/time -v` on Linux; PowerShell `Get-Process` or `wmic` on MSYS2 — neither uniform). First wall-time signals surfaced (2026-05-07): M3h-zsttar level-3 ladder pack ~19.9 s on a 1.4 MB input — the 7-entry `(level, --long)` probe ladder is the dominant pack-time hot spot; M3e-targz / M3f-ar at ~7 s on similar-size inputs.
- **Fuzz coverage is shallow.** `tests/fuzz.sh` (shipped 2026-05-09) does ~50 random mutations × 7 kinds; found 1 bug (`raw_inflate_dyn` truncated-stream hang). A structured AFL/libFuzzer pass on each `pack_*` with format-aware corpora would surface deeper issues.
- **No corruption-tolerance story.** zstd-19 solid + format-aware recipes mean a single corrupted byte in the payload likely loses everything. Document the failure mode; consider whether per-entry framing is worth adding (probably not — it costs ratio).
- **No streaming pack/unpack.** Whole-file `read_whole_file` everywhere. Fine for ≤1 GB; falls over above. Defer until a real workload demands it.
- **Manifest format unstable.** ZXLE_VER bumped 2 → 3 (M5/codec swap) → 4 (M6 v1) → 5 (M6 v2) → 6 (M6 v3) in three days. No version-skew testing; no migration path. Cross-version interop will only matter if external tools consume `.zxle`; the current source-of-truth doc is the `kinds.h` header comment.

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
4. ~~**`--fast` flag**~~ — **shipped 2026-05-13 (M7 step 4).** Both final-step xz invocations switch from `--threads=1` to `--threads=0 --block-size=8388608` when `--fast` is set. Manifest unchanged (`xz -d` handles multi-block streams). Measured on silesia mozilla (51 MB): 23.4 s → 4.0 s (−83%, 5.9×) at +2.77% size. Details in delivered.md.
5. **Multi-threaded sniffer** (M6 prerequisite — already shipped) — content-type sniffing can run per-entry in parallel during the unwrap walker.

**Risks:** subprocess fan-out on Windows is heavier than POSIX `fork`; need to use `posix_spawn` consistently. Determinism gate (`--fast` flag) needs careful manifest-flag plumbing (similar to `--slow`).

### M8 — GPU match-finder (scout done 2026-05-14; reframed into M8a/M8b/M8c)

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

**Branch history:** `feat/m8a-optimal-parse` (step 1, FF-merged to master 2026-05-14), `feat/m8a-step2-repcodes` (step 2, FF-merged to master 2026-05-14). **Expected:** SA-based optimal parser whose `ZSTD_compressSequences` output lands within +1% of `zstd -19` on the silesia mix at 100 KB / 1 MB / 10 MB / 100 MB sizes.

**Plan:**
1. ~~Multi-length candidates per position~~ — **shipped 2026-05-14 (M8a step 1).** `tools/m8/cpu_lz_optimal_v2.c` walks SA in both directions (depth-bounded to 32 ranks per side) collecting up to 8 distinct (length, smallest-offset) tiers. DP picks freely among (literal) and all candidates. Measured at 1 MB on silesia mix: **+1.25% vs zstd-19** (was +1.55% with longest-match-only). 5 MB: +1.96%. 102 KB: +8.67%. 10 MB hits a multi-block validator bug (deferred — step 4's two-pass rewrite supersedes the matcher anyway). Bonus: end-to-end CPU pipeline runs at ~7 MB/s on 5 MB, already 4× xz-9e's 1.8 MB/s pack speed before any GPU work. Gate at 1 MB missed by 0.25 pp; steps 2/3/4 below are the path to close it.
2. ~~Repcode tracking~~ — **shipped 2026-05-14 (M8a step 2).** `tools/m8/cpu_lz_optimal_v3.c` switches from backward DP to **forward DP carrying state = (cost, rep[3])** per position. At each position the DP tries: literal, every multi-length candidate, and a live byte-by-byte scan of T at each of the 3 current rep offsets (the candidate set keeps smallest-offset per length tier and would miss rep-matching offsets otherwise). Cost model: rep matches priced at `(3 + log2(len)) * 8` (1/8-bit units) vs `(4 + log2(len) + log2(off)) * 8` for fresh offsets, reflecting zstd's FSE offset-code prices. Output sequences carry raw offsets with `rep=0`; `ZSTD_compressSequences` at level ≥10 re-detects repcodes internally (per zstd.h: "Repcodes are, as of now, always re-calculated within this function, ZSTD_Sequence.rep is effectively unused"), so a parse that picks more rep-reusable matches encodes smaller. Measured on silesia mix: **102 KB +1.82%** (was +8.67% in v2, −6.85pp), **1 MB +0.82%** (was +1.25%, **passes the +1% gate**), **5 MB +1.77%** (was +1.96%, −0.19pp). Forward-DP wall time at 1 MB ~290 ms (SA 39 + cand 72 + DP 176); at 5 MB ~1.2 s.
3. Two-pass cost model. First pass uses static 8-bit literal cost. Second pass estimates literal Huffman cost from the first parse's literal distribution.
4. Re-measure ratio at 100 KB / 1 MB / 10 MB / 100 MB silesia vs `zstd -19` and `xz -9e`.

**Ratio gate:** if M8a converges within +1% of `zstd -19` on silesia, proceed to M8b. **If after the four steps above M8a is still >+2% worse than `zstd -19`, M8 deflates into "Tried and reverted" with M8a's numbers as the structural reason** — the work to close the gap would amount to reimplementing zstd-19's optimal parser, at which point the codec lift is no longer in our court.

**Multi-week.** Realistic timeline 1–2 weeks of focused work. This must be flagged as such per workflow.

#### M8b — GPU SA integration into the M8a pipeline (after M8a passes the gate)

**Branch:** `feat/m8b-gpu-sa` · **Expected:** swap libsais (CPU SA, 31 MB/s) for libcubwt (GPU SA, 315 MB/s) inside the M8a pipeline. End-to-end pack wall time gain: roughly the SA-build fraction of M8a's CPU time (probably 30–50%), assuming the optimal-parse step doesn't dominate.

**Plan:**
1. Patch libcubwt to expose its internal SA (currently only BWT). Either add `int64_t libcubwt_sa(storage, T, SA, n)` or accept GPU→CPU copy of SA after the existing internal sort and skip the BWT permutation.
2. Wire SA buffer into M8a's pipeline; libsais call becomes optional fallback for non-CUDA builds.
3. Build a CUDA PLCP+LCP kernel (libcubwt doesn't ship one). The Kasai algorithm linearizes on CPU but a GPU version exists per Deo/Keely 2013 ("Parallel suffix array and least common prefix for the GPU").
4. Measure end-to-end pack speed vs M8a CPU and vs `zstd -19` and `xz -9e`. Decode unchanged.

#### M8c — Chunked output / inputs > VRAM (deferred)

Unchanged from original spec. Adds `--gpu-chunked` mode for inputs above the SA ~300 MB VRAM ceiling, partitions match-stream into N chunks with a cross-chunk bridge side-stream. Only ship when there's a named workload that demands it (today: silesia 211 MB and GIANT 1 GB both fit in 6 GiB VRAM at the per-pass ceiling, decode is already 35× faster than pack so parallel-decode is the wrong target).

#### M8d — Neural / pretrained-model backend (parked; research; was M8b)

Originally bundled into M8, then split into M8b. Renamed M8d here so the milestone codes align with the new ratio-gated structure.

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
