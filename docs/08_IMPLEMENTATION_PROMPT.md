# 08 — LLM implementation prompt

Copy the following prompt into the implementation agent, with this repository available. The complete single-file handoff includes the supporting documents and reference schema.

---

You are implementing **Music Sample Organizer**, a fully offline Windows desktop application for professional producers. Use the engineering specification in this repository as the working contract. Build the application; do not stop at a plan or replace implementation with mock screens. Work with Git throughout.

Read `AGENTS.md`, `README.md`, `docs/01_REQUIREMENTS.md` through `docs/09_DECISIONS_AND_SOURCES.md`, and `contracts/schema.sql`. If only `MUSIC_SAMPLE_ORGANIZER_SPEC.md` is provided, it contains the same material; reconstruct the specified repository documents before coding. The numbered documents and schema are authoritative; regenerate the combined handoff after specification changes.

The fixed stack is C++23 core, Qt 6 Widgets UI and one SQLite database per portable library. Windows 11 x64 is the supported target, including N editions. The installed app must work with all networking unavailable. No accounts, cloud inference, telemetry, runtime downloads, remote APIs, or hidden online fallback. Use local model packs and a CPU baseline. No GPU requirement.

First inspect the repository and environment. Preserve unrelated changes. Create a feature branch from the current branch without rewriting its history. If application code already exists, map it to the roadmap and continue from the earliest unmet dependency; do not recreate working systems blindly. If it is this specification-only repository, start P0 and continue through P1 to a usable, tested release. Do not attempt P2–P7 wholesale before the P1 gate. The later phases remain part of the product and should be recorded as pending.

Create and maintain `IMPLEMENTATION_STATUS.md` with current phase, completed task IDs, actual test/build commands and results, measured budgets, unresolved failures and the exact next action. Work autonomously through routine implementation choices using the defaults in the specification. Do not reopen settled choices. Do not fabricate test results, compatibility, performance or model quality. If the environment cannot compile Windows UI/audio, implement and verify portable components where possible and explicitly state which Windows gates remain unexecuted.

Deliver P0 first: a pinned and documented Windows build, Qt shell, domain types, SQLite migration/backup/FTS harness, worker/audio spike, local-files watcher spike, and native Windows vector-index feasibility evidence. Model selection must not delay the early metadata/playback path. Use documented toolchain versions and feature probes; do not use floating dependencies or hardcode another person's installed paths. Runtime offline behavior is separate from deliberate developer dependency bootstrap.

Deliver P1 next: create/open library; reference-folder import with bounded metadata probes, progress and cancellation; a paged sortable table; filename/tag/filter search; play/pause/stop/seek with lazy selected waveform; basic volume; drag existing sample files to Ableton/another DAW; notes, ratings, favorites, simple tags and manual collections; persistent undo/redo; missing-source display; verified backup and crash recovery. It must be useful without a model installed. Commit and package this vertical slice before implementing advanced AI.

Protect these invariants:

- Import is metadata work. Whole-file hashing, full-library waveforms, BPM/key, classification, embeddings and duplicate analysis require explicit actions. Integrity hashing during an authorized managed copy and selected-sample preview are the documented exceptions; they do not authorize other background analysis.
- Source files remain unchanged. Never auto-delete originals, silently move them, rewrite embedded metadata or collapse duplicates. Managed import copies with recovery journaling.
- Every collection-sized structure is disk-backed. Bound all pages, queues, caches, threads, result spools and workers. No full-library array of UI objects or decoded audio. Memory mapping is not free physical memory.
- No allocation, locks, I/O, SQLite, decoding, inference, logging or Qt calls on the audio callback. Preview is isolated from throttled analysis.
- One serialized writer per library; short transactions; committed history and outbox are atomic with metadata. Filesystem and database publication use explicit recoverable protocols. No invented multi-library atomic transaction.
- Missing media remains searchable. Whole-catalog disconnect uses a read-only local metadata mirror with an honest watermark. Do not create offline audio proxies.
- User overrides and accepted annotations survive reanalysis; unknown, stale, unavailable and failed states are distinct.
- All worker results are fenced by lease, content revision, profile and cancellation epoch before commit. A late completion cannot resurrect cancelled work.
- ANN/model capabilities require numerical, quality, recall, memory and Windows validation. Do not substitute random embeddings or a cloud API. Placeholder capability must be visibly unavailable and cannot satisfy a gate.

Use the reference schema as a destination, adding only the migrations needed for each milestone. Preserve released migration checksums. Add tests for actual risks: transaction boundaries, history conflicts, copy crashes, watcher overflow, source disconnects, malformed audio, queue cancellation, decoder seek, hard-filter correctness and bounded resource usage. Do not write shallow tests that merely mirror implementation. Use the prescribed benchmark datasets/machine definitions and distinguish synthetic metadata from real audio analysis.

Before each commit, inspect the diff, run relevant checks, stage explicit files and write a precise message. Commit every coherent passing increment. Do not commit private samples, database files, model weights, credentials or build output. Do not force push, reset unrelated work, choose a public project license or publish a remote repository without explicit instruction. Preserve local history.

When P1 is complete, provide the runnable package/build instructions, completed requirement/task IDs, actual validation results, measured performance, remaining limitations and next phase. If a required gate fails, fix it or record the exact blocker; do not call the milestone complete. Keep working until the scoped milestone is complete or a concrete external blocker prevents further progress.

---

For subsequent implementation sessions, replace the P0/P1 execution scope with the earliest unfinished phase listed in `IMPLEMENTATION_STATUS.md`; retain all invariants and acceptance gates. Do not treat a context reset as authorization to change architecture or discard history.
