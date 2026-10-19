---
name: add-analysis-stage
description: Add an explicit analysis result (e.g. LUFS loudness, onset density) end to end - DSP in the analysis module, worker protocol, supervisor, fenced catalog publication, search flag and tests.
---

# Add an analysis stage

Analysis runs only inside an analysis run (queued by the user, or automatically by the desktop
app after an import), always in the `mso-worker` child process, and results are published only after
fencing (lease, cancel epoch, content revision).

## Steps

1. **DSP** (`src/analysis/`): add a streaming accumulator class fed by the shared decode pass in
   `decoded_stream_analysis.cpp` (bounded memory, deterministic, honours the stop token). Use pocketfft
   for spectra (`include/mso/analysis/spectrum.hpp`). Add the stage to `Stage`, `to_string`/`parse_stage`, a version string in
   `AlgorithmVersions` (bump it, e.g. `tempo-flux-autocorr-2` → `-3`, whenever results change, so old
   results are re-analyzed), and a field in `AnalysisReport`. Report progress and work units only
   through the `ProgressPhase` you are given (`ProgressReporter` throttles to 10 Hz; the worker
   forwards every fraction it receives, so a second throttle must not be added). A request lists
   each stage at most once (`validate_request`, mirrored by the protocol's `validate_task`), and
   every new function that returns `mso::Result` is declared `[[nodiscard]]`.
2. **Protocol** (`include/mso/worker/worker_protocol.hpp`, `src/worker/worker_protocol.cpp`): serialise
   the new report field in `TaskResult` (strict decoding: wrong types are `PARSE_ERROR`) and range-check
   it in `validate_result` (a worker sending an out-of-range value is a protocol violation). Bump the
   protocol minor version (1.1 today). A new resource bound belongs in `WorkerLimits`; the supervisor
   refuses a handshake whose limits differ from its own. Long stages must keep advancing the progress counter: the
   supervisor treats a task whose heartbeat `task_progress` does not move for `task_stall_deadline`
   (60 s) as hung, cancels it, then terminates the worker (a retryable `WORKER_CRASH`).
3. **Supervisor** (`src/app/analysis_supervisor.cpp`): copy the result into
   `catalog::Library::AnalysisFacts`.
4. **Catalog** (`src/catalog/library_analysis.cpp`): store it as a provenanced observation in
   `write_analysis_facts` (source `analysis`, algorithm version, confidence), give the stage a bit in
   `include/mso/domain/analysis_flags.hpp` (`kAnalyzed*` and `analysis_flag_for_stage`; bits are
   persisted, never renumber) so "already analyzed" skipping works, and update effective values only through
   `recompute_effective_musical_facts` (user overrides always win).
5. **Profiles**: add it to `Application::profile_stages` where it belongs. The desktop app queues the
   profile named by the workspace preference `analysis.automatic_profile` (default `Full`) after every
   import, so a stage in `Full` runs automatically for new and changed samples.
6. **Search**: extend `analyzed:` in `src/search/query_parser.cpp` with the new flag (the row
   evaluator in `src/search/row_matcher.cpp` reads the same bits).
7. **Tests**: synthetic signals with known answers in `tests/analysis_tests.cpp` (fixtures from
   `tests/test_support.hpp`: `test::write_wav`, `test::sine_samples`); "unknown" cases must return no
   value rather than a guess; a catalog test that a re-analysis never overrides a user value; protocol
   round trips and worker process behaviour in `tests/worker_tests.cpp` (suite `worker`; the worker's
   `--self-test-fault` modes exist only for these tests). Run both suites under the `sanitize` preset
   too (AddressSanitizer + UBSan; CTest sets the symbolizer and `PATH` itself): the worker's 2 GiB
   job-object memory cap leaves ample room for ASan on Windows (peak commit measured at about 86 MiB).

Be honest in docs: synthetic tests prove mechanics, not real-world accuracy.
