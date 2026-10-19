# Specification validation record

> This file records checks of the *specification documents and reference schema*. Verification of
> the implemented application (builds, test suites, benchmarks) is recorded in
> [`IMPLEMENTATION_STATUS.md`](IMPLEMENTATION_STATUS.md). The tools now read files as UTF-8, so
> `python tools/validate_spec.py` also passes on Windows code pages (re-run 2026-10-19: OK).

Validated 2026-10-19 in the preparation environment with Python's SQLite 3.53.1. This validates documentation and the reference schema, not the Windows application.

- `python3 tools/validate_spec.py`: **26 checks passed**.
- All 28 confirmed requirement IDs occur in the roadmap traceability table.
- All nine source chapters are present; JSON examples parse; fenced blocks are balanced; source-reference IDs resolve.
- The reference SQL creates 36 domain tables plus the FTS5 virtual table and its internal tables in a fresh database.
- Schema checks cover STRICT typing, foreign keys, override states, rating bounds, unique live paths, taxonomy siblings, collections, valid JSON, job states, per-run and cross-run task uniqueness, receipt identity, active index generation uniqueness and ordered duplicate pairs.
- FTS insert/update/delete, rebuild and external-content integrity checks passed.
- Transaction rollback reverted both ordinary metadata and its FTS projection.
- SQLite backup copied catalog rows and searchable FTS data into a fresh database; integrity check returned `ok`.
- Manual consistency review covered the import/preview/analysis boundary, offline-catalog mirror lag, snapshot/writer coordination, metadata history, cancellation fencing, and phased requirement coverage.

The source schema intentionally relies on application validation for recursive hierarchy cycles, cross-root folder references, field provenance, content revisions, command-digest comparisons, history semantics and publication protocols. The checks do not claim those future services are implemented.

Not executed in this specification-phase validation (before any implementation existed): C++/Qt build, Windows/WASAPI/DAW integration, real-media decoding, million-row performance, model export/inference, ANN recall, audio-quality evaluation, application crash recovery, or production migration execution. Their acceptance procedures and gates are specified in chapter 06. What the implementation has since built and verified, and what is still unverified, is recorded in [`IMPLEMENTATION_STATUS.md`](IMPLEMENTATION_STATUS.md).

To repeat document/schema checks, run `python3 tools/validate_spec.py`. To regenerate the combined handoff after source edits, run `python3 tools/build_handoff.py`. The build script does not require network access or third-party Python packages.
