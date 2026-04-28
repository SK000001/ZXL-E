# ZXL-E Workflow

Read this first when starting a new session. Mirrors ZXL's discipline; project specifics differ.

---

## Where to find current state

1. `roadmap.md` — single source of truth for shipped milestones, current bench, next-session plan, "tried and reverted" graveyard. **Read this before proposing anything.**
2. `README.md` — public-facing description with current architecture status table. Update when a milestone ships.
3. `tests/bench.sh` — run before starting work to verify current numbers.
4. `git log --oneline -10` — recent history.

Don't trust memory snippets about ratios — always re-bench.

---

## Session pattern

1. **Verify baseline.** `bash tests/bench.sh` and confirm round-trip OK on every file. Note headline numbers: average ratio across the corpus and per-format breakdown (ZIP-family / media / binary / text).
2. **Cut a feature branch.** `git checkout -b feat/<short-descriptive-name>`.
3. **Implement.** One milestone or sub-milestone per branch. Don't pile experiments.
4. **Build + bench.** `make 2>&1 | tail -3 && bash tests/bench.sh 2>&1 | head -20`. Round-trip must be `OK` on every file before any ratio claim.
5. **Decide.**
   - **Merge to master** if the corpus average improves (or stays flat) AND no file regresses meaningfully AND round-trip OK everywhere.
   - **Tune** within branch if close.
   - **Abandon** if net regression: `git checkout master && git branch -D feat/<name>`. Document in `roadmap.md` "Tried and reverted" with concrete numbers and the structural reason.
6. **Update docs** on master.
   - `roadmap.md`: move the entry to "shipped" or "tried and reverted".
   - `README.md`: refresh status table.
   - Commit docs separately with `docs:` prefix.

---

## Git rules

- **Never add `Co-Authored-By: Claude` (or any AI attribution) to commits.** Subject + body only.
- **Never `git push` without explicit user request.**
- **Always commit on a feature branch, fast-forward merge to master on success.** No merge commits. No force-pushes to master.
- **Commit messages**: short imperative subject (`feat:`, `tune:`, `docs:`, `test:` prefixes), then a paragraph with what + why + actual numbers.
- **`tests/corpus/`** (large sample files) and **`logs/`** are gitignored — local-only.

---

## Benchmarking rules

- **The corpus is heterogeneous on purpose.** Goal is "best across everything" — every category must be represented: ZIP-family (DOCX/JAR), media (JPEG/PNG/MP4), text (source/JSON/MD), binary (PE/ELF), already-compressed (xz/zstd archives). Regressions on one class are not OK without justification.
- **Round-trip OK is non-negotiable.** Bytes must come out identical to bytes that went in. Format-aware unwrapping makes this hard for ZIP-family — see "M2 failure mode" in roadmap. Always verify with `cmp` byte-for-byte after unpack.
- **Compare against the right baseline.** xz-9e is the universal target (best public general-purpose codec at byte level). Format-specific baselines (brunsli for JPEG, cjxl for PNG, ZXL for PE) are the targets *per stream type*.
- **Recipe overhead counts.** A milestone that wins 22% on JPEG payload but ships a 200 B recipe per JPEG can net-regress on small images. Always include recipe size in the reported ratio.

---

## Documentation discipline

- **`roadmap.md` is the index.** Update after every shipped milestone. Move shipped items to "shipped", failed experiments to "Tried and reverted" with **concrete numbers and the structural reason**.
- **Document why things failed.** Future sessions need the reason to avoid retrying.
- **Don't create new docs for one-off thoughts.** `roadmap.md`, `README.md`, `workflow.md` (this file), and that's it.
- **Don't write `Current Status` sections that go stale.** Date-stamp anything with numbers; update the stamp when numbers change.

---

## Communication style with the user

- **Terse, direct.**
- **One sentence before each tool call** stating what you're about to do.
- **Honest about failures.** When an experiment regresses, say so plainly with numbers.
- **End-of-turn summary**: 1–2 sentences. What changed, what's next.
- **Never propose subagents** unless asked.
- **Never offer Co-Authored-By or AI branding** in any output going to the repo.

---

## Approach to new ideas

1. **Estimate the gain range honestly.** Use prior session data and "Tried and reverted".
2. **State the failure modes** before implementing.
3. **Bound the effort.** Multi-week items must be flagged as such.
4. **Prefer reversible changes.** Format changes ripple through encoder + decoder + recipe + tests.
5. **When in doubt, instrument first.** Measure before implementing. Remove instrumentation before commit.

---

## What NOT to do

- Don't add error handling, logging, validation, or comments beyond what the change requires.
- Don't refactor "while you're in there."
- Don't introduce abstractions for hypothetical future use.
- Don't add Co-Authored-By, AI tool references, or any AI branding.
- Don't push to GitHub without explicit ask.
- Don't run `git reset --hard`, `git push --force`, or other destructive ops without confirmation.
- Don't retry experiments listed in "Tried and reverted" without explaining what's structurally different now.
- Don't trust your memory of ratios — re-bench.

---

## Quick-reference: project specifics

- **Language:** C, gcc -O3 -march=native.
- **Build:** `make`. Backend deps: system `zstd`; M2+ adds `python` (for `python -m zipfile`); M3+ adds optional `brunsli` / `cjxl` / `reflate`.
- **File extension:** `.zxle`.
- **Magic bytes:** `Z X L E` (4 bytes) + version byte + flags byte.
- **Sister project:** [ZXL](../Zxl) — PE-specific codec, used as a backend in M3 when a PE stream is detected.

---

## At session start, run this

```bash
git status
git log --oneline -5
bash tests/bench.sh 2>&1 | head -20
```

Confirm bench numbers match what `roadmap.md` claims. If they diverge, docs are stale — fix that before doing new work.