# ZXL-E improvement review — triage record (reviewed 2026-07-14, executed same day)

The full review text that used to live here has been executed and folded into the
standing docs. This file is now just the disposition table so the review isn't
re-litigated; delete it whenever it stops being useful. Shipped items are detailed in
[delivered.md](delivered.md) ("Current state 2026-07-14" + milestone blocks); deferred
items live in [roadmap.md](roadmap.md) marked **[2026-07-14 review]**.

| # | Item | Disposition |
|---|---|---|
| 1.1 | Manifest/recipes stored uncompressed | **Shipped** (v7, variant a: whole-manifest xz). Variant (b) — STRUCT bytes into bucket 0 — deferred to roadmap ("Manifest STRUCT bytes: variant (b) unexplored") |
| 1.2 | Nested-dispatch asymmetry | **Partially shipped** (OP_ZIP_STORE: zip-in-tar/ar). Remaining axes in roadmap ("Nested-dispatch remaining axes") |
| 1.3 | GNU/pax tar widening | Deferred → roadmap (priority raised; measure `git archive` output first) |
| 1.4 | ZIP64 / ZIP variants | Deferred → roadmap (corpus-scan 50 real ZIPs first) |
| 1.5 | Raw-deflate scan in opaque streams (PDF etc.) | Deferred → roadmap (new bullet under coverage gaps) |
| 1.6 | BCJ x86 filter applied to ARM64 PE/ELF | **Shipped** (machine-aware `bucket_for_bytes`) |
| 1.7 | --fast fixed 8 MiB blocks overpay at scale | **Shipped** (input-scaled blocks; 1 GB size cost +3.24% → +0.58%) |
| 1.8 | M4 rationale stale / 64 MiB dict ceiling | Documented → roadmap ("Solid window at scale" + note in M4 graveyard entry) |
| 1.9 | Middle tier between default and --slow (bsc/zpaq-m4) | Deferred → roadmap (under "Text / source code") |
| 1.10 | packJPG/lepton vs brunsli; varints; PNG unfilter | Deferred → roadmap (JPEG gap now *measured*: precomp\|xz beats us −0.70% on synth.jpg) |
| 2.1 | Decode depends on system tool versions | **Detection shipped** (v7 crc32 fails hard on drift). Prevention (record versions / vendor liblzma+libzstd) → roadmap |
| 2.2 | No integrity checking in the format | **Shipped** (per-entry crc32, verified at unpack) |
| 2.3 | Path traversal + basename collision + shell injection | **Shipped** (name sanitization at unpack, duplicate refusal at pack). argv-spawn for shell-quoting robustness rides with the vendoring item in roadmap |
| 2.4 | mode stored but unapplied; no mtimes | Deferred → roadmap ("Fidelity: mode/mtime") |
| 2.5 | 2 GiB ftell cap on Windows; u32 csize | **Shipped** (64-bit IO; csize u64) |
| 2.6 | Structured fuzzing | Already in roadmap; unchanged |
| 3.1 | Per-entry recompression serial | Deferred → roadmap (M7 "step 5" note under multi-threaded pack) |
| 3.2 | Process spawn per probe | Deferred → roadmap (same work item as 2.1 prevention) |
| 3.3 | min-pack double-pack heuristic | Deferred → roadmap (under pack-time item) |
| 3.4 | Unpack parallelism | Dropped for now (decode already ~35× faster than pack) |
| 4.1 | 7-Zip + precomp\|xz baselines missing | **Shipped** (bench sections + `make 7zip-deps` + silesia 7z baseline) |
| 4.2 | No CI / unit tests | Deferred → roadmap (validation gaps) |
| 4.3 | Peak RSS unreported | Already in roadmap; unchanged |
| 5 | M8 strategy notes | Folded into roadmap M8c context (M8a gate binary OOM >30 MB noted there) |

Headline outcomes of the executed items: sample.jar −51% vs v6 (−80.22% vs xz-9e, beats
precomp), pptx −2.8%, mixed.tar family −1%, zip-in.tar −5.61% vs xz-9e (new capability),
--fast at 1 GB +0.58% size cost (was +3.24%), and unpack now fails hard on corruption or
tool-version drift instead of silently emitting wrong bytes. 66/66 bench fixtures RT OK.
