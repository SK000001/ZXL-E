# ZXL-E Roadmap

Forward-looking plan: what's still ahead, what we haven't done that others have, planned milestones, and the "Tried and reverted" graveyard so we don't retry failed experiments. Shipped milestones, current-state snapshots, and completed bench measurements live in [delivered.md](delivered.md).

---

## Current state (2026-07-17c, M7 step 5 complete + bench cache + --fast sweep)

M1 + M2 + M3a–j + M3k (KIND_PDF FlateDecode scanner) + M3l (PDF-in-container) + **M3m (generic opaque flate scan, 5%-coverage-gated, bucket-0 only)** + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 + M6 v2 + M6 v3 + parser fuzz harness + M7 steps 1/2/4 + **M7 step 5 (zip entry-loop worker pool; tar/ar pending)** + large-corpus measurement + XLSX/PPTX bench + M8a steps 1-4 (probes under `tools/m8/`) + ZXLE_VER 7 (xz-compressed manifest + per-entry crc32 + 64-bit IO + OP_ZIP_STORE + merged-manifest layout) + BCJ arch guard + --fast block scaling + pack/unpack hardening (incl. 2026-07-17 bucket rollback + tests/hostile.sh) + GitHub Actions CI (unverified until push) + 7z & precomp-cn|xz competitor bench + JPEG codec race ship end-to-end.

**As of 2026-07-15, no benched competitor produces a smaller archive than zxle on any bench fixture, including PDFs** — the adversarial bench (real PDFs, pax tars, ZIP64, encrypted PDF) found exactly one loss class (unencrypted PDFs, 3.8× to precomp|xz) and M3k flipped it (test.pdf −74% vs xz-9e, arxiv.pdf −48.2%, both now beating precomp|xz). It also *corrected* two assumed gaps: pax tars and per-entry-ZIP64 files parse today. 8-file headline improved to per-file 0.3232 / solid 0.3174. Remaining "best" gaps: unmeasured competitors (freearc, kanzi, xtool/razor-class), zpaq-class on long text (--slow only matches -m5), and the narrower coverage items below. Shipped numbers in [delivered.md](delivered.md); surviving review items marked **[2026-07-14 review]**.

Headline numbers and the milestone-by-milestone history are in [delivered.md](delivered.md). Latest reproducible bench numbers live in [README.md](README.md) "Headline numbers" table.

**M8 scout (2026-05-14):** five probes in `tools/m8/` measured the GPU-match-finder hypothesis end-to-end on CPU before any production GPU plumbing. GPU SA throughput is fine (libcubwt: **315 MB/s** on 100 MB silesia, **178× xz-9e** match-finder); the `zstd_compressSequences` API accepts an externally-supplied LZ77 parse and roundtrips byte-identically. But the **ratio bottleneck is the parser, not the SA build**: greedy SA parse loses to zstd-19 by +2 to +22% across input sizes; optimal-DP over the longest match per position closes that to +1.55 to +14.05%. At 1 MB on this corpus, zstd-19 already matches xz-9e (−0.2%), so the bar to clear is zstd-19's internal parser — and closing the last 1.5% requires multi-length match candidates + repcodes + accurate Huffman/FSE cost model + two-pass refinement. None are research, all are 1–2 weeks of focused work. **M8 is split into M8a (CPU optimal-parse gate) + M8b (GPU SA integration) + M8c (chunked, deferred) below.** M7 step 3 (deferred), M8b-neural (parked research, renamed M8d), and M9 (deferred) are the other unstarted items.

---

## Future work (coverage gaps and unknowns)

Roughly ordered by real-world impact-to-effort ratio. The ones near the top should be funded first.

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
- **Cross-platform build polish** — built and tested on Windows 11 / MinGW + Git-Bash. Linux/Mac builds are likely fine but unverified.
- **GPU / SIMD acceleration** — zstd has SIMD entropy decoders; we have none. Tracked as aspirational M8.

Most of these are not novel research problems — they're engineering items that take weeks each. The items genuinely worth doing soon are M7 (multi-threading, free ratio gain), large-corpus measurement (validates everything), and structured fuzzing (safety before any external use).

### Coverage gaps the synthetic corpus doesn't surface

- ~~**Real `.deb` re-fixture**~~ — **closed 2026-05-14.** `tests/corpus/real_hello.deb` (data layer `.tar.xz`, hello_2.10-3) and `tests/corpus/real_coreutils.deb` (data layer `.tar.zst`, coreutils 9.5) are wired into `tests/bench.sh` (lines 186-187). M3f-ar headline holds on both shipped layouts.
- **Multi-member gzip / bzip2** — small extension; rare in practice; add only if the real-world bench shows a hit.
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

- ~~**JPEG: packJPG vs brunsli**~~ — **closed 2026-07-15.** JPEG codec race shipped: blobs carry a u8 codec byte, pack tries both and keeps the smaller. synth.jpg 116,612 now beats precomp+xz (116,656). lepton remains untried — only worth a probe if a real-JPEG corpus shows packJPG/brunsli both losing to it.
- ~~**JAR gap vs precomp-cn|xz**~~ — **closed 2026-07-15.** Root cause was layout (two xz container overheads vs precomp's one); merged-manifest mode + small-input layout race shipped. sample.jar 2,804 vs precomp+xz 3,012.
- **Text / source code** — currently routed to solid xz-9e (default) / zpaq-m5 (--slow). **[2026-07-14 review]** the default↔slow ratio gap on silesia is 17% (0.2269 vs 0.1891) with nothing in between; a middle tier (libbsc BWT-class, or zpaq -m4) could capture much of it at ~10× less pack time than -m5. Also worth one bench: with --slow, bucket 1 still goes to xz+BCJ — zpaq -m5 has an internal E8/E9 model and may beat it on PE bytes.
- **Solid window at scale** — **[2026-07-14 review]** the M4 "parked" rationale cites zstd's 128 MiB long-window, but the final step is now xz-9e = 64 MiB dict; at Silesia/GIANT scale cross-file matches past 64 MiB are dropped. Options: probe `--lzma2=preset=9e,dict=192MiB` on the ≥64 MiB path (decode RAM = dict), or an rzip-style long-range dedup pre-pass on bucket 0.
- **PE streams** — see "Tried and reverted" PE-via-ZXL. Revisit only if ZXL gains a solid/multi-stream input mode.
- **JPEG XL (`.jxl`)** — emerging format; brunsli is the JPEG legacy path, jxl is its forward path. No urgency until real-world artifacts contain `.jxl` payloads.
- **Modern audio (Opus, FLAC)** — packMP3 only handles MPEG-1 Layer III. FLAC has lossless reconstruction tooling; Opus does not.
- **Video** — nontrivial. Out of scope for the current architecture.

### Validation / quality gaps

- **Competitor benchmark — precomp, zpaq, 7z, precomp|xz closed; freearc + kanzi + xtool-class pending.** `tests/bench.sh` runs precomp v0.4.7, zpaq v7.15 `-m5`, 7-Zip `-mx=9 -ms=on`, and the `precomp -cn | xz -9e` combo against zxle on the headline-positive fixtures, plus zpaq and 7z in the silesia baselines. 2026-07-15 signal after the JPEG race + merged-manifest ships: **zxle beats precomp|xz on all 10 combo fixtures** (+0.02% to +7.42%); 7z ties xz-9e on containers (zxle +15–47%). freearc, kanzi, and the repacker-scene chains (xtool/razor-class) remain unbenched — none is bundled or on Windows MinGW PATH; would need a setup ship before measurement. Likely outcome: zxle wins on container-heavy artifacts, loses to zpaq-class on long-text (already visible in the silesia zpaq baseline).
- ~~**No size-scaling data.**~~ — **closed 2026-05-13.** Measured at 51 MB / 211 MB / 1.06 GB via `ZXLE_SILESIA=1` and `ZXLE_GIANT=1`. No solid-mode cliff at 1 GB; zxle matches tar+xz-9e byte-for-byte on opaque routing. Memory bottleneck is `read_whole_file` at ~3-4 GB on 64-bit Windows. See delivered.md "Large-corpus measurement".
- **No memory numbers reported anywhere.** Pack/unpack wall time now ships in `tests/bench.sh` (per-file `pk_ms`/`un_ms` columns plus a `perf:` line per container case, via bash 5 `EPOCHREALTIME`). Peak RSS still pending — needs a platform-specific wrapper (`/usr/bin/time -v` on Linux; PowerShell `Get-Process` or `wmic` on MSYS2 — neither uniform). First wall-time signals surfaced (2026-05-07): M3h-zsttar level-3 ladder pack ~19.9 s on a 1.4 MB input — the 7-entry `(level, --long)` probe ladder is the dominant pack-time hot spot; M3e-targz / M3f-ar at ~7 s on similar-size inputs.
- **Fuzz coverage is shallow.** `tests/fuzz.sh` (shipped 2026-05-09) does ~50 random mutations × 7 kinds; found 1 bug (`raw_inflate_dyn` truncated-stream hang). A structured AFL/libFuzzer pass on each `pack_*` with format-aware corpora would surface deeper issues.
- **Corruption: detection shipped, recovery absent.** v7 (2026-07-14) added per-entry crc32 verified at unpack — corruption or decode-side tool-version drift now fails hard instead of silently emitting wrong bytes. Recovery still absent (solid stream: one bad byte loses everything after it); per-entry framing would cost ratio, so document rather than build. (The 2026-07-17 hostile-input finding — mid-walk pack_* failures orphaning solid-bucket bytes — is fixed: bucket rollback in all three walkers, guarded by `tests/hostile.sh`.)
- **Decode-side tool-version coupling.** unpack_xz/zst/bz2 re-encode with PATH tools; a version whose output differs from pack-time breaks extraction (now detected by crc, not prevented). **[2026-07-14 review]** record tool version strings in the manifest and warn on mismatch; long-term fix is vendoring liblzma/libzstd statically (same work as the spawn-cost item above).
- **CI shipped 2026-07-17; unverified until push.** `.github/workflows/ci.yml`: ubuntu libpreflate+zxle build, self-contained fixtures, `tests/hostile.sh`, bench gated on no rt=FAIL. The workflow has never executed (repo not pushed since); first push should confirm it goes green. Mac still unverified; no unit tests beyond bench/fuzz/hostile scripts.
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
