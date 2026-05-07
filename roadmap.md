# ZXL-E Roadmap

Single source of truth for where ZXL-E is, where it's going, and what not to retry.

---

## Current state (2026-05-05, M3j store-ops shipped)

M1 + M2 + M3a (preflate) + M3b (brunsli) + M3b-zip (JPEG-in-ZIP) + M3c-mp3 (packMP3) + M3c-png (zlib-L9 / preflate over IDAT) + M3c-png-zip (PNG-in-ZIP) + M3d-gzip (single-member gzip wrapper) + M3e-tar (ustar per-entry dispatch) + M3e-targz (gzip-wrapped tar) + M3e-tar-gz-in (gzip files inside tar) + M3f-ar (Unix archive: .a / .deb) + M3g-bz2tar (bzip2-wrapped tar) + M3h-zsttar (zstd-wrapped tar) + min-pack fallthrough + zstd frame-header probing + M3i-xztar (xz-wrapped tar) + M3j-store-ops (OP_XZ_STORE / OP_ZSTD_STORE) ship end-to-end.

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

## Next session — after real-tarxz-src measurement + pack_xz tuning (2026-05-07)

The OP vocabulary is structurally complete for the gz/bz2/xz/zst container family, the encode-time hot spots in pack_zst and pack_xz are bounded, and three real-world fixtures (real_hello.deb / real_coreutils.deb / real_coreutils_src.tar.xz) cover the canonical `.tar.xz` and `.tar.zst` shapes. All three sit at the universal-codec floor (within 200 B of xz-9e). Remaining items are **measurement / quality / wider coverage**, not new container code:

- **Multi-threaded zstd reproducibility** — confirmed 2026-05-07 on real Ubuntu coreutils.deb that the data-layer `.tar.zst` (1.4 MB) is encoded with `-T0` and falls through to OP_STORE because per-worker frame splits are non-deterministic. The 2026-05-07 multi-frame fast-fail in pack_zst now bails on those inputs in milliseconds. Headline-positive routing on multi-threaded `.tar.zst` would require a multi-frame-aware probe that recognizes worker boundaries and reproduces each frame independently. Large work, uncertain payoff — most real-world `.tar.zst .deb`s sit at the universal-codec floor anyway.
- **Widen pack_xz reproducibility for non-preset encoders** — empirically established 2026-05-06 (Debian hello, dict=8MiB, custom mf/mode/nice/depth) and 2026-05-07 (GNU coreutils-9.11.tar.xz, dict=32MiB, ditto). The 2026-05-07 dict-driven pruning + bail correctly identifies these as unreachable in 2 probes (vs 8) but the headline still ties at floor. Real wins would require either (a) widening probe space with `mf` × `mode` × `nice` × `depth` permutations (large search, slow, uncertain payoff — likely doesn't reproduce libzstd-direct or GNU-release-script outputs anyway), or (b) reading lzma2 encoder choices from the stream itself (block-level filter parameters). Defer until validation gaps below close.
- **Validation gaps from "Future work"** — competitor benchmarks (precomp / freearc / zpaq), size-scaling data past the 128 MiB long-window, fuzz testing of container parsers, peak-RSS reporting in bench. Of these, **competitor benches** would most directly inform whether the headline-positive container shapes (M2 ZIP, M3a preflate, M3b brunsli, M3c MP3/PNG, M3e-targz, etc.) hold up vs. precomp / zpaq on the same fixtures. **Fuzz testing** of `pack_zip` / `pack_tar` / `pack_ar` / `pack_gz` / `pack_bz2` / `pack_xz` / `pack_zst` is the highest-impact safety win before any external adoption.

### Done (kept for context): real GNU-release `.tar.xz` ties xz-9e + pack_xz dict-driven pruning (measured + shipped 2026-05-07)

Pulled `coreutils-9.11.tar.xz` (GNU release, 6,562,420 B; dict=32 MiB per `xz -lvv`) into `tests/corpus/real_coreutils_src.tar.xz` (gitignored). Result: `zxle=6,562,636` vs `xz-9e=6,562,816` — **−0.00%** (180 B win), round-trip OK. Same floor-tie shape as `mixed.tar.xz`, `real_hello.deb`, and `real_coreutils.deb`.

**Why the tie:** dict=32 MiB maps to xz preset 8 only; pack_xz now probes (8e, 8) — both fail because GNU's release script uses `xz` with non-preset lzma2 sub-parameters (custom `mf`/`mode`/`nice`/`depth` beyond preset 8's defaults). Bail to KIND_OPAQUE; min-pack picks opaque. Predicted shape — text-heavy source `.tar.xz` at xz-9e is already at the codec floor; per-entry tar routing would only help if the inner tar contained media, which a small source distribution doesn't.

**What shipped alongside the measurement:** pack_xz now (a) parses the LZMA2 dict-size byte from the xz block header and probes only preset levels with matching dict (8 → 2 probes for this fixture; falls back to the original 8-probe ladder if header parsing fails); (b) bails immediately on multi-stream input via second-magic scan (xz `-T0` worker output). Wall-time: 176 s → 67.7 s (−62%) on this fixture; small bonuses on `real_hello.deb` (1.5 s → 964 ms) and `real_coreutils.deb` (19.6 s → 18.3 s) because their inner `.tar.xz` members also engage the new path. Sizes preserved byte-exact across all fixtures.

**Implication:** real `.tar.xz` from established release scripts (GNU, Debian) consistently uses lzma2 sub-parameters our preset-only ladder can't reproduce. The fast-fail keeps us correct via min-pack opaque; chasing headline-positive routing on these inputs requires either custom-param probing or stream-driven param extraction, both deferred (see "Widen pack_xz" in next-session list).

### Done (kept for context): real `.tar.zst` Debian-family `.deb` ties xz-9e (measured 2026-05-07)

Pulled `coreutils_9.5-1ubuntu1_amd64.deb` (Ubuntu 24.04, 1,465,358 B; ar → control.tar.zst + data.tar.zst) into `tests/corpus/real_coreutils.deb` (gitignored). Result: `zxle=1,465,450` vs `xz-9e=1,465,500` — **−0.00%** (50 B win, effective tie), round-trip OK. All three universal codecs (xz/zstd/zxle) within 150 B of orig — canonical "already-compressed wall" shape, same as `real_hello.deb` (.tar.xz data layer).

**Routing engagement (third-party `.tar.zst` first-time exercise):**
- `control.tar.zst` (small, single-thread) → KIND_ZSTD via frame-header probing matched (`level=19 long=27 io=file check=on`), inner ustar tar dispatched. Confirms the M3h-zsttar frame-header-probing milestone works on a real third-party stream beyond `which.pkg.tar.zst`.
- `data.tar.zst` (1.4 MB, multi-threaded `-T0`) → falls through to OP_STORE. Multi-threaded zstd splits the input across workers and emits multiple concatenated frames at non-deterministic boundaries; `pack_zst` only handles single-frame inputs and bails. **The documented `-T0` non-determinism risk from M3h-zsttar is now empirically confirmed** on real Ubuntu .debs.
- min-pack picks opaque (1,465,450 < 1,472,779 unwrap candidate, 7-KB recipe overhead unrecovered).

**Implication:** real `.tar.zst` .debs land in the same bucket as real `.tar.xz` .debs — opaque-wins ties at the codec floor, confirming both `.deb` shapes via the safe min-pack fallthrough. The KIND_ZSTD frame-header probe path is now proven on two real third-party streams (Arch `.pkg.tar.zst` and Ubuntu control layer); the multi-threaded data-layer gap is a known, bounded limitation.

### Done (kept for context): real `.deb` ties xz-9e (measured 2026-05-06)

Pulled `hello_2.10-3_amd64.deb` (53,080 B; data layer `.tar.xz`, dict=8MiB, CRC64) into `tests/corpus/real_hello.deb` (gitignored). Result: `zxle=53133` vs `xz-9e=53148` — **−0.03%** (15 B win), round-trip OK. Same shape as M3i-xztar tie: when the input is already-compressed at high ratio, xz-9e barely compresses further and our opaque path stores+overhead → near-tie.

**Why the synthetic `mixed.deb` headline (−13.31%) does NOT generalize:** synthetic uses inner `.tar.gz` whose deflate streams reproduce via preflate, so OP_GZIP_STORE fires on the data member and per-entry routing wins. Real Debian `.deb`s use inner `.tar.xz` whose lzma2 stream uses non-preset encoder params (probed: dict=8MiB matches but `mf/mode/nice/depth` don't match any of `xz -0..9` or `-0e..9e`; closest is `mf=bt4,mode=normal,dict=8MiB` at 51012 B vs target 51020 B — 8 B off). pack_xz's preset-only ladder returns -1 → all 3 ar members fall through to OP_STORE → opaque path picked by min-pack.

**Implication:** `mixed.deb` as the only `.deb` fixture overstated real-world wins. Real `.deb` is now in the bench as `M3f-ar real .deb (hello_2.10-3)` documenting the actual shape. Headline-positive routing on real .debs requires either (a) widening pack_xz to probe `mf/mode` permutations (large search, slow), or (b) Debian Trixie's `.tar.zst` data-layer .debs where pack_zst's frame-header probing should match.

See "Future work" below for the broader gap inventory.

### Done (kept for context): zstd ladder cannot reproduce real `.pkg.tar.zst`

The 7-entry `(level, --long)` ladder in `pack_zst` fires KIND_OPAQUE on every real Arch package because none of its probes are byte-exact. The right fix is **frame-header-driven probing**: parse the input's zstd frame header, extract observable parameters, and use them to pin the encoder command precisely.

**Probing data gathered 2026-05-05 on `which.pkg.tar.zst`** (don't re-derive next session — pick up from here):

The frame header (RFC 8478 §3.1.1.1) for `which.pkg.tar.zst` is `28 b5 2f fd 04 78 ...`:
- byte 4 = `0x04` Frame_Header_Descriptor → FCS_flag=00, Single_Segment=0, Checksum=1, Dictionary_ID=00. So: **no FCS bytes, no dict ID, content checksum on**.
- byte 5 = `0x78` Window_Descriptor → exponent=15, mantissa=0 → **window_log = 25** (32 MiB window).

The reproducing command, found by manual probe: `zstd -20 --long=25 -q --no-progress < raw > out` (stdin, since no FCS in header → must encode via stdin to suppress FCS that file-mode `-o ... <input>` would write). Single byte-exact match across the level/window/io grid.

Three rules the empirical probing established:

1. **`--long=N` is NOT idempotent across N**, even when the encoded window_log in the header is identical. On `which.pkg.tar`, `-20` (no `--long`), `-20 --long=23`, `-20 --long=25`, `-20 --long=27` all produced 16,366 B but only `--long=25` byte-matched. Implication: we *must* try the observed `--long` value (and probably a small neighborhood), not just any value ≥ window_log.
2. **FCS presence is observable from byte 4 (FCS_flag bits)**. zstd CLI writes FCS in `-o output input` (file→file) mode but suppresses it in stdin mode. So pinning is exact: observed FCS=present → file mode; observed FCS=absent → stdin mode.
3. **Default-mode synthetic fixtures (mixed.tar.zst) reproduce with the existing file-mode probe**, so the new code must keep that path working.

**Implementation sketch** (`pack_zst` in `src/zxle.c:1344`):

```
parse_frame_header(p, n) -> (window_log, has_fcs, has_checksum, dict_id_present)
if dict_id_present: return -1  // no dictionary, can't reproduce

build probe ladder = [22, 21, 20, 19, 18, 17, 9, 6, 3, 1] for level
for each level:
  for each io_mode in [observed_io_only]:  // file if has_fcs else stdin
    for each long_arg in [--long=window_log, no --long]:
      cmd = build with check flag from has_checksum
      probe; cmp; if match -> store (level, window_log, flags) in recipe
```

**Recipe layout change**: bump KIND_ZSTD recipe to `(u8 level, u8 window_log, u8 flags, u32 raw_len, u32 orig_len, u8 inner_kind, [...])`. flags bits: `0x01 = use_stdin (FCS suppressed)`, `0x02 = no_check (checksum disabled)`. Update `unpack_zst` symmetrically. No fixture-file commits to invalidate — `.zxle` outputs are regenerated by bench.sh each run.

**Sanity check** before claiming done: re-run `bench_one.sh which.pkg.tar.zst`. Expected: `kind=zstd` (not opaque) and the inner tar dispatched. Likely small headline change (the package is small enough that solid-frame overhead dominates), but the proof is engagement of KIND_ZSTD on a real third-party stream — the synthetic-fixture-only constraint goes away.

**Risk**: makepkg uses multi-threaded zstd (`-T0` / `--auto-threads`) on larger packages. Multi-threaded output is non-deterministic per worker assignment and won't reproduce single-threaded. For small packages like `which` (one frame, single-threaded effectively), this isn't a problem. For larger Arch packages it may be — pull a 5–10 MB package into the bench corpus to measure. If multi-threaded output breaks reproducibility, document and accept fall-through-to-OPAQUE for that subclass.

### Done (kept for context): KIND_XZ (shipped 2026-05-05 as M3i-xztar)

Predicted shape held: mixed-content `.tar.xz` ties xz-9e (xz already crushes mixed.tar to its floor; min-pack picks opaque); media-heavy `.tar.xz` wins ~−8.6% via inner-tar + PNG/JPEG-aware routing. See `M3i-xztar` sub-milestone under M3 for implementation details. Source-tarball `.tar.xz` measurement still pending — it's the next-session item if real-world coverage reveals headroom.

---

## Future work (coverage gaps and unknowns)

Roughly ordered by real-world impact-to-effort ratio. The ones near the top should be funded first.

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
- **No fuzz testing.** Container parsers (zip / tar / ar / gzip / bz2 / zst) all walk untrusted-shaped input. A 1-day AFL/libFuzzer pass on each `pack_*` would surface issues before real-world adoption.
- **No corruption-tolerance story.** zstd-19 solid + format-aware recipes mean a single corrupted byte in the payload likely loses everything. Document the failure mode; consider whether per-entry framing is worth adding (probably not — it costs ratio).
- **No streaming pack/unpack.** Whole-file `read_whole_file` everywhere. Fine for ≤1 GB; falls over above. Defer until a real workload demands it.
- **Manifest format frozen at v2 since M2.** No version-skew testing; no migration story. Document the format more thoroughly in `roadmap.md` if external tools ever consume `.zxle`.

### Architecture-level open questions

- **M4 cross-stream ordering** — parked. Revisit only if the corpus exceeds the long-window or we move to a non-solid block format.
- **M5 neural residual fallback** — pending. Probably only worth shipping after the container-coverage and per-stream story is broadly solid; otherwise the slowdown (NNCP-class) buys little above what's already achievable.
- **Block / streaming output format** — current container is solid-only. A block-mode variant would enable random access and corruption tolerance at a (small) ratio cost. No demand yet.

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

#### M3c / M3d / M3e — pending sub-milestones
**Branch:** `feat/m3-*` · **Expected:** large wins per stream.

- ~~MP3 via packMP3~~ — shipped as M3c-mp3 above.
- ~~PNG byte-exact via IDAT-zlib-L9 / preflate~~ — shipped as M3c-png above.
- ~~PE streams via ZXL~~ — see "Tried and reverted"; revisit once ZXL has a multi-stream/solid mode.
- ~~JPEGs inside ZIP entries~~ — shipped as M3b-zip above.
- ~~gzip single-member wrapper~~ — shipped as M3d-gzip above.
- ~~ustar tar per-entry dispatch~~ — shipped as M3e-tar above.
- ~~gzip-wrapped tar~~ — shipped as M3e-targz above.
- ~~gzip files inside tar~~ — shipped (added `OP_GZIP_STORE` to the OP vocabulary, wired into `pack_tar` alongside the new M3f-ar handler).
- ~~Unix AR archive (.a / .deb)~~ — shipped as M3f-ar above.
- ~~bzip2-wrapped tar~~ — shipped as M3g-bz2tar above.
- ~~bzip2 files inside tar/ar~~ — shipped (added `OP_BZ2_STORE` to the OP vocabulary, wired into `pack_tar` and `pack_ar` alongside `OP_GZIP_STORE`).
- ~~zstd-wrapped tar~~ — shipped as M3h-zsttar above.
- ~~xz-wrapped tar~~ — shipped as M3i-xztar above.
- ~~xz / zstd files inside tar / ar~~ — shipped as M3j-store-ops above (added `OP_XZ_STORE 0x08` and `OP_ZSTD_STORE 0x09` to the OP vocabulary, wired into both `pack_tar` and `pack_ar`).

Each recompressor lives behind an availability check; missing recompressors fall through to opaque-zstd.

### M4 — Cross-stream content-defined ordering
**Status:** parked pre-implementation (2026-05-01). See "Tried and reverted" — measurement showed no headroom on sub-window corpora because zstd `--long=27` (128 MiB window) already captures cross-stream matches regardless of order. Revisit when (a) corpora routinely exceed the long-window size, or (b) we ship a non-solid block format where ordering matters per-block.

### M5 — Neural residual fallback (optional)
**Branch:** `feat/m5-neural` · **Expected:** −5 to −15% on residuals where zstd is already near optimal.

Small autoregressive byte predictor (NNCP-class). Default-off due to slowdown; user opts in with `--slow`.

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
