# Engineering agent rules

1. Read README, the specification, and the current roadmap phase before coding. Treat confirmed requirements as binding. Chosen defaults are implementation decisions, not claims that the user explicitly chose them.
2. Implement one usable vertical slice at a time. Start with phase P0, then P1. Later milestones remain requirements; do not implement every subsystem upfront.
3. Keep the shipping application completely offline. No account, telemetry, updater, remote model download, cloud inference, network decoder protocol, or hidden online fallback. Developer dependency acquisition is separate from runtime behavior.
4. Import performs bounded filesystem/container metadata work only. Full hashing, full waveform extraction, BPM, key, embeddings, classification, and duplicate analysis require an explicit action. Interactive preview may decode the selected file and show its waveform under the separate preview policy.
5. Preserve source files. Never auto-delete, auto-move, rewrite embedded metadata, or silently merge records. Availability is not deletion.
6. Keep audio callback work bounded: no allocation, locks, filesystem, SQLite, logging, Qt calls, decoding, or inference on that callback.
7. Keep datasets disk-backed and UI models paged. Every queue, cache, result set, and worker pool must have an explicit bound. A memory-mapped file still consumes physical memory when paged in.
8. Use one writer per library. Persist metadata, history, revision, and outbox atomically. Never claim an atomic transaction across libraries or across SQLite and the filesystem.
9. Use the reference contracts and schema. Add numbered forward migrations only when their feature ships. Do not edit a migration already released. Derived data remains rebuildable and versioned.
10. Add meaningful tests for correctness and failure behavior. Never relax tests, budgets, recall thresholds, or acceptance criteria merely to make a phase pass. Document an evidence-backed specification amendment before changing a contract.
11. Work with Git: inspect status first; preserve unrelated changes; use a feature branch; commit each coherent, passing increment. Never commit credentials, recordings, personal libraries, build outputs, downloaded model weights, or benchmark corpora. Never run destructive reset/clean or force push without explicit authorization.
12. Update `IMPLEMENTATION_STATUS.md` with completed work, checks, unresolved failures, and the exact next action. Record actual command output summaries, not invented tests or performance.
13. Use pragmatic defaults for routine choices. Do not stop to ask questions already answered here. If blocked by an external artifact, keep the interface, mark the capability unavailable, and finish independent authorized work. Never pretend an unvalidated model or stub is production-ready.
14. No sub-agent delegation is required by this document. No application implementation is claimed by this specification repository.
