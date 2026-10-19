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
   `AlgorithmVersions`, and a field in `AnalysisReport`.
2. **Protocol** (`src/analysis/worker_protocol.cpp`): serialise the new report field in `TaskResult`
   (strict decoding: wrong types are `PARSE_ERROR`). Bump the protocol minor version.
3. **Supervisor** (`src/app/analysis_supervisor.cpp`): copy the result into
   `catalog::Library::AnalysisFacts`.
4. **Catalog** (`src/catalog/library_analysis.cpp`): store it as a provenanced observation in
   `write_analysis_facts` (source `analysis`, algorithm version, confidence), give the stage a bit in
   `stage_flag` so "already analyzed" skipping works, and update effective values only through
   `recompute_effective_musical_facts` (user overrides always win).
5. **Profiles**: add it to `Application::profile_stages` where it belongs. The desktop app queues the
   profile named by the workspace preference `analysis.automatic_profile` (default `Full`) after every
   import, so a stage in `Full` runs automatically for new and changed samples.
6. **Search**: extend `analyzed:` in `src/search/query_parser.cpp` with the new flag.
7. **Tests**: synthetic signals with known answers in `tests/analysis_tests.cpp`; "unknown" cases must
   return no value rather than a guess; a catalog test that a re-analysis never overrides a user value.

Be honest in docs: synthetic tests prove mechanics, not real-world accuracy.
