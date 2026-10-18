# Music Sample Organizer — engineering handoff

Version 1.0 · 2026-10-19 · Windows x64 · C++23 / Qt 6 / SQLite

This repository specifies an application to be implemented. It does not contain the application or claim measured application performance.

## Start here

Feed `MUSIC_SAMPLE_ORGANIZER_SPEC.md` to the implementation LLM. It contains the complete specification, roadmap, agent instructions, and reference schema. For repository work, read `AGENTS.md`, then `docs/08_IMPLEMENTATION_PROMPT.md` and the current phase in `docs/07_ROADMAP.md`.

## Contents

| File | Purpose |
| --- | --- |
| `docs/01_REQUIREMENTS.md` | Confirmed decisions, chosen defaults, workflows, requirements |
| `docs/02_ARCHITECTURE.md` | Components, concurrency, portability, import, playback |
| `docs/03_DATA_AND_SEARCH.md` | Data invariants, SQL, query language, federation, history |
| `docs/04_CONTRACTS.md` | APIs, messages, operations, errors, example payloads |
| `docs/05_AUDIO_INTELLIGENCE.md` | Analysis, model packs, embeddings, similarity, duplicates |
| `docs/06_QUALITY_AND_OPERATIONS.md` | Budgets, benchmarks, recovery, migrations, release gates |
| `docs/07_ROADMAP.md` | Phased backlog and acceptance gates |
| `docs/08_IMPLEMENTATION_PROMPT.md` | Copy-ready LLM execution instructions |
| `docs/09_DECISIONS_AND_SOURCES.md` | Architecture decisions, risk register, primary references |
| `contracts/schema.sql` | Executable reference schema; implemented features are migrated incrementally |
| `tools/validate_spec.py` | Document contract and SQL integrity smoke checks |
| `tools/build_handoff.py` | Rebuilds the single-file specification from its sources |
| `VALIDATION.md` | Checks actually performed on this specification |

The numbered documents and `contracts/schema.sql` are authoritative. The single-file handoff is generated; do not edit it directly. Requirement IDs are stable. Performance numbers are acceptance targets, not results. A deferred release phase does not cancel a requirement.

## Git

The repository has a local `main` branch and incremental commits. No remote is configured. The delivered archive includes `.git`; the accompanying Git bundle preserves the same history. After extracting, run `git log --oneline` and `git status`. To clone a bundle, run `git clone music-sample-organizer-spec.bundle music-sample-organizer`.

Use feature branches, small verified commits, and annotated release tags. Do not push, rewrite published history, or select a source-code license without a separate instruction.
