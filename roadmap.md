# ZXL-E Roadmap

Forward-looking plan: what's still ahead, what we haven't done that others have, planned milestones, and the "Tried and reverted" graveyard so we don't retry failed experiments. Shipped milestones, current-state snapshots, and completed bench measurements live in [delivered.md](delivered.md).

---

## Current state (2026-05-13, M7 step 4 shipped)

M1 + M2 + M3a–j + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 + M6 v2 + M6 v3 + parser fuzz harness + M7 step 1 + M7 step 2 + **M7 step 4 (--fast flag for parallel final-step xz encode)** ship end-to-end.

Headline numbers and the milestone-by-milestone history are in [delivered.md](delivered.md). Latest reproducible bench numbers live in [README.md](README.md) "Headline numbers" table.

The honest gap inventory ("what others have, we don't") is the next section. M7 step 3 (unwrap+force_opaque parallelism — deferred), M8 (GPU/ML), and M9 (specialized model training) are the remaining unstarted items in the roadmap.

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
- **Large-corpus measurement** — bench corpus tops out at Silesia (211 MB). Behavior at GB scale (memory, time, ratio) unmeasured.
- **Fuzz beyond random byte mutation** — `tests/fuzz.sh` (shipped 2026-05-09) does ~700 random mutations and found 1 bug. AFL/libfuzzer with structured corpora would do much better; haven't done that.
- **Real-world deployment signal** — no users, no archives in production, no bug reports beyond what fuzz surfaced. Other codecs have years of millions-of-archives feedback.
- **Cross-platform build polish** — built and tested on Windows 11 / MinGW + Git-Bash. Linux/Mac builds are likely fine but unverified.
- **GPU / SIMD acceleration** — zstd has SIMD entropy decoders; we have none. Tracked as aspirational M8.

Most of these are not novel research problems — they're engineering items that take weeks each. The items genuinely worth doing soon are M7 (multi-threading, free ratio gain), large-corpus measurement (validates everything), and structured fuzzing (safety before any external use).

### Coverage gaps the synthetic corpus doesn't surface

- **Real `.deb` re-fixture** — replace `mixed.deb` (synthetic, uses inner `.tar.gz`) with a real Debian package whose data layer is `.tar.xz` or `.tar.zst`. Confirms M3f-ar headline holds on the actually-shipped layout.
- **Multi-member gzip / bzip2** — small extension; rare in practice; add only if the real-world bench shows a hit.
- **GNU tar widening** — base-256 size encoding, pax extended headers, sparse files, long-name records. Currently rejected → KIND_OPAQUE on real GNU-formatted tars. Real-world coverage, no headline.
- **ZIP variants** — ZIP64 (large archives), encrypted (legacy ZipCrypto + AES), DEFLATE64, BZIP2-in-ZIP, LZMA-in-ZIP, prefix bytes (self-extractors). All currently fall through to KIND_OPAQUE; common in real Windows-world ZIPs.
- **APK / IPA / JAR signed-archive specifics** — verify the deflate roundtrip survives JAR signing's specific deflate output. Likely fine via preflate, but unmeasured.
- **OOXML beyond DOCX** — XLSX / PPTX should already work via the M2 ZIP path (unmeasured).
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

- **No competitor benchmark.** We compare against xz-9e (the universal target). For honest positioning we should also bench against precomp, freearc, zpaq, kanzi on the same fixtures. Likely outcome: zxle wins on container-heavy artifacts, loses on long-text where zpaq is king.
- **No size-scaling data.** Corpus is ~7 MB; real archives can be GB. Does solid mode still help past the 128 MiB long-window? When does memory or time become the bottleneck? Bench on a larger constructed corpus (e.g., 200 MB+) to find the cliff.
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

### M8 — GPU / ML backend (research; aspirational)

**Branch:** `feat/m8-gpu` · **Expected:** if it works, push past the zpaq-m5 efficient frontier on Silesia (target ratio 0.16–0.17, vs current --slow 0.1891) at GPU-class speed (10–100× faster than CPU paq8/cmix). Big "if" — this is research, not engineering.

**Two viable directions:**

1. **nvCOMP fast-path** — NVIDIA's GPU-accelerated zstd/deflate. Same ratio class as CPU zstd (~0.25 on Silesia, i.e., looser than our default xz). Useful as a `--gpu-fast` flag for users who want very-fast pack at moderate ratio. Adds CUDA dep. Concrete, ~1 week of integration if a CUDA dev box is available.
2. **Pretrained byte-level transformer / RNN backend** — bundle a small distilled model (10–50 MB), do GPU-accelerated arithmetic coding with it as the per-byte predictor. Could land in cmix territory at zpaq-class speed *on GPU*. CPU-only would still be slow. Adds onnxruntime / libtorch dep. Multi-week of work plus model selection / training.

**Why this is properly research-grade:**
- Both paths require a GPU dependency; without one, no win.
- Determinism: GPU floating-point isn't bit-exact across hardware. Arithmetic coding requires the predictor's output to be deterministic across encode/decode pairs. Workable (use fixed-point or quantized models) but adds complexity.
- The compression community has been chasing this for ~15 years; nothing off-the-shelf currently delivers cmix-class ratio at zpaq-class speed.

**Decision gate:** don't start until M7 (parallelism) is landed. M7's payoff is known; M8's payoff is uncertain.

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
