---
name: doc-sync
description: Audit in-repo documentation (README.md, CLAUDE.md, other *.md, and code comments) against the current source tree and report or fix drift. Use when docs may have fallen behind code — renamed symbols, removed features, changed commands/flags, moved files, stale examples.
---

# Doc Sync

Verify that documentation reflects current code. Report drift first, fix second — never rewrite docs speculatively.

## Scope

In scope:

- `README.md`, `CLAUDE.md`, any other `*.md` tracked in the repo
- Non-obvious comments in source files (`*.c`, `*.h`, `Makefile`, shell scripts)
- Build/run instructions, command examples, file paths, flag names, version labels

Out of scope:

- Generated files, vendored code, anything under `build/` or ignored paths
- Style-only rewrites, tone changes, "improvements" unrelated to drift

## Procedure

1. **Enumerate docs.** List every tracked `*.md` and collect inline comments that assert behavior (not just what-the-code-does narration).
2. **Extract claims.** For each doc, pull out concrete, verifiable claims: commands, file paths, symbol names, flag names, version numbers, directory layout, example output.
3. **Verify each claim against source.** Use Read/Grep/Glob to confirm:
   - Commands actually exist (e.g., `make fmt-check` is a real target)
   - Referenced files/dirs exist at the stated path
   - Function/struct/macro names still exist and have the described shape
   - Flags/options are still accepted by the code or tooling
   - Version labels match the roadmap (e.g., v1/v2/v3 directories)
4. **Classify each finding.**
   - **Stale** — claim was true once, now wrong (rename, removal, path change).
   - **Wrong** — claim was never true, or contradicts current code.
   - **Missing** — code has a user-visible feature/flag/target with no doc mention.
   - **Redundant** — comment restates what well-named code already says; candidate for deletion.
5. **Report before fixing.** Produce the findings table (below). Wait for confirmation before editing, unless the user asked to fix in one shot.
6. **Fix conservatively.** Only change what is drifted. Preserve surrounding prose. Do not reformat, reflow, or restructure untouched sections.

## Report format

Short summary, then a table:

| File:line     | Kind      | Claim                              | Reality                        | Proposed fix   |
| ------------- | --------- | ---------------------------------- | ------------------------------ | -------------- |
| README.md:42  | Stale     | `make run` launches server         | target renamed to `make serve` | update command |
| src/net.c:118 | Redundant | `// increment counter` above `i++` | self-evident                   | delete comment |

End with a one-line count: `N stale, N wrong, N missing, N redundant`.

## Rules

- Never invent commands, flags, or paths. If a claim can't be verified either way, mark it **Unverified** and say what evidence is missing.
- Do not add new documentation sections unless the user asks — drift fixes only.
- Do not delete a comment just because it's long. Delete only if it's redundant with the code or contradicted by it.
- Respect existing doc voice and structure. A fix is a minimal edit, not a rewrite.
- If the same claim appears in multiple docs, fix all occurrences in one pass and list them together in the report.
