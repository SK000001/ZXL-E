# ZXL-E Delivered

Historical record of shipped milestones, completed bench measurements, current-state snapshots, and prior-session plans. Split out of `roadmap.md` so the roadmap stays focused on what's still ahead. When a milestone ships, its entry moves here.

---

## Current-state log (most recent first)

## Current state (2026-05-13, M7 step 4 shipped)

M1 + M2 + M3a–j + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 + M6 v2 + M6 v3 + parser fuzz harness + M7 step 1 + M7 step 2 + **M7 step 4 (--fast flag for parallel final-step xz encode)** ship end-to-end.

**Headline M7 step 4 result (2026-05-13):** new `--fast` pack flag switches both final-step xz invocations (`CODEC_XZ_9E`, `CODEC_XZ_9E_X86`) from `--threads=1` to `--threads=0 --block-size=8388608`. The explicit 8 MiB block size is required because preset 9e's 64 MiB LZMA2 dict drives a 192 MiB default block under `-T0` — inputs below that fit in one block and don't parallelize. Manifest layout unchanged (xz -d handles multi-block streams transparently). CODEC_ZPAQ_M5 unchanged (zpaq is single-threaded; `--fast` is a no-op when combined with `--slow`).

| Fixture | Default pack | --fast pack | size cost |
|---|---:|---:|---:|
| silesia mozilla (51 MB single file) | 23.4 s | **4.0 s (−83%, 5.9×)** | +2.77% |

Default bench unchanged (no `--fast` → byte-identical to master). 8-file per-file 0.3383, solid 0.3326. 43/43 default-bench fixtures + 9/9 ZXLE_SLOW=1 fixtures round-trip OK. M7 step 3 (unwrap+force_opaque parallelism) deferred — speculative parallelism would regress default-bench cases where opaque is currently skipped.

## Current state (2026-05-13, M7 step 2 shipped)

M1 + M2 + M3a–j + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 + M6 v2 + M6 v3 + parser fuzz harness + M7 step 1 + **M7 step 2 (parallel --slow + default cross-codec tier)** ship end-to-end.

**Headline M7 step 2 result (2026-05-13):** when `--slow` runs on a small input (total < 1 MB), `do_pack` invoked the slow tier (zpaq -m5 final) and the default tier (xz -9e final) sequentially and kept the smaller. Both tiers are independent — now each runs on its own thread (pthread + `min_pack_for_tier`) and we join + pick the winner. Pre-flight total-size summation moved out of `pack_run`'s side effects into an upfront stat loop so the gate decision happens before either thread launches.

| Fixture (--slow path) | Master | M7 step 2 | Δ |
|---|---:|---:|---:|
| DOCX (real ZIP/L6) | 13,503 ms | **9,371 ms** | **−31%** |
| JAR (real ZIP/L6) | 293 ms | **206 ms** | **−30%** |
| JPEG (brunsli) | 204 ms | **122 ms** | **−40%** |
| MP3 (packMP3) | 856 ms | **423 ms** | **−51%** |

Non-eligible (≥1 MB) --slow fixtures unchanged. All --slow output sizes byte-identical. Round-trip OK across the full default bench (43 fixtures) and the 9 ZXLE_SLOW=1 fixtures. Ratios unchanged: 8-file per-file 0.3383, solid 0.3326.

## Current state (2026-05-13, M7 step 1 shipped)

M1 + M2 + M3a–j + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 + M6 v2 + M6 v3 + parser fuzz harness + **M7 step 1 (parallel probe ladders in pack_xz / pack_zst)** ship end-to-end.

**Headline M7 step 1 result (2026-05-13):** pack_xz and pack_zst now run their probe candidates concurrently via a new `try_run_parallel` helper (pthreads + `system()`, each candidate into a unique rt file). After join, the lowest-ladder-index byte-identical match wins. Output recipes are unchanged — parallelism only affects encode wall time, not which candidate is selected.

| Case | Master | M7 step 1 | Δ |
|---|---:|---:|---:|
| real_coreutils.deb (zst path) | 15,423 ms | **6,430 ms** | **−58%** |
| real_coreutils_src.tar.xz | 53,205 ms | **33,123 ms** | **−38%** |
| mixed.tar.zst | 4,054 ms | 4,467 ms | +10% |
| mixed.tar.zst3 (level-3 ladder) | 3,275 ms | 4,402 ms | +34% |
| mixed.tar.xz | 4,022 ms | 4,431 ms | +10% |

Small-fixture cases regress 5–34% (subprocess fan-out overhead when the serial loop would have terminated at probe 1–3); absolute regression <1.2 s per fixture, dwarfed by the multi-second wins on the cases where probes don't match quickly. Ratios byte-identical: 8-file per-file 0.3383, solid 0.3326. All 23 bench fixtures + 8-file corpus + competitor sections round-trip OK. Makefile gains `-lpthread`.

## Current state (2026-05-09, M6 v3 shipped)

M1 + M2 + M3a–j + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 + M6 v2 + parser fuzz harness + **M6 v3 (per-OP bucket routing)** ship end-to-end.

**Headline M6 v3 result (2026-05-09):** dropped the v5 manifest u8 unwrap_bucket field; recipes now carry per-OP bucket bytes. OP_STORE / OP_REDEFLATE / OP_PREFLATE each gain a u8 after their u32 raw_size; KIND_PNG/GZIP/BZIP2/XZ/ZSTD recipes gain a u8 bucket field used when inner_kind==0. Each pack_<kind> calls a small bucket_for_bytes() helper on raw payload (PE/ELF magic at offset 0 → bucket 1, else bucket 0); container-level pre-sniffers (zip_is_pe_heavy, wrapped_is_pe_heavy, etc.) and the 2 KiB shell-out probe in M6 v2 are all gone. ZXLE_VER bumped to 6.

Mixed-content container fixtures gained 1.26–8.2 pp (PE bytes route to bucket 1 even when sharing a container with PNG/JPEG/text):

| Fixture | M6 v2 | M6 v3 | gain |
|---|---|---|---|
| mixed.tar (PNG + JPEG + 2 DLLs) | 1,188,859 | **1,091,252** | **-8.2%** |
| mixed.deb (ar → gz → tar → 2 DLLs + text) | 1,258,334 | **1,222,430** | -2.85% |
| mixed.tar.gz | -21.66% vs xz-9e | **-22.92%** | -1.26 pp |
| mixed.tar.bz2 | -20.51% | **-21.79%** | -1.28 pp |
| mixed.tar.zst3 | -21.44% | **-22.70%** | -1.26 pp |
| mixed.tar.zst (level 19) | -6.68% | **-8.18%** | -1.50 pp |

Pure-PE container fixtures unchanged (M6 v2 already routed everything to bucket 1). Pure-text/pure-image fixtures unchanged. 8-file corpus per-file 0.3383, solid 0.3326 — flat (per-file fixtures have no containers, only +1 byte per PNG recipe). JAR pays 31 bytes (32 entries × 1 byte OP_REDEFLATE bucket field) — still −59.33% vs xz-9e. All RT OK across 23 bench fixtures + 8-file corpus. Fuzz harness 210/210 clean.

## Current state (2026-05-09, parser fuzz harness shipped)

M1 + M2 + M3a–j + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 + M6 v2 + **`tests/fuzz.sh` (container-parser fuzz harness)** ship end-to-end.

**Headline fuzz result (2026-05-09):** new `tests/fuzz.sh` mutates per-kind seed fixtures (ZIP/TAR/AR/GZIP/BZIP2/XZ/ZSTD) with bit-flips, byte-flips, truncations, and header zeroing/randomization, then asserts `zxle pack <ANY_BYTES>` exits 0 in bounded time. First run found one 45-second hang on a truncated gzip — `raw_inflate_dyn` doubled its output buffer forever because zlib's `Z_BUF_ERROR` is overloaded ("output room exhausted" *and* "input exhausted mid-stream under Z_FINISH"). Fix: bail when `avail_in == 0` on `Z_BUF_ERROR`/`Z_OK`. Also replaced two `die()` calls in realloc-failure paths with NULL returns so the parser opaque-routes cleanly under allocation pressure. After fix: 700 mutations across two seeds (50 × 7 × 2) all clean — 0 fail / 0 crash / 0 hang. Bench unchanged: 8-file per-file 0.3383, solid 0.3326, all 43 RT OK.

## Current state (2026-05-08, M6 v2 shipped)

M1 + M2 + M3a–j + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + M6 v1 (top-level OPAQUE BCJ routing) + **M6 v2 (container-aware BCJ routing)** ship end-to-end.

**Headline M6 v2 result (2026-05-08):** sniffers detect PE-heavy ZIP/TAR/AR via filename, and PE-heavy GZIP/BZIP2/ZSTD/XZ via inflate-first-2KiB-and-scan. Pure-PE container fixtures gain +2.0–2.8 pp vs M6 v1. ZXLE_VER bumped to 5; manifest gains a u8 unwrap_bucket per non-OPAQUE recipe-bearing kind.

## Current state (2026-05-08, M6 v1 shipped)

M1 + M2 + M3a–j + ZXLE_VER 3 final-step xz-9e + M5 --slow + per-fixture min-pack tier + **M6 v1 (BCJ x86 routing)** ship end-to-end.

**Headline M6 v1 result (2026-05-08):** new top-level x86 sniffer routes PE/ELF KIND_OPAQUE entries to a dedicated sub-stream finalized with `xz -9e --x86` (BCJ filter). 8-file corpus per-file ratio drops 0.3453 → **0.3383**; solid 0.3391 → **0.3326**. Now beats xz-9e baseline by **5.6%** (was 3.8%). Per-PE-DLL gains: ntdll −3.1%, kernel32 −2.2%, user32 −2.0%. ZXLE_VER bumped to 4. Manifest layout: u8 opaque_bucket per KIND_OPAQUE entry, trailing payload is multi-bucket (u8 num_buckets, per-bucket u8 codec_id + u32 csize + bytes). Container fixtures byte-identical (M6 v2 will route their inflated PE bytes too).

## Current state (2026-05-08, M5 --slow shipped)

M1 + M2 + M3a–j + final-step xz-9e (ZXLE_VER 3) + **M5 --slow mode (zpaq -m5 final-step)** ship end-to-end.

**Headline M5 result (2026-05-08):** new `--slow` CLI flag finalizes the solid stream with `zpaq -m5` (cmix-class context mixing) instead of `xz -9e`. Wire format unchanged (ZXLE_VER stays at 3); the previously-unused flags byte at offset 5 now uses bit 0 (0x01) to indicate the codec choice, and `do_unpack` reads it to dispatch xz vs zpaq. `do_pack` parses leading flags before positional args. `min-pack` runs both unwrap and force_opaque passes as before, both finalized with whichever codec --slow selects.

The integration achieves "best of both worlds": the unwrap path inflates containers to raw bytes; zpaq's context-mixing then crushes those raw bytes much harder than xz-9e can. On container-shaped inputs we get unwrap × context-mixing simultaneously, beating both baselines.

| Fixture | xz-9e baseline | zxle default | **zxle --slow** | --slow vs xz-9e |
|---|---|---|---|---|
| pe-deflate.zip | 2,263,692 | 1,800,810 | **1,522,756** | **−32.73%** |
| pe-deflate-l6.zip | 2,274,840 | 1,802,202 | **1,524,151** | **−33.00%** |
| sample.docx | 585,696 | 473,042 | **308,002** | **−47.41%** |
| sample.jar | 15,392 | 6,222 | 6,608 | −57.07% |
| ntdll.dll.gz | 1,162,316 | 968,355 | **820,271** | **−29.43%** |
| mixed.tar.gz | 1,419,292 | 1,111,803 | **984,252** | **−30.65%** |
| mixed.deb | 1,541,516 | 1,258,327 | **1,061,138** | **−31.16%** |
| synth.jpg | 161,248 | 117,492 | 118,517 | −26.50% (format-aware floor) |
| synth.mp3 | 458,596 | 398,775 | 399,800 | −12.82% (format-aware floor) |

**Silesia 211 MB (the SOTA gap, now closed):**

| Codec | Bytes | Ratio | Pack |
|---|---|---|---|
| zxle solid (default) | 48,408,614 | 0.2284 | 204 s |
| **zxle --slow** | **40,068,635** | **0.1891** | 327 s |
| tar + xz-9e | 48,412,816 | 0.2284 | 101 s |
| tar + zstd-19 long=27 | 52,598,764 | 0.2482 | 40 s |
| zpaq -m5 (tar) | 40,070,637 | 0.1891 | 163 s |

**zxle --slow vs zpaq -m5: −0.00%** (2 KB win on 40 MB; effective tie). zxle --slow matches the SOTA general-purpose codec on Silesia and *beats it by 14–57% on every container fixture*. JAR's +6.20% vs default zxle is the only minor regression — a tiny input where zpaq's journaling-format overhead dominates the per-stream gain; still −57% vs xz-9e.

Pack-time tradeoff: 5–10× xz default; gated entirely behind `--slow`. Default behavior unchanged.

## Current state (2026-05-07, ZXLE_VER 3 final-step xz-9e)

M1 + M2 + M3a (preflate) + M3b (brunsli) + M3b-zip (JPEG-in-ZIP) + M3c-mp3 (packMP3) + M3c-png (zlib-L9 / preflate over IDAT) + M3c-png-zip (PNG-in-ZIP) + M3d-gzip (single-member gzip wrapper) + M3e-tar (ustar per-entry dispatch) + M3e-targz (gzip-wrapped tar) + M3e-tar-gz-in (gzip files inside tar) + M3f-ar (Unix archive: .a / .deb) + M3g-bz2tar (bzip2-wrapped tar) + M3h-zsttar (zstd-wrapped tar) + min-pack fallthrough + zstd frame-header probing + M3i-xztar (xz-wrapped tar) + M3j-store-ops (OP_XZ_STORE / OP_ZSTD_STORE) + final-step codec swap zstd-19 → xz-9e (ZXLE_VER 3) ship end-to-end.

**Headline final-step codec swap (ZXLE_VER 3, 2026-05-07):** the solid stream's compressor switched from `zstd -19 --long=27` to `xz -9e --threads=1`. Manifest layout unchanged; trailing payload is now LZMA2 instead of zstd. Triggered by the precomp v0.4.7 measurement, which showed precomp's LZMA2 final-step beating our zstd-19 by 4.5–5.9% on PE-binary container fixtures. Empirical post-swap impact on the existing corpus (no fixture changes, RT OK across all 23):

- **8-file corpus**: per-file ratio 0.3673 → **0.3453**, solid 0.3608 → **0.3391**. Now beats xz-9e baseline (0.3524) directly — zxle is the *strongest* per-file/solid number on the headline corpus.
- **Container fixtures**: every previously-positive-headline shape gained 4-7 percentage points vs xz-9e. M2 ZIP −15.04% → −20.45%; M3a preflate L6 −15.39% → −20.78%; M3b-zip JPEG-in-ZIP −15.25% → −19.85%; M3c-png-zip PNG-in-ZIP −15.73% → −22.07%; M3d gzip −11.45% → −16.69%; M3e-targz −16.04% → −21.66%; M3e gz-in-tar −9.37% → −14.68%; M3f-ar −13.31% → −18.37%; M3g-bz2tar −14.80% → −20.51%; M3h-zsttar (default-3) −15.79% → −21.44%.
- **Previously-floor-tied fixtures now win**: mixed.tar.xz −0.00% → **−6.67%**; xz-in.tar +1.55% → **−1.41%**; zst-in.tar +0.11% → **−5.79%**. Wins because the inflated PE binaries inside compress significantly tighter under LZMA2 than zstd-19 long=27, breaking through what was previously the codec floor.
- **vs precomp v0.4.7** (positive = zxle smaller): zxle now wins or ties on **9 of 10** competitor-bench fixtures. PE-binary containers flipped from −5% (loss) to +1% (win); test.png from −0.71% to **+14.05%** (huge); DOCX +11.94% → **+12.76%**. Only JAR (small Java classes, −51%) and JPEG (−0.7% tie) remain in precomp's column.
- **Pack/unpack time**: comparable to slightly faster in most cases. xz-9e on ~1 MB inputs is competitive with zstd -19 --long=27, which had to set up the long-window dictionary anyway. Real-world fixtures pack faster (real_coreutils.deb 18.3 s → 14.8 s; real_coreutils_src.tar.xz 65.9 s → 46.6 s). The expected major slowdown didn't materialize.

ZXLE_VER bumped 2 → 3 (manifest format unchanged; the version gate prevents v2 binaries from misreading v3 trailing payloads).

**Headline M3j-store-ops result (2026-05-05):** new recipe ops `OP_XZ_STORE = 0x08` and `OP_ZSTD_STORE = 0x09` complete the in-tar/in-ar STORE family (alongside OP_GZIP_STORE 0x06 / OP_BZ2_STORE 0x07). Both wire into `pack_tar` and `pack_ar` payload-dispatch chains and decode through `unpack_recipe` calling the existing `unpack_xz` / `unpack_zst`. Detection is the same magic-byte sniff used at top level. New fixtures `xz-in.tar` (1,792,000 B, ntdll.dll.xz + kernel32.dll) and `zst-in.tar` (1,853,440 B, ntdll.dll.zst + kernel32.dll) verify wiring. Bench: xz-in.tar zxle 1,288,657 vs xz-9e 1,269,040 → +1.55%; zst-in.tar zxle 1,329,945 vs xz-9e 1,328,532 → +0.11% (effective tie). On both, min-pack correctly picks the opaque candidate over the unwrap candidate — when the inner already-compressed entry is at its codec's floor (xz-9e on a 2.5 MB PE = 951 KB; solid zstd-19 of the inflated body ≈ 1.0 MB), unwrapping pays recipe overhead without recovering the gap. Milestone value is **correctness coverage** (op vocabulary completeness), as predicted by the M3i roadmap entry. RT OK on both new fixtures and all M1–M3i fixtures preserved byte-exactly.

**Headline M3i-xztar result (2026-05-05):** new `KIND_XZ = 10`. Detection is the 6-byte xz magic `FD 37 7A 58 5A 00`. Pack shells out to `xz -dc` to inflate, then probes a small `(level, --extreme)` ladder — `{9e, 9, 6e, 6, 3e, 3, 1, 0}` — pinning `--threads=1` for determinism. First match wins; mismatches fall through to KIND_OPAQUE. Inner-kind dispatch routes ustar tar payloads through `pack_tar` exactly like KIND_BZIP2/KIND_ZSTD. Recipe: `(u8 level, u8 flags, u32 raw_len, u32 orig_len, u8 inner_kind, [tar_recipe])` where flags bit 0x01 = `--extreme`. On `mixed.tar.xz` (xz -9e wrap of `mixed.tar`, 1,188,332 B): zxle 1,188,412 vs xz-9e 1,188,460 → **tie (−0.00%)**, as predicted — xz-9e already crushes mixed.tar to its theoretical floor, and min-pack picks the opaque candidate (1,188,412) over the inner-tar-routed candidate (1,232,231) which loses the recipe-overhead vs solid-zstd-19 trade. On a media-only variant (PNG + JPEG ustar in xz -9e, 313,780 B) the unwrap path wins **−8.59%** (zxle 286,833) via inner-tar + PNG IDAT recompression. Single-threaded encode is deterministic across `xz-utils 5.6.x`; `-T0` multi-threaded inputs are non-deterministic and fall through. All synthetic fixtures preserved byte-exactly.

**Headline zstd-probing result (2026-05-05):** `pack_zst` now parses the input's zstd frame header (RFC 8478 §3.1.1.1) to derive the io mode (file vs stdin, from FCS_flag), checksum policy (from Content_Checksum_flag), and a candidate `--long=N` value (from Window_Descriptor). The probe ladder iterates `level ∈ {19,22,20,21,18,17,9,6,3,1} × long ∈ {27, observed_window_log, none}` and pins io/check from the header rather than guessing. `which.pkg.tar.zst` (Arch makepkg, 16,366 B) now engages KIND_ZSTD — matched at `level=19 long=25 io=stdin check=on`, inner tar dispatched (3 gzip-store + 3 store entries) — instead of falling through to KIND_OPAQUE. Headline unchanged (−0.09% vs xz-9e — small file, min-pack picks opaque), but the synthetic-fixture-only constraint is gone. All synthetic and real-world fixtures preserved (mixed.tar.zst gained 1 byte for the new `flags` recipe byte). Recipe layout bumped: `(u8 level, u8 window, u8 flags, u32 raw_len, u32 orig_len, u8 inner_kind, [...])` where `flags` bits are `0x01 = use_stdin (FCS suppressed)` and `0x02 = no_check`. Single_Segment frames are handled (window_log=0 in recipe → no `--long` arg); dictionary frames still bail to KIND_OPAQUE.

**Headline min-pack result (2026-05-05):** the real-world bench (see `tests/real_world.md`) surfaced a catastrophic regression on small tightly-deflated tarballs: `leftpad.tgz` (1,679 B) packed to 6,067 B (+247.08% vs xz-9e), `is-odd.tgz` (2,774 B) to 6,952 B (+144.44%). The mechanism is structural — unwrapping a near-optimal gzip stream into 7 KB of inflated text and feeding it to solid zstd-19 cannot beat the original gzip output, but pays preflate reconstruction + recipe + solid framing overhead on top. Fix: `do_pack` now runs the existing pack twice when at least one entry is unwrapped — once with container routing engaged (status quo) and once with `force_opaque=1` (every input goes to KIND_OPAQUE) — and keeps the smaller result. After the fix: `leftpad.tgz` → 1,729 B (**−1.09%** vs xz-9e); `is-odd.tgz` → 2,823 B (**−0.74%**). `hello.deb` similarly tightened from +0.04% to −0.04%. All M1–M3h synthetic headlines preserved byte-exactly (the unwrap path wins on every fixture, so the opaque fallback is computed and discarded). All RT OK. 2× pack time on container-shaped inputs is the cost. Round-trip OK across the 8-file corpus, all ZIP fixtures, the DOCX/JAR fixtures, the standalone JPEG/MP3/PNG fixtures, the gzip fixture, the mixed.tar/tar.gz/tar.bz2/.deb fixtures.

**Headline M3h-zsttar result:** on `mixed.tar.zst3` (default zstd -3 wrap of `mixed.tar`, 1,419,046 B) `zxle` is **−15.79% vs xz-9e** (1,188,879 vs 1,411,820). Default-level `.tar.zst` is the typical real-world shape (most CLI / distro tooling uses level 3, not max), and zstd -3 leaves much more headroom than xz-9e can recover from the outside. On the high-effort variant `mixed.tar.zst` (zstd -19 --long=27, 1,253,451 B) the win is **−5.16% vs xz-9e** (1,188,878 vs 1,253,580). New container kind `KIND_ZSTD = 9`. Recipe stores `(level, long_window, raw_len, orig_len, inner_kind, [tar_recipe])`. Reproducibility verified at pack time by probing a 7-entry `(level, --long)` ladder (covering our internal `-19 --long=27` first, then default `-3`, plus -22/-19/-22/-1 variants); first cmp match wins, otherwise KIND_OPAQUE.

**Headline M3g-bz2tar result:** on `mixed.tar.bz2` (bzip2 -9 wrap of `mixed.tar`, 1,395,196 B) `zxle` is **−14.80% vs xz-9e** (1,188,877 vs 1,395,332). xz-9e cannot crack the bzip2 body so loses across the board on `.tar.bz2`; zxle inflates via system `bzip2 -dc`, recursively routes the inner tar through `pack_tar`, and PNG/JPEG payloads get format-aware treatment. New container kind `KIND_BZIP2 = 8`. Recipe stores `(block_size, raw_len, orig_len, inner_kind, [tar_recipe])`. Reproducibility verified at pack time by `bzip2 -<n>` re-encode + cmp; any mismatch falls through to KIND_OPAQUE. No preflate-equivalent for bzip2 — single mode, no fallback within the kind.

Bonus headline (same milestone): on `bz2-in.tar` (ustar tar containing one bzip2 -9 DLL + one stored DLL, 1,935,360 B) `zxle` is **−6.03% vs xz-9e** (1,327,842 vs 1,413,036). The `.bz2` entry inside the tar routes through `pack_bz2` via the new recipe op `OP_BZ2_STORE = 0x07` (mirrors OP_GZIP_STORE inside pack_tar/pack_ar). Smaller win than gz-in-tar (−9.37%) because xz-9e compresses a bz2 stream more tightly than it compresses a gz stream — but still cleanly positive.

**Headline M3f-ar result:** on a `.deb`-shape AR archive (8-byte `!<arch>\n` magic + `debian-binary` text entry + `data.tar.gz` of two corpus DLLs, 1,543,622 B) `zxle` is **−13.31% vs xz-9e** (1,336,266 vs 1,541,516). Recursive unwrap chain: `ar → gzip → tar → DLLs`, each layer routed through its own pack handler so the inflated DLL bytes land in the same solid stream as everything else. New container kind `KIND_AR = 7`; new recipe op `OP_GZIP_STORE = 0x06` carries an embedded gzip recipe inside any outer container's recipe (used by both `pack_ar` and the new gzip path inside `pack_tar`). Headers and 2-byte alignment pad bytes go in `OP_STRUCT`. BSD vs GNU long-name variants don't need to be interpreted — special `//` / `#1/N` entries route through the same payload-dispatch chain (typically falls through to OP_STORE).

Bonus headline (same op): on `gz-in.tar` (ustar tar containing one `.gz` file + one stored DLL, 2,007,040 B) `zxle` is **−9.37% vs xz-9e** (1,336,642 vs 1,474,808). The `.gz` entry inside the tar now routes through `pack_gz` instead of opaquing to solid; xz-9e can't crack the gzip-deflated portion so loses despite the stored DLL being trivially redeflatable.

**Headline M3e-targz result:** on `mixed.tar.gz` (gzip -9 wrap of `mixed.tar`, 1,419,435 B) `zxle` is **−16.04% vs xz-9e** (1,191,596 vs 1,419,292) — xz can barely touch a `.gz` file because the deflated body is already opaque to it, while zxle inflates the gzip, recursively routes the inner tar through `pack_tar`, and PNG/JPEG payloads get format-aware treatment. KIND_GZIP recipe gained a trailing `inner_kind` byte (0=plain solid, 1=tar) plus an optional nested tar recipe; mode 0/1 (l9 vs preflate) is unchanged. Single-member gzip only (same M3d limitation).

**Headline M3e-tar result:** on `mixed.tar` (3,041,280 B, ustar tar of corpus PNG + JPEG + 2 corpus DLLs) `zxle` ties `xz-9e` to within 0.04% (1,188,859 vs 1,188,340 — a 519-byte noise gap), and beats opaque zstd-19 by 5.16% (1,253,549). Top-level files with `"ustar"` at offset 257 are walked block-by-block; per-entry payloads route through `pack_png` / brunsli / OP_STORE; headers and padding go in OP_STRUCT verbatim. Recipe reuses the OP_* vocabulary from KIND_ZIP, so unpack dispatches the same `unpack_recipe` walker — no separate decoder.

The 0.04% number is what a mixed-content tar (heavy on already-compressible PE binaries) looks like. On a media-only tar (corpus PNG + synth.jpg) the same routing produced **−22.18% vs xz-9e** (244,180 vs 313,780), because xz-9e cannot crack the PNG IDAT or the JPEG. M3e's structural value is two-pronged: (a) it prevents the ~5% regression that an opaque tar would suffer against xz-9e, (b) it captures the per-entry format-aware wins for any media inside.

Limitations: ustar / GNU "ustar " magic only. GNU base-256 size encoding rejected. Plain `.tar` only — the gzip wrapper around `.tar.gz` still bypasses tar routing (M3d-gzip dumps the inflated tar bytes straight to solid). Wiring gzip → tar recursion is the natural next step (M3e-targz).

**Headline M3d-gzip result:** `zxle` is **−11.45% vs xz-9e** on `ntdll.dll.gz` (gzip default-level wrap of the corpus DLL, 1,163,931 B → 1,029,252 B). Top-level files starting with `1F 8B 08` are parsed (FEXTRA/FNAME/FCOMMENT/FHCRC optional fields walked), the deflate body inflated, CRC32 + ISIZE verified against the trailer; mode 0 attempts a zlib-L9 raw redeflate against the original body, mode 1 falls back to preflate (the typical case for GNU-gzip output, which uses a different lazy-match policy from zlib-L9). New container kind `KIND_GZIP = 5`. Single-member only — multi-member gzip falls through to KIND_OPAQUE. Inflated body bytes flow into the same solid stream as everything else.

**Headline M3c-png-zip result:** `zxle` is **−15.73% vs xz-9e** on `zip-with-png.zip` (1 stored PNG + 2 deflate-9 DLLs, 1,263,952 B → 1,060,279 B). Stored ZIP entries with the PNG signature are routed through `pack_png` exactly as a top-level KIND_PNG would be; the IDAT inflated bytes flow into the same solid stream as the surrounding ZIP entries. New recipe op `OP_PNG_STORE = 0x05` carries an embedded PNG recipe inside the outer ZIP recipe.

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
| zip-with-png.zip (1 stored PNG + 2 deflate-9 DLLs) | 1,263,952 | 1,060,279 | 1,258,232 | **−15.73%** |
| ntdll.dll.gz (gzip -default of corpus DLL) | 1,163,931 | 1,029,252 | 1,162,316 | **−11.45%** |
| mixed.tar (PNG + JPEG + 2 DLLs, ustar) | 3,041,280 | 1,188,859 | 1,188,340 | **+0.04%** (ties xz-9e; −5.16% vs zstd-19) |
| mixed.tar.gz (gzip -9 wrap of mixed.tar) | 1,419,435 | 1,191,596 | 1,419,292 | **−16.04%** |
| gz-in.tar (.gz file inside ustar tar) | 2,007,040 | 1,336,642 | 1,474,808 | **−9.37%** |
| mixed.deb (.deb-shape ar -> gzip -> tar -> DLLs) | 1,543,622 | 1,336,266 | 1,541,516 | **−13.31%** |
| mixed.tar.bz2 (bzip2 -9 wrap of mixed.tar) | 1,395,196 | 1,188,877 | 1,395,332 | **−14.80%** |
| bz2-in.tar (.bz2 file inside ustar tar) | 1,935,360 | 1,327,842 | 1,413,036 | **−6.03%** |
| mixed.tar.zst (zstd -19 --long=27 wrap of mixed.tar) | 1,253,451 | 1,188,878 | 1,253,580 | **−5.16%** |
| mixed.tar.zst3 (zstd -3 default wrap of mixed.tar) | 1,419,046 | 1,188,879 | 1,411,820 | **−15.79%** |
| mixed.tar.xz (xz -9e wrap of mixed.tar) | 1,188,332 | 1,188,412 | 1,188,460 | **−0.00%** (tie; xz already at floor) |
| xz-in.tar (.xz file inside ustar tar) | 1,792,000 | 1,288,657 | 1,269,040 | **+1.55%** (coverage; xz at floor → opaque wins) |
| zst-in.tar (.zst file inside ustar tar) | 1,853,440 | 1,329,945 | 1,328,532 | **+0.11%** (coverage; effective tie) |

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

## Next-session plans (kept for context)

## Next session — push past zpaq-m5 efficient frontier (planned 2026-05-08, partially executed through 2026-05-09)

Plateau reached on the existing-codec ladder: default mode beats xz-9e on every container fixture and ties it on flat input; `--slow` mode matches zpaq -m5 on Silesia and stacks the unwrap-path gain on top. `--slow` is now strict pareto over default (per-fixture min-pack tier shipped 2026-05-08). Three documented small-input regressions closed.

Going *beyond* zpaq -m5 on raw text/binary (the only place we don't already win) requires either heavier codecs (paq8/cmix at 100–1000× slowdown) or new architecture. The new milestones in the Roadmap section below are framed against that:

- **M6 v1** — shipped 2026-05-08. PE/ELF top-level entries route to xz+BCJ bucket. +2.0% on per-file/solid corpus, +3.1% on PE DLLs.
- **M6 v2** — shipped 2026-05-08. Container-aware bucket routing for ZIP/TAR/AR/GZIP/BZIP2/ZSTD/XZ. +2.0–2.8 pp on pure-PE container fixtures. Mixed-content fixtures unchanged (per-container choice not granular enough; M6 v3 would close).
- **M6 v3** — shipped 2026-05-09. Per-OP bucket routing inside the recipe walker. Mixed-content fixtures (mixed.tar, mixed.deb, mixed.tar.{gz,bz2,zst}) gained 1.26–8.2 pp. Pure-PE and pure-text fixtures unchanged. Replaced four container-level sniffers with one `bucket_for_bytes()` helper that runs on already-inflated bytes; net code is 124 lines shorter.
- **M7** — CPU parallelism (probe ladders + min-pack tiers in parallel; optional `--fast` for multi-threaded final codec). 2–8× pack-time speedup, zero ratio cost.
- **M8** — GPU / ML backend. Aspirational; only attempt after M6/M7 land.
- **M9** — corpus-specific trained model. Deferred until a deployment target appears.

## Prior next-session list — after final-step codec swap to xz-9e (2026-05-07)

The OP vocabulary is structurally complete, encode-time hot spots are bounded, real-world `.tar.xz` / `.tar.zst` fixtures land at the universal floor, and the v3 final-step codec swap delivered a uniform +4-7 pp headline gain — zxle now beats xz-9e on the per-file corpus and wins or ties precomp on 9/10 competitor-bench fixtures. The hypothesis "switch to LZMA2" was validated empirically (no flag, no fallback); ZXLE_VER bumped to 3.

Remaining items are **measurement / quality / wider coverage**, not new container code:

- **Multi-threaded zstd reproducibility** — confirmed 2026-05-07 on real Ubuntu coreutils.deb that the data-layer `.tar.zst` (1.4 MB) is encoded with `-T0` and falls through to OP_STORE because per-worker frame splits are non-deterministic. The 2026-05-07 multi-frame fast-fail in pack_zst now bails on those inputs in milliseconds. Headline-positive routing on multi-threaded `.tar.zst` would require a multi-frame-aware probe that recognizes worker boundaries and reproduces each frame independently. Large work, uncertain payoff — most real-world `.tar.zst .deb`s sit at the universal-codec floor anyway.
- **Widen pack_xz reproducibility for non-preset encoders** — empirically established 2026-05-06 (Debian hello, dict=8MiB, custom mf/mode/nice/depth) and 2026-05-07 (GNU coreutils-9.11.tar.xz, dict=32MiB, ditto). The 2026-05-07 dict-driven pruning + bail correctly identifies these as unreachable in 2 probes (vs 8) but the headline still ties at floor. Real wins would require either (a) widening probe space with `mf` × `mode` × `nice` × `depth` permutations (large search, slow, uncertain payoff — likely doesn't reproduce libzstd-direct or GNU-release-script outputs anyway), or (b) reading lzma2 encoder choices from the stream itself (block-level filter parameters). Defer until validation gaps below close.
- **Validation gaps** — Silesia + precomp v0.4.7 + zpaq v7.15 + M5 --slow + parser fuzz harness all now in tests/. Remaining: freearc (closer architectural peer to ZXL-E with multi-codec routing), size-scaling data past 128 MiB, peak-RSS reporting in bench.

- **Parser fuzz** — closed 2026-05-09. `tests/fuzz.sh` runs ~50 mutations × 7 kinds with a per-iteration timeout; first run uncovered the `raw_inflate_dyn` truncated-stream infinite-realloc hang (described in the current-state header above). 700 mutations clean post-fix. Future expansion: AFL/libfuzzer harness against per-parser entry points if the bash harness stops finding bugs.

- **JAR small-input regression** — closed 2026-05-08. Per-fixture min-pack tier: when --slow is set on inputs < 1 MB, also run the default-mode pack and keep the smaller. Fixes JAR (+6.20% → +0.00%), JPEG (+0.87% → +0.00%), MP3 (+0.26% → +0.00%) — all three regression shapes — at the cost of a few hundred ms extra pack on small fixtures. Big-input fixtures (>= 1 MB) unchanged. **--slow is now a strict pareto improvement over default**: never larger, often much smaller (DOCX -34.89%, pe-deflate.zip -15.44%, etc.).

- **Pack-time on --slow** — partially closed 2026-05-08. min-pack's force_opaque pass now skips when `unwrapped == n && osz < 0.95 * total` (every input unwrapped, and unwrap shrunk meaningfully). Saves 17–53% pack time across single-input container fixtures with `--slow` (pe-deflate.zip 18.5s → 12.6s, JAR 402ms → 190ms, mixed.deb 16.6s → 12.7s, etc.). Silesia is unchanged because only 1 of 12 inputs unwraps, so the safety condition fails and both passes still run — correctly catching the mozilla-XPI inflation case. All sizes byte-identical post-tuning.

---

## Done bench items (kept for context)

### zpaq v7.15 -m5 in bench — SOTA gap quantified (shipped 2026-05-08)

`third_party/zpaq/zpaq64.exe` (gitignored) auto-detected; per-fixture (9 of 10; PNG dropped due to relative-path RT-verifier quirk) and Silesia baseline. zpaq beats us decisively on Silesia, loses to us decisively on every container fixture.

| Fixture | zxle | zpaq -m5 | zpaq vs zxle |
|---|---|---|---|
| pe-deflate.zip | 1,800,810 | 2,258,341 | **+25.41%** |
| pe-deflate-l6.zip | 1,802,202 | 2,269,634 | **+25.94%** |
| sample.docx | 473,042 | 586,690 | **+24.02%** |
| sample.jar | 6,222 | 16,459 | **+164.53%** |
| ntdll.dll.gz | 968,355 | 1,161,171 | **+19.91%** |
| mixed.tar.gz | 1,111,803 | 1,416,867 | **+27.44%** |
| mixed.deb | 1,258,327 | 1,538,855 | **+22.29%** |
| synth.jpg | 117,492 | 154,399 | **+31.41%** |
| synth.mp3 | 398,775 | 454,019 | **+13.85%** |
| **Silesia (211 MB)** | 48,408,614 | 40,070,637 | **−20.81%** ← zpaq wins |

**Reading:** two cleanly opposed shapes. zpaq doesn't unwrap; it compresses opaque streams. Its context-mixing margin over xz-9e (which is ~30% on flat text) shows up *only* on raw inputs. On already-structured archives it lands within 0–7% of xz-9e — and our unwrap path then beats it by 14–164%. Conversely, on Silesia (mostly raw text/binary), context-mixing's 30% margin lands and zpaq wins by 20.81%.

**Architectural implication:** the M5 milestone (originally "neural residual fallback") should be re-scoped to **zpaq-as-backend**. Same shell-out pattern we already use for brunsli/packMP3/xz. Raw streams route through it; containers stay on the unwrap+xz-9e pipeline; min-pack picks the smaller. Closes the Silesia gap without giving up any container win and without writing a context-mixing codec from scratch. Documented as the next major milestone.

### Silesia standard corpus measurement (shipped 2026-05-07)

12-file Silesia (211,938,580 B) added to bench gated behind `ZXLE_SILESIA=1`. RT-verified solid pack vs `tar | xz -9e --threads=1` and `tar | zstd -19 --long=27` baselines:

| Codec | Bytes | Ratio | Pack time |
|---|---|---|---|
| zxle solid | 48,408,614 | **0.2284** | 215 s |
| tar + xz-9e | 48,412,816 | 0.2284 | 102 s |
| tar + zstd-19 | 52,598,764 | 0.2482 | 42 s |

**zxle vs tar+xz-9e: −0.01%** (4 KB win on 48 MB; effective tie). vs zstd-19: **−7.97%**. Pack-time 2× xz-9e from min-pack double-run.

**Reading:** on flat text/binary input (Silesia is mostly raw files), zxle lands exactly at xz-9e because the final-step codec is xz-9e and the container unwrap path doesn't trigger on raw blobs. Our headline edge comes from container unwrapping; on Silesia that edge is negligible because only `mozilla` is structurally a ZIP-like archive, and even that may lose to opaque under min-pack on a 51 MB input.

**Honest positioning:** zpaq -m5's published Silesia ratio is ~0.165 (~30% smaller than xz-9e). We are not at SOTA on flat-text and will not be without a slow context-mixing or neural-residual fallback (M5 in roadmap; deferred). The "best in the world" claim does not hold on Silesia; it holds only on container-shaped artifacts where unwrap-and-recompress beats the universal-codec floor.

### First competitor measurement — precomp v0.4.7 (shipped 2026-05-07)

`third_party/precomp/precomp.exe` (gitignored under `third_party/`) auto-detected by `tests/bench.sh`; runs on 10 headline-positive fixtures with RT verification via `precomp -r` + cmp.

| Fixture | zxle vs xz-9e | precomp vs xz-9e | precomp vs zxle |
|---|---|---|---|
| pe-deflate.zip (M2) | −15.04% | −19.37% | **−5.10%** |
| pe-deflate-l6.zip (M3a) | −15.39% | −19.77% | **−5.17%** |
| sample.docx | −18.64% | −8.93% | **+11.94%** |
| sample.jar | −57.51% | −80.20% | **−53.41%** |
| ntdll.dll.gz (M3d) | −11.45% | −15.41% | **−4.47%** |
| mixed.tar.gz (M3e-targz) | −16.04% | −21.02% | **−5.93%** |
| mixed.deb (M3f-ar) | −13.31% | −17.91% | **−5.30%** |
| synth.jpg (M3b brunsli) | −27.15% | −27.65% | **−0.68%** |
| test.png (M3c-png) | −22.06% | −22.62% | **−0.71%** |
| synth.mp3 (M3c-mp3) | −13.05% | −12.99% | **+0.07%** |

**Reading:** the +/- signs are about precomp; negative means precomp is smaller than zxle. Three buckets:
1. **PE-binary containers (ZIP/gzip/ar):** precomp consistently denser by 4.5–5.9% — final-step codec difference (LZMA2 vs zstd-19 long=27) is the explanation. zstd-19 was chosen for speed; LZMA2 is denser on already-mixed-binary streams.
2. **DOCX:** zxle wins by 11.94% — zstd-19 long=27 captures XML redundancy across entries better than LZMA2. The same `--long=27` window that's "wasted" on small fixtures pays off here.
3. **Format-aware (JPEG/PNG/MP3):** ties within ±1% — both routes use brunsli / preflate / packMP3 to convert the stream to a near-optimal blob, so the final-step codec barely moves the needle.

**JAR's outlier (53%):** small Java class files, each ~600 B. zstd-19 long=27 can't find redundancy across such small payloads when wrapped in our solid block (frame overhead dominates). LZMA2's tighter coupling at small block sizes wins decisively. This shape is structurally bad for our current architecture.

**Implication:** the speed/density tradeoff is now empirically calibrated. Decision is whether to (a) keep zstd-19 (fast, +12% on DOCX, ~tie on format-aware, −5% on PE containers, much worse on small-input archives like JAR), or (b) add an `--lzma2` flag for the final-step alternative (slower pack, closes ~5% PE gap, gives up DOCX win). No code change yet — measure path (b) before committing.

### Real GNU-release `.tar.xz` ties xz-9e + pack_xz dict-driven pruning (measured + shipped 2026-05-07)

Pulled `coreutils-9.11.tar.xz` (GNU release, 6,562,420 B; dict=32 MiB per `xz -lvv`) into `tests/corpus/real_coreutils_src.tar.xz` (gitignored). Result: `zxle=6,562,636` vs `xz-9e=6,562,816` — **−0.00%** (180 B win), round-trip OK. Same floor-tie shape as `mixed.tar.xz`, `real_hello.deb`, and `real_coreutils.deb`.

**Why the tie:** dict=32 MiB maps to xz preset 8 only; pack_xz now probes (8e, 8) — both fail because GNU's release script uses `xz` with non-preset lzma2 sub-parameters (custom `mf`/`mode`/`nice`/`depth` beyond preset 8's defaults). Bail to KIND_OPAQUE; min-pack picks opaque. Predicted shape — text-heavy source `.tar.xz` at xz-9e is already at the codec floor; per-entry tar routing would only help if the inner tar contained media, which a small source distribution doesn't.

**What shipped alongside the measurement:** pack_xz now (a) parses the LZMA2 dict-size byte from the xz block header and probes only preset levels with matching dict (8 → 2 probes for this fixture; falls back to the original 8-probe ladder if header parsing fails); (b) bails immediately on multi-stream input via second-magic scan (xz `-T0` worker output). Wall-time: 176 s → 67.7 s (−62%) on this fixture; small bonuses on `real_hello.deb` (1.5 s → 964 ms) and `real_coreutils.deb` (19.6 s → 18.3 s) because their inner `.tar.xz` members also engage the new path. Sizes preserved byte-exact across all fixtures.

**Implication:** real `.tar.xz` from established release scripts (GNU, Debian) consistently uses lzma2 sub-parameters our preset-only ladder can't reproduce. The fast-fail keeps us correct via min-pack opaque; chasing headline-positive routing on these inputs requires either custom-param probing or stream-driven param extraction, both deferred (see "Widen pack_xz" in next-session list).

### Real `.tar.zst` Debian-family `.deb` ties xz-9e (measured 2026-05-07)

Pulled `coreutils_9.5-1ubuntu1_amd64.deb` (Ubuntu 24.04, 1,465,358 B; ar → control.tar.zst + data.tar.zst) into `tests/corpus/real_coreutils.deb` (gitignored). Result: `zxle=1,465,450` vs `xz-9e=1,465,500` — **−0.00%** (50 B win, effective tie), round-trip OK. All three universal codecs (xz/zstd/zxle) within 150 B of orig — canonical "already-compressed wall" shape, same as `real_hello.deb` (.tar.xz data layer).

**Routing engagement (third-party `.tar.zst` first-time exercise):**
- `control.tar.zst` (small, single-thread) → KIND_ZSTD via frame-header probing matched (`level=19 long=27 io=file check=on`), inner ustar tar dispatched. Confirms the M3h-zsttar frame-header-probing milestone works on a real third-party stream beyond `which.pkg.tar.zst`.
- `data.tar.zst` (1.4 MB, multi-threaded `-T0`) → falls through to OP_STORE. Multi-threaded zstd splits the input across workers and emits multiple concatenated frames at non-deterministic boundaries; `pack_zst` only handles single-frame inputs and bails. **The documented `-T0` non-determinism risk from M3h-zsttar is now empirically confirmed** on real Ubuntu .debs.
- min-pack picks opaque (1,465,450 < 1,472,779 unwrap candidate, 7-KB recipe overhead unrecovered).

**Implication:** real `.tar.zst` .debs land in the same bucket as real `.tar.xz` .debs — opaque-wins ties at the codec floor, confirming both `.deb` shapes via the safe min-pack fallthrough. The KIND_ZSTD frame-header probe path is now proven on two real third-party streams (Arch `.pkg.tar.zst` and Ubuntu control layer); the multi-threaded data-layer gap is a known, bounded limitation.

### Real `.deb` ties xz-9e (measured 2026-05-06)

Pulled `hello_2.10-3_amd64.deb` (53,080 B; data layer `.tar.xz`, dict=8MiB, CRC64) into `tests/corpus/real_hello.deb` (gitignored). Result: `zxle=53133` vs `xz-9e=53148` — **−0.03%** (15 B win), round-trip OK. Same shape as M3i-xztar tie: when the input is already-compressed at high ratio, xz-9e barely compresses further and our opaque path stores+overhead → near-tie.

**Why the synthetic `mixed.deb` headline (−13.31%) does NOT generalize:** synthetic uses inner `.tar.gz` whose deflate streams reproduce via preflate, so OP_GZIP_STORE fires on the data member and per-entry routing wins. Real Debian `.deb`s use inner `.tar.xz` whose lzma2 stream uses non-preset encoder params (probed: dict=8MiB matches but `mf/mode/nice/depth` don't match any of `xz -0..9` or `-0e..9e`; closest is `mf=bt4,mode=normal,dict=8MiB` at 51012 B vs target 51020 B — 8 B off). pack_xz's preset-only ladder returns -1 → all 3 ar members fall through to OP_STORE → opaque path picked by min-pack.

**Implication:** `mixed.deb` as the only `.deb` fixture overstated real-world wins. Real `.deb` is now in the bench as `M3f-ar real .deb (hello_2.10-3)` documenting the actual shape. Headline-positive routing on real .debs requires either (a) widening pack_xz to probe `mf/mode` permutations (large search, slow), or (b) Debian Trixie's `.tar.zst` data-layer .debs where pack_zst's frame-header probing should match.

### zstd ladder cannot reproduce real `.pkg.tar.zst` (resolved by frame-header probing 2026-05-05)

The 7-entry `(level, --long)` ladder in `pack_zst` fired KIND_OPAQUE on every real Arch package because none of its probes were byte-exact. The fix shipped as M3h-zsttar frame-header probing — see that sub-milestone.

**Probing data gathered 2026-05-05 on `which.pkg.tar.zst`** (kept for historical context):

The frame header (RFC 8478 §3.1.1.1) for `which.pkg.tar.zst` is `28 b5 2f fd 04 78 ...`:
- byte 4 = `0x04` Frame_Header_Descriptor → FCS_flag=00, Single_Segment=0, Checksum=1, Dictionary_ID=00. So: **no FCS bytes, no dict ID, content checksum on**.
- byte 5 = `0x78` Window_Descriptor → exponent=15, mantissa=0 → **window_log = 25** (32 MiB window).

The reproducing command, found by manual probe: `zstd -20 --long=25 -q --no-progress < raw > out` (stdin, since no FCS in header → must encode via stdin to suppress FCS that file-mode `-o ... <input>` would write).

Three rules the empirical probing established:

1. **`--long=N` is NOT idempotent across N**, even when the encoded window_log in the header is identical. On `which.pkg.tar`, `-20` (no `--long`), `-20 --long=23`, `-20 --long=25`, `-20 --long=27` all produced 16,366 B but only `--long=25` byte-matched. Implication: we *must* try the observed `--long` value (and probably a small neighborhood), not just any value ≥ window_log.
2. **FCS presence is observable from byte 4 (FCS_flag bits)**. zstd CLI writes FCS in `-o output input` (file→file) mode but suppresses it in stdin mode. So pinning is exact: observed FCS=present → file mode; observed FCS=absent → stdin mode.
3. **Default-mode synthetic fixtures (mixed.tar.zst) reproduce with the existing file-mode probe**, so the new code must keep that path working.

**Risk acknowledged at the time:** makepkg uses multi-threaded zstd (`-T0` / `--auto-threads`) on larger packages. Multi-threaded output is non-deterministic per worker assignment and won't reproduce single-threaded. Confirmed empirically on real `coreutils.deb` data-layer (see "Real `.tar.zst` Debian-family `.deb`" above).

### KIND_XZ (shipped 2026-05-05 as M3i-xztar)

Predicted shape held: mixed-content `.tar.xz` ties xz-9e (xz already crushes mixed.tar to its floor; min-pack picks opaque); media-heavy `.tar.xz` wins ~−8.6% via inner-tar + PNG/JPEG-aware routing. See `M3i-xztar` sub-milestone under M3 below for implementation details. Source-tarball `.tar.xz` measurement still pending — it's the next-session item if real-world coverage reveals headroom.

---

## Shipped milestone details

### M7 step 4 — --fast flag for parallel final-step xz encode (shipped 2026-05-13)
- New `--fast` pack flag parsed in `do_pack` (peer of `--slow`) and threaded through `min_pack_for_tier` / `pack_run` / `TierJob`. When set, the two final-step xz commands (CODEC_XZ_9E and CODEC_XZ_9E_X86) swap `--threads=1` for `--threads=0 --block-size=8388608`. Manifest layout unchanged; `xz -d` consumes single- and multi-block streams identically, so no decoder work needed.
- Block-size choice: preset 9e's 64 MiB LZMA2 dict drives a 192 MiB default block under `-T0`. Without an explicit block size, inputs below 192 MiB fit in one block and `-T0` doesn't actually parallelize — naive `--threads=0` gave only a ~2% improvement on silesia mozilla. 8 MiB blocks force per-CPU parallelism on every multi-MiB final-step encode at a measured +2.77% size cost. Sweep on mozilla 51 MB: T1 24.1 s → T0 8 MiB 4.3 s (5.6×, +2.77%) vs T0 16 MiB 7.6 s (3.2×, +1.61%). Picked 8 MiB because `--fast` is opt-in for the aggressive trade.
- CODEC_ZPAQ_M5 unchanged: zpaq is single-threaded in the 7.15 binary we ship, and `--fast` is documented as a no-op when combined with `--slow`.
- Measured wall time (silesia mozilla, single 51 MB file → bucket 0 only): default 23.4 s, --fast 4.0 s (−83%). Default bench unchanged (no `--fast` flag → byte-identical to master). All 43 default-bench fixtures + 9 `ZXLE_SLOW=1` fixtures round-trip OK. Ratios byte-identical: per-file 0.3383, solid 0.3326.
- M7 step 3 (unwrap + force_opaque parallelism) deferred: the existing min-pack opaque-pass skip condition is the common path on the default bench, so speculative parallelism would add opaque-pass work to most fixtures and regress wall time. Would need an upfront heuristic predicting when both passes will run; not pursued in this milestone.

### M7 step 2 — Parallel --slow + default cross-codec tier (shipped 2026-05-13)
- `do_pack` now spawns the slow tier (zpaq -m5 final) and the default tier (xz -9e final) on two pthreads when `--slow` is set on a total < 1 MB input. Each tier writes through `min_pack_for_tier` to its own out-path (`out` for slow, `out.def.tmp` for default), so their internal temp files don't collide. After both threads join, the smaller blob wins and we rename it to `out`.
- Pre-flight: the input total-size summation that previously came out of `pack_run` as a side effect now happens upfront via a stat loop, so the cross-codec gate (`total_in < 1 MB`) is decided before either thread launches.
- New `TierJob` struct + `tier_worker` thread entry in `src/zxle.c`; `#include <pthread.h>` added. Non-eligible cases fall through to a single sequential `min_pack_for_tier` (unchanged behavior).
- Measured (`ZXLE_SLOW=1 bash tests/bench.sh`): DOCX 13.5 s → 9.4 s (−31%), JAR 293 → 206 ms (−30%), JPEG 204 → 122 ms (−40%), MP3 856 → 423 ms (−51%). Non-eligible (≥1 MB) --slow fixtures unchanged. All --slow output sizes byte-identical to master.
- Round-trip OK across the full default bench (43 fixtures) and the 9 `ZXLE_SLOW=1` fixtures. Ratios byte-identical: 8-file per-file 0.3383, solid 0.3326. M7 steps 3–4 (unwrap+force_opaque parallelism, `--fast` flag) still pending.

### M7 step 1 — Parallel probe ladders in pack_xz / pack_zst (shipped 2026-05-13)
- New helper `try_run_parallel(cmds[], n, rcs[])` in `src/util.c` spawns one pthread per command, each calling `system()`, joins all, and returns the per-command exit codes. Falls back to serial `system()` for any thread `pthread_create` declines to spawn, so rcs[] is always fully populated.
- `pack_xz` and `pack_zst` build their full candidate list up front (ladder entries for xz, the 8-probe `(level, --long)` Cartesian product for zst), give each candidate its own `rt.<i>.xz` / `rt.<i>.zst` temp path, fire them all in parallel, then walk results in ladder priority order to pick the first match. Identical to the prior serial behavior in terms of which candidate wins and what bytes land in the recipe.
- Makefile: `LDFLAGS` gains `-lpthread` (resolves to MinGW winpthreads on Windows; built fine on the project's MinGW toolchain).
- Pack-time wins on the cases where probes don't match early: real_coreutils.deb 15.4 s → 6.4 s (−58%); real_coreutils_src.tar.xz 53.2 s → 33.1 s (−38%). Small-fixture overhead of 5–34% on inputs where the serial loop would have matched at probe 1–3 — absolute <1.2 s, accepted as a net win.
- Round-trip OK on all 23 bench fixtures + 8-file corpus. Ratios byte-identical (per-file 0.3383, solid 0.3326). M7 steps 2–4 (min-pack tier parallelism, unwrap+force_opaque parallelism, `--fast` flag) still pending.

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
- Limitations: only top-level PNGs in this milestone. STORED PNGs inside ZIP entries are addressed by M3c-png-zip below.

#### M3c-png-zip — PNGs inside ZIP entries (shipped 2026-05-01)
- Extends the M2/M3a ZIP unwrap path. New recipe op `OP_PNG_STORE = 0x05`: `(u8 op)(u32 raw_len)(u32 png_recipe_len)(png_recipe_bytes)`. Used for STORED ZIP entries (method=0) whose payload starts with the 8-byte PNG signature.
- Encode reuses `pack_png`; inflated IDAT bytes flow into the same solid stream as surrounding ZIP entries, the PNG recipe is embedded inside the outer ZIP recipe. Decode side calls `unpack_png` inline from `unpack_recipe`.
- Method=8 (deflated) PNGs stay on the existing redeflate/preflate path; in practice ZIP tools auto-store PNGs (already-compressed wall), same rationale as M3b-zip.
- Measured on `tests/corpus/zip-with-png.zip` (1 stored PNG + 2 deflate-9 DLLs, 1,263,952 B): zxle 1,060,279 vs xz-9e 1,258,232 → **−15.73%**. RT OK. All other fixtures preserved.

#### M3d — gzip wrapper (shipped 2026-05-02)
- New container kind `KIND_GZIP = 5`. Detection at top level: bytes `1F 8B 08` (gzip magic + CM=deflate). FLG bits parsed; FEXTRA / FNAME / FCOMMENT / FHCRC optional fields walked to find the start of the deflate body.
- Pack: inflate the deflate body, verify CRC32 + ISIZE against the 8-byte trailer, then try mode 0 (zlib-L9 raw redeflate matches → mode flag set, just store inflated bytes in solid). On mismatch, try mode 1 (preflate over the deflate body — GNU gzip's lazy-match heuristic differs from zlib-L9 so most real-world `.gz` files take this path). Either success path puts inflated bytes in the solid zstd-19 stream and stores `(hdr, mode, raw_len, def_len, [diff_len/diff if mode 1], trailer[8])` in the recipe.
- Unpack: rebuild the deflate body (raw-deflate-L9 in mode 0; preflate-rejoin in mode 1), then emit `header || body || trailer` byte-identical.
- Measured: **−11.45% vs xz-9e** on `ntdll.dll.gz` (zxle 1,029,252 vs xz-9e 1,162,316). All other fixtures preserved.
- Limitations: single-member gzip only. Multi-member streams (rare; concatenated `.gz` blobs) fall through to KIND_OPAQUE. `.gz`-wrapped tar (`.tar.gz`) gets the outer gzip stripped but the inflated tar then goes opaque to solid until a tar handler ships.

#### M3e — ustar tar (shipped 2026-05-02)
- New container kind `KIND_TAR = 6`. Detection at top level: `"ustar"` at offset 257 of the first 512-byte block. Walk 512-byte header blocks until two consecutive zero blocks (end-of-archive); emit headers + padding via OP_STRUCT, route regular-file payloads through `pack_png` / `try_brunsli_buf`, fall back to OP_STORE.
- Recipe reuses the existing OP_* vocabulary from KIND_ZIP, so `unpack_recipe` walks both. KIND_TAR adds zero new ops.
- Size parsed as octal; GNU base-256 size encoding rejected (high bit on size[0] → fall through to KIND_OPAQUE). Non-regular typeflags (dirs/links/longname) keep their header in OP_STRUCT and have no payload to route; a non-regular entry with non-zero size is conservatively STORE'd.
- Measured on `mixed.tar` (PNG + JPEG + 2 DLLs, 3,041,280 B): zxle 1,188,859 vs xz-9e 1,188,340 → **+0.04%** (519-byte noise tie); vs opaque zstd-19 1,253,549 → −5.16%. Media-only tar (PNG + JPEG): zxle 244,180 vs xz-9e 313,780 → **−22.18%**.

#### M3e-targz — gzip-wrapped tar (shipped 2026-05-03)
- KIND_GZIP recipe extended with a trailing `u8 inner_kind` (0 = inflated body bytes consumed from solid verbatim — old M3d behavior; 1 = inflated body is a ustar tar) plus, when `inner_kind==1`, a `(u32 tar_recipe_len, tar_recipe_bytes)` nested tar recipe. The nested tar recipe consumes from the same solid stream via `unpack_recipe`. Pack: after `pack_gz` inflates and CRC-verifies the body and picks mode 0/1, if the body sniffs as ustar (size ≥ 1024, multiple of 512, "ustar" at offset 257), `pack_tar` is run on the inflated bytes into scratch buffers; on success, those scratch buffers are committed instead of dumping `raw` to solid. Unpack: when `inner_kind==1`, `unpack_gz` materializes the tar bytes via `unpack_recipe` to a temp file, reads them back, then deflates as before.
- Measured on `mixed.tar.gz` (gzip -9 wrap of `mixed.tar`, 1,419,435 B): zxle 1,191,596 vs xz-9e 1,419,292 → **−16.04%**. Inner tar shows `4 regular (2 store, 1 jpeg-store, 1 png-store)`. RT OK. All other fixtures preserved within ±1 B (KIND_GZIP entries gained 1 byte for the new `inner_kind` flag).
- Limitation inherits M3d's single-member-gzip-only constraint and M3e-tar's ustar-only constraint. Multi-member `.gz` or non-ustar tar inside a `.gz` falls through to the existing M3d plain-solid path.

#### M3g-bz2tar — bzip2-wrapped tar (shipped 2026-05-04)
- New container kind `KIND_BZIP2 = 8`. Detection at top level: `BZh[1-9]` at offset 0. Pack shells out to `bzip2 -dc` to inflate (same shell-out pattern as `cbrunsli`/`packMP3`/`zstd`/`xz`), reads the raw bytes, then re-runs `bzip2 -<n>` and `cmp`s byte-for-byte against the original input. On match, raw bytes flow into the solid zstd-19 stream and the recipe stores `(u8 block_size, u32 raw_len, u32 orig_len, u8 inner_kind[, u32 tar_recipe_len, tar_recipe_bytes])`. On mismatch, falls through to KIND_OPAQUE.
- Inner-kind dispatch mirrors M3e-targz: when the inflated body is a ustar tar (size ≥ 1024, multiple of 512, "ustar" at offset 257), it routes through `pack_tar` so per-entry payloads (PNG via `pack_png`, JPEG via brunsli, DLLs to solid STORE) each get format-aware treatment instead of dumping opaque to solid.
- Measured on `mixed.tar.bz2` (bzip2 -9 wrap of `mixed.tar`, 1,395,196 B): zxle 1,188,877 vs xz-9e 1,395,332 → **−14.80%**. RT OK. All other fixtures preserved.
- Limitation: reproducibility relies on the system `bzip2` binary being deterministic for the chosen block size; verified per-pack by the cmp step. No preflate-equivalent for bzip2 — there's no fallback path within the kind, just KIND_OPAQUE on miss. Multi-stream concatenated `.bz2` files would need a multi-member extension (not done; concatenated bzip2 is rare in practice).
- Companion: new recipe op `OP_BZ2_STORE = 0x07` (same shape as OP_GZIP_STORE) routes `.bz2` files inside tar / ar entries through `pack_bz2`. Wired into `pack_tar` and `pack_ar` payload-dispatch chains. Measured on `bz2-in.tar` (ustar tar of one bzip2 -9 DLL + one stored DLL, 1,935,360 B): zxle 1,327,842 vs xz-9e 1,413,036 → **−6.03%**.

#### M3h-zsttar — zstd-wrapped tar (shipped 2026-05-04)
- New container kind `KIND_ZSTD = 9`. Detection at top level: zstd magic `28 B5 2F FD` at offset 0. Pack shells out to `zstd -d` to inflate, then re-runs `zstd -19 --long=27` and `cmp`s byte-for-byte against the original. On match, raw bytes flow into the solid zstd-19 stream and the recipe stores `(u32 raw_len, u32 orig_len, u8 inner_kind[, u32 tar_recipe_len, tar_recipe_bytes])`. On mismatch, falls through to KIND_OPAQUE.
- Inner-kind dispatch mirrors M3g-bz2tar / M3e-targz: ustar tar payloads go through `pack_tar` so per-entry payloads (PNG via `pack_png`, JPEG via brunsli, DLLs to solid STORE) each get format-aware treatment.
- Measured on `mixed.tar.zst` (zstd -19 --long=27 wrap of `mixed.tar`, 1,253,451 B): zxle 1,188,876 vs xz-9e 1,253,580 → **−5.16%**. RT OK. All other fixtures preserved.
- Reproducibility: pack-time probes a 7-entry ladder of `(level, --long_window)` combos and stores the matching pair in the recipe (u8 level, u8 long_window). Order: `-19 --long=27` (matches our internal solid output), `-3` (zstd CLI default — most common in the wild), `-3 --long=27`, `-22 --long=27`, `-19`, `-22`, `-1`. First cmp match wins; if none match, KIND_OPAQUE.
- Two fixture data points: `mixed.tar.zst` (level 19, 1,253,451 B → −5.16%); `mixed.tar.zst3` (level 3, 1,419,046 B → **−15.79%**). The default-level fixture is the more representative real-world shape and gives the bigger headline because xz-9e on a `-3` zstd stream still can't crack its body.

#### M3h-zsttar frame-header probing (shipped 2026-05-05)
- `pack_zst` parses the zstd frame header (RFC 8478 §3.1.1.1) instead of guessing. From byte 4 (Frame_Header_Descriptor): FCS_flag → io mode (file if FCS present, stdin if absent), Content_Checksum_flag → `--check`/`--no-check`, Dictionary_ID_flag (non-zero → bail to OPAQUE), Single_Segment flag → window descriptor present or not. From byte 5 (Window_Descriptor, when present): `window_log = 10 + exponent`.
- Probe ladder: `level ∈ {19, 22, 20, 21, 18, 17, 9, 6, 3, 1} × long ∈ {27, observed_window_log, none}` (dedup'd), io mode and check flag pinned from header. `--long=N` is *not* idempotent across N — verified empirically against makepkg output — so the observed value is one of the probes (along with our internal 27 to keep the synthetic fixture path).
- Recipe layout bumped to `(u8 level, u8 window, u8 flags, u32 raw_len, u32 orig_len, u8 inner_kind, [...])`. `flags` bits: `0x01 = use_stdin (FCS suppressed)`, `0x02 = no_check`. `window=0` in the recipe means "no `--long` arg used" (so the value 0 is a sentinel, not a real window).
- Measured on `which.pkg.tar.zst` (Arch makepkg `core/which-2.23-1`, 16,366 B): kind=tar (zst engaged → inner ustar dispatched, 3 gzip-store + 3 store entries), matched at `level=19 long=25 io=stdin check=on`, RT OK. Headline unchanged at −0.09% vs xz-9e because the file is small enough that min-pack picks the opaque candidate, but the structural fixture-only constraint is gone. All synthetic and real-world fixtures preserved (mixed.tar.zst gained 1 byte for the new `flags` recipe byte).
- Limitations: dictionary frames bail to KIND_OPAQUE. Multi-threaded zstd (`-T0`) output is non-deterministic per worker assignment; not addressed here, will fall through to KIND_OPAQUE on larger packages.

#### M3i-xztar — xz-wrapped tar (shipped 2026-05-05)
- New container kind `KIND_XZ = 10`. Detection at top level: 6-byte xz magic `FD 37 7A 58 5A 00`. Pack shells out to `xz -dc` to inflate, then probes a small `(level, --extreme)` ladder — `{9e, 9, 6e, 6, 3e, 3, 1, 0}` — pinning `--threads=1` for determinism. First match (`cmp` byte-for-byte) wins; mismatches fall through to KIND_OPAQUE. Recipe: `(u8 level, u8 flags, u32 raw_len, u32 orig_len, u8 inner_kind, [tar_recipe])`; `flags` bit `0x01 = --extreme`.
- Inner-kind dispatch mirrors M3g-bz2tar / M3h-zsttar: ustar tar payloads (`raw_n >= 1024` and `"ustar"` at offset 257) route through `pack_tar` so per-entry payloads (PNG / JPEG / gzip / bzip2 inside) get format-aware treatment.
- Measured on `mixed.tar.xz` (xz -9e wrap of `mixed.tar`, 1,188,332 B): zxle 1,188,412 vs xz-9e 1,188,460 → **tie (−0.00%)**. xz already crushes the mixed corpus to its theoretical floor, so the inner-tar candidate (1,232,231) loses to opaque-solid (1,188,412) and min-pack picks opaque. Media-only variant `media.tar.xz` (PNG + JPEG ustar in xz -9e, 313,780 B): zxle 286,833 → **−8.59%** via inner-tar + PNG IDAT recompression (RT exercised via the unwrap path). RT OK across both. All other fixtures preserved byte-exactly.
- Reproducibility: xz-utils single-threaded encode is deterministic for a given liblzma version; pack-time cmp is the gate. Multi-threaded inputs (`-T0`) are non-deterministic across worker assignments and fall through to KIND_OPAQUE — same caveat as zstd.

#### M3j-store-ops — OP_XZ_STORE / OP_ZSTD_STORE (shipped 2026-05-05)
- Two new recipe ops complete the in-tar/in-ar STORE family alongside `OP_GZIP_STORE 0x06` and `OP_BZ2_STORE 0x07`: `OP_XZ_STORE = 0x08` and `OP_ZSTD_STORE = 0x09`. Layout matches the rest of the family — `(u8 op)(u32 raw_size)(u32 inner_recipe_len)(inner_recipe_bytes)` — and the embedded recipe is consumed by the existing `unpack_xz` / `unpack_zst`.
- Wired into `pack_tar` and `pack_ar` payload-dispatch chains (after the gzip/png/jpeg/bz2 detection branches, before the OP_STORE fallback). Detection is the same magic-byte sniff used at the top level (xz: `FD 37 7A 58 5A 00`; zst: `28 B5 2F FD`). Inflated bodies flow into the same solid stream as surrounding entries.
- Fixtures: `xz-in.tar` (1,792,000 B; ntdll.dll.xz + kernel32.dll) and `zst-in.tar` (1,853,440 B; ntdll.dll.zst + kernel32.dll). Bench:
  - `xz-in.tar`: zxle 1,288,657 vs xz-9e 1,269,040 → **+1.55%**. Unwrap candidate (1,329,401) loses to opaque (1,288,657); min-pack picks opaque. xz-9e on a PE DLL = 951 KB; solid zstd-19 of the inflated body ≈ 1.0 MB, so the cross-stream win from the second DLL doesn't recover the 50-KB gap.
  - `zst-in.tar`: zxle 1,329,945 vs xz-9e 1,328,532 → **+0.11%** (effective tie). Unwrap path wins this one (no min-pack swap fired).
- Milestone value is **correctness coverage** — completing the OP_*_STORE vocabulary so that nested gz/bz2/xz/zst payloads inside tar / ar are all routed through their format-aware handlers (and contribute to solid for any cross-stream wins available). Headline tie / mild regression matches the M3i-xztar shape: when the inner content is at xz/zstd's floor, unwrapping pays recipe overhead without recovering the gap, and min-pack falls back to opaque.
- RT OK on both new fixtures. All M1–M3i fixtures preserved byte-exactly.

#### min-pack fallthrough (shipped 2026-05-05)
- New `pack_run(out, n, files, force_opaque, ...)` helper: lifted from `do_pack`'s body. When `force_opaque=1` every input is stored as KIND_OPAQUE (all magic-detection branches gated on `!force_opaque`); otherwise the unwrap chain runs as before. Returns the count of unwrapped entries plus the produced file size.
- New `do_pack` driver: pack with unwrap engaged → if any entry was unwrapped, also pack as all-opaque to a sibling `.opq.tmp` → keep the smaller; emit `min-pack: opaque <a> < unwrap <b> -> using opaque` to stderr on the rare opaque-wins path. Cost: 2× pack time on container-shaped inputs (single-pass on plain inputs, since nothing was unwrapped to begin with).
- Motivation: real-world bench (`tests/real_world.md`) revealed unwrapping a tightly-deflated 1.7 KB gzip into 7 KB of inflated text and feeding that to solid loses to the original gzip — solid zstd-19 cannot beat a near-optimal gzip encoding of the same bytes, and we pay preflate reconstruction + recipe + solid framing on top. Fix is correctness-safe (we only swap if smaller) and asymmetric in cost (the wins are large, the cost is just doubled pack time).
- Measured (real-world bench, before → after):
  - `leftpad.tgz` (npm, 1,679 B): +247.08% → **−1.09%** vs xz-9e
  - `is-odd.tgz` (npm, 2,774 B):  +144.44% → **−0.74%** vs xz-9e
  - `hello.deb` (Debian hello_2.10-3, 53,080 B): +0.04% → −0.04% vs xz-9e
- Synthetic fixtures: every M1 / M2 / M3a–h headline is byte-exact unchanged because the unwrap path wins on each, so the opaque candidate is computed and discarded. RT OK across the full corpus + real-world bench.

#### M3 sub-milestone status

All M3 sub-milestones shipped:
- M3a DEFLATE (preflate), M3b JPEG (brunsli), M3b-zip JPEG-in-ZIP — shipped above.
- M3c-mp3 MP3 (packMP3), M3c-png PNG IDAT, M3c-png-zip PNG-in-ZIP — shipped above.
- M3d gzip, M3e ustar tar, M3e-targz gzip-wrapped tar, M3e-tar-gz-in (OP_GZIP_STORE) — shipped above.
- M3f-ar Unix archive — shipped (see "Headline M3f-ar result" in current-state log).
- M3g-bz2tar bzip2-wrapped tar, M3g bz2-in-tar (OP_BZ2_STORE) — shipped above.
- M3h-zsttar zstd-wrapped tar, M3h-zsttar frame-header probing — shipped above.
- M3i-xztar xz-wrapped tar — shipped above.
- M3j-store-ops in-tar/in-ar `.xz` / `.zst` — shipped above.

Each recompressor lives behind an availability check; missing recompressors fall through to opaque-zstd.

PE streams via ZXL: see `roadmap.md` "Tried and reverted"; revisit once ZXL has a multi-stream/solid mode.

### M4 — Cross-stream content-defined ordering

**Status:** parked pre-implementation (2026-05-01). See `roadmap.md` "Tried and reverted" — measurement showed no headroom on sub-window corpora because zstd `--long=27` (128 MiB window) already captures cross-stream matches regardless of order. Revisit when (a) corpora routinely exceed the long-window size, or (b) we ship a non-solid block format where ordering matters per-block.

### M5 — Slow context-mixing final-step (shipped 2026-05-08)

`zxle pack --slow` finalizes the solid stream with `zpaq -m5` instead of `xz -9e`. Cleaner integration than originally scoped: not a new KIND or per-stream routing, just a codec swap at the solid-stream boundary. The unwrap pipeline still produces a single concatenated raw-byte stream; zpaq replaces xz at the very end. min-pack still runs both unwrap and force_opaque passes; both use whichever final codec --slow selects.

Wire format: ZXLE_VER stays at 3. Flags byte bit 0 (0x01) = trailing payload is zpaq -m5; do_unpack dispatches accordingly. v3 files written by default-mode binaries (flags=0) decode unchanged through the xz path.

Headline impact: see "Current state" log above. Silesia gap closed (zxle --slow = 0.1891, matches zpaq -m5 within 2 KB). Container fixtures gain a further 11–35% over default zxle (−29 to −47% vs xz-9e baseline). The original JAR/JPEG/MP3 small-input regressions (+6.20% / +0.87% / +0.26% vs default) were closed 2026-05-08 by the per-fixture min-pack tier — `--slow` is now a strict pareto improvement (never larger than default, often much smaller).

Code: ~80 LoC in `src/zxle.c` (--slow flag parsing, codec dispatch in pack_run + do_unpack, zpaq archive extract-and-rename in unpack since zpaq is journaling-format not stream-format). No new translation unit; zpaq is shell-out at the solid-stream boundary, same pattern as xz/zstd/bzip2/cbrunsli/packMP3.

Pack-time: 5–10× default xz mode (Silesia 327 s vs 204 s). Unpack: similar (zpaq pack/extract are roughly symmetric).

### M6 — Per-content-type codec routing within the solid stream (v1+v2 shipped 2026-05-08, v3 shipped 2026-05-09)

**v1 scope shipped:** top-level KIND_OPAQUE entries are sniffed by magic-byte (PE "MZ" / ELF "7F E L F") and routed to a separate sub-stream finalized with `xz -9e --x86` (BCJ filter + LZMA2). Other content (text, mixed binary, already-compressed) goes to the main bucket with the requested codec (xz-9e or zpaq-m5 per `--slow`). Manifest gains a u8 opaque_bucket per KIND_OPAQUE entry; trailing payload is multi-bucket. ZXLE_VER bumped 3 → 4.

**v1 impact:**
- ntdll.dll: 951,366 → **921,762** B (−3.1%)
- kernel32.dll: 316,565 → **309,601** B (−2.2%)
- user32.dll: 574,203 → **562,531** B (−2.0%)
- 8-file corpus per-file ratio: 0.3453 → **0.3383**; solid: 0.3391 → **0.3326**
- vs xz-9e baseline (0.3524): was −3.8% smaller → now **−5.6% smaller**
- Container fixtures: byte-identical (their inflated bytes flow through bucket 0 by design)

**v2 shipped 2026-05-08:** container-aware bucket routing for KIND_ZIP/TAR/AR/GZIP/BZIP2/ZSTD/XZ. Each kind has its own sniffer:

- ZIP/TAR/AR: walk in-memory directory, count PE-extension filenames (.dll/.exe/.sys/.drv/.efi/.so/.o/.obj), bucket 1 if ≥ 50%.
- GZIP/BZIP2/ZSTD/XZ: shell out to inflate first ~2 KiB and scan for MZ / 7F E L F magic anywhere. Catches gz-of-PE, gz-of-tar-of-PE, xz-of-PE, etc.

Each kind's manifest entry gains a u8 unwrap_bucket after the kind byte; unpack dispatches the recipe walker to `bucket_bytes[unwrap_bucket]`. PNG keeps bucket 0 fixed (pixel data is never x86). ZXLE_VER bumped 4 → 5.

Headline impact (default mode):
| Fixture | M6 v1 | **M6 v2** | Δ |
|---|---|---|---|
| pe-deflate.zip | −20.45% | **−22.44%** | +2.0 pp |
| pe-deflate-l6.zip | −20.78% | **−22.76%** | +2.0 pp |
| zip-with-jpeg.zip | −19.85% | **−21.95%** | +2.1 pp |
| zip-with-png.zip | −22.07% | **−23.26%** | +1.2 pp |
| ntdll.dll.gz | −16.69% | **−19.23%** | +2.5 pp |
| gz-in.tar | −14.68% | **−17.10%** | +2.4 pp |
| bz2-in.tar | −11.57% | **−14.09%** | +2.5 pp |
| xz-in.tar | −1.41% | **−4.22%** | +2.8 pp |
| zst-in.tar | −5.79% | **−8.47%** | +2.7 pp |

Mixed-content tarballs (mixed.tar.gz, mixed.deb, mixed.tar.bz2 etc.) unchanged — wrapper sniff routes them to bucket 1 (PE majority by bytes), but BCJ's gain on the PE half is offset by small overhead on the JPEG/PNG portions that share the bucket. Per-OP bucket routing (M6 v3, see below) closes that.

**v3 shipped 2026-05-09:** per-OP bucket routing in the recipe walker. The v5 manifest u8 unwrap_bucket field is gone; each OP_STORE/OP_REDEFLATE/OP_PREFLATE op gains a u8 bucket after its u32 raw_size, and KIND_PNG/GZIP/BZIP2/XZ/ZSTD recipes gain a u8 bucket field used when inner_kind==0. Lets a mixed.tar.gz route DLL bytes to BCJ and JPEG/PNG bytes to LZMA2 simultaneously. The v2 container-level sniffers (zip_is_pe_heavy / wrapped_is_pe_heavy / etc.) are replaced by one `bucket_for_bytes()` helper that runs on already-inflated bytes inside each pack_*. Net code -124 lines. ZXLE_VER bumped 5 → 6.

Headline impact (default mode, M6 v2 → v3):
| Fixture | M6 v2 | **M6 v3** | Δ |
|---|---|---|---|
| mixed.tar (PNG+JPEG+2 DLLs) | 1,188,859 | **1,091,252** B | −8.2% |
| mixed.deb (ar→gz→tar→DLLs+text) | 1,258,334 | **1,222,430** B | −2.85% |
| mixed.tar.gz | −21.66% | **−22.92%** | −1.26 pp |
| mixed.tar.bz2 | −20.51% | **−21.79%** | −1.28 pp |
| mixed.tar.zst3 (default-3) | −21.44% | **−22.70%** | −1.26 pp |
| mixed.tar.zst (level-19) | −6.68% | **−8.18%** | −1.50 pp |
| zip-with-png.zip | −23.26% | **−23.49%** | −0.23 pp |

Pure-PE fixtures unchanged (M6 v2 already routed everything to bucket 1). Pure-text fixtures unchanged. JAR pays 31 bytes from per-OP bucket overhead (32 entries × 1 byte) — still −59.33% vs xz-9e. RT OK on all 23 fixtures + 8-file corpus; fuzz harness 210/210 clean.

**Possible v4 directions** (no concrete plan; tracked in `roadmap.md` Future-work / Per-stream improvements): entropy + printable-ratio sniffer for text-vs-binary classification on Silesia-shaped flat corpora; deeper AR member sniff; third bucket as a delta filter for PCM-ish data.

### Parser fuzz harness (`tests/fuzz.sh`, shipped 2026-05-09)

Container-parser fuzz harness. ~50 mutations × 7 kinds (ZIP/TAR/AR/GZIP/BZIP2/XZ/ZSTD) per run with a per-iteration timeout; bit-flips, byte-flips, truncations, header zeroing/randomization. Asserts `zxle pack <ANY_BYTES>` exits 0 in bounded time — any parser that fails to inflate a malformed container must fall through to KIND_OPAQUE.

First run uncovered the `raw_inflate_dyn` truncated-stream infinite-realloc hang: zlib's `Z_BUF_ERROR` is overloaded ("output room exhausted" *and* "input exhausted mid-stream under Z_FINISH"); the dyn loop treated both as "need bigger output buffer" and reallocated until it OOM'd via `die()` (45-second hang on a truncated 1 MB gzip). Fix: bail out (return NULL) when `avail_in == 0` on `Z_BUF_ERROR`/`Z_OK`. Also replaced two `die()` calls in realloc-failure paths with NULL returns so the parser opaque-routes cleanly under allocation pressure.

After fix: 700 mutations across two seeds (50 × 7 × 2) all clean — 0 fail / 0 crash / 0 hang. Future expansion: AFL/libfuzzer harness against per-parser entry points if the bash harness stops finding bugs.
