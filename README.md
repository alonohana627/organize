# Music Sample Organizer

An offline desktop organizer for producers' sample libraries: catalog folders of audio in place, search
them instantly with a typed query language, preview with a real-time audio engine, tag and rate them with
durable undo, and run explicit local analysis (BPM, key, waveforms, duplicates). No accounts, no cloud,
no telemetry. C++23 (clang + libc++), Qt 6, SQLite. Licensed under the GNU GPL v3.

- **Build:** [`docs/BUILDING.md`](docs/BUILDING.md) (Windows via MSYS2 CLANG64 is verified; Linux is prepared)
- **CI:** [`.github/workflows/ci.yml`](.github/workflows/ci.yml) builds with warnings as errors and runs every
  test suite on **Windows** (MSYS2 CLANG64: clang + libc++, Qt 6, FFmpeg) and **Linux** (Ubuntu 24.04,
  LLVM 20 clang + libc++, Qt 6, FFmpeg) on every push and pull request. It also verifies the vendored
  dependencies (`deps/update_deps.py --check`).
- **What is implemented and verified:** [`IMPLEMENTATION_STATUS.md`](IMPLEMENTATION_STATUS.md)
- **Guides for coding agents:** [`SKILLS/`](SKILLS/README.md) · repository rules in [`AGENTS.md`](AGENTS.md)
- **Dependencies:** [`deps/README.md`](deps/README.md)
- **Specification:** `docs/01`–`09`, `contracts/schema.sql` (the original engineering handoff; see below)

## What it does

- **Libraries** are portable folders (`library.json`, `catalog.sqlite`, derived data, verified backups).
  Up to eight can be open and searched together. Source folders are referenced in place; their
  absolute paths are machine-local bindings, so a library survives drive-letter changes.
- **Import** enumerates and probes metadata only (bounded: ≤4 MiB and ≤2 s per file) in parallel, commits
  in batches of ≤500, and is cancellable and resumable. Rescans recognise renames on the same volume by
  file ID (keeping tags, ratings and history), mark vanished files *missing* (never deleting records),
  and flag files whose content changed for review. Unchanged files are not re-probed.
- **Formats:** WAV/RF64/W64, AIFF/AIFC, FLAC, MP3, Ogg Vorbis natively; AAC/M4A, Opus, ALAC, WMA, WavPack,
  CAF and more through FFmpeg (local files only, no network protocols).
- **Search** (`kick bpm:120..130 tag:Drums rating:>=3`) compiles to parameterized SQL plus escaped FTS5
  terms; results page through keyset cursors (no OFFSET scans), sort on the whole result, and merge
  across libraries. Relevance ranking fuses libraries with reciprocal rank fusion.
- **Organize:** ratings, favorites, notes, display names, hierarchical tags (rename, move, merge with
  aliases), categories, manual and smart collections. Every edit is a durable history action with
  compare-and-set undo/redo that survives restarts and refuses to clobber later edits.
- **Preview:** a streaming playback engine (WASAPI on Windows, PulseAudio/ALSA on Linux) with a lock-free
  real-time callback, seek, volume, loops and varispeed.
- **Automatic in the desktop app:** add sample folders and they are scanned and analyzed (BPM, key,
  waveforms, loudness, duplicate hashes) without further steps. A default library ("My Samples") is
  created in the workspace on first start, folders are rescanned at start-up and when a drive
  reconnects, and only new or changed samples are analyzed. The automatic profile is the workspace
  preference `analysis.automatic_profile` (default `Full`; empty disables it). The CLI stays explicit.
- **Analysis** runs as durable jobs in a separate `mso-worker` process confined by a job object
  (memory and CPU caps). Results are fenced by lease, cancel epoch and content revision; user
  overrides always win over analysis.
- **Export** metadata to CSV (RFC 4180, spreadsheet-safe by default) or JSON, atomically.

## Using the app

1. Build (see `docs/BUILDING.md`), then start `build/release/MusicSampleOrganizer.exe` with
   `C:\msys64\clang64\bin` on `PATH`.
2. Click **Add sample folder…** (or File → Add Folder) and pick a folder of samples. Repeat for more
   folders. A default library ("My Samples") is created for you in
   `%LOCALAPPDATA%\MusicSampleOrganizer\Libraries`; your audio files never move.
3. The Jobs dock shows the scan and the automatic analysis. Results appear while they run.
4. Search with the query language below, select a sample and press **Space** to preview, drag rows
   into your DAW, and tag, rate or categorize in the Inspector. Ctrl+Z undoes any edit, even after a
   restart.

More libraries: File → New Library (stored in the workspace too) or File → Open Library for a
library folder made elsewhere.

## Query language

| Example | Meaning |
| --- | --- |
| `kick snare` | both words (whitespace is AND) |
| `"metallic impact"` | phrase |
| `kic*` | prefix (two or more characters) |
| `(tag:Drums OR tag:Percussion) AND NOT tag:Acoustic` | boolean logic, OR binds weaker than AND |
| `tag:Drums/Kick`, `tag:Kick` | tag subtree; a single name matches at any depth |
| `category:Bass`, `folder:"Packs/Goa"`, `collection:Favorites` | navigation filters |
| `bpm:140..155`, `bpm:>=120`, `key:F#minor`, `key:unknown` | musical facts |
| `duration:<2s`, `size:>1MB`, `rate:44.1k`, `channels:2`, `bits:24` | technical facts |
| `format:flac`, `codec:pcm`, `rating:>=4`, `favorite:true`, `available:true` | more filters |
| `analyzed:tempo` (`key`, `waveform`, `hash`, `loudness`, `any`) | analysis coverage |
| `name:kick`, `path:loops`, `notes:dusty` | text restricted to one field |

Errors point at the exact character (`PARSE_ERROR at 4: 'fast' is not a number`) and nothing runs.

## Command-line tool

```text
mso [--workspace DIR] [--json] <command>
  create <lib> [--name N]            add-root <lib> <folder>          import <lib> [--root ID]
  info <lib>                         search -l <lib> [-l <lib2>] "<query>" [--sort F] [--desc] [--count]
  show <lib> <id>                    edit <lib> <ids...> [--rating N] [--tag P] [--untag P] [--category P]
                                          [--favorite yes|no] [--notes T] [--bpm X|auto|unknown] [--key K]
  undo|redo|history <lib>            tag <lib> <id> --rename N | --move-under ID | --merge-into ID
  collection <lib> --create N [--smart Q] | --id ID --rename N | --delete
  tags|collections <lib>             analyze <lib> [query] --profile Waveforms|Musical|Duplicates|Full
  export <lib> <file> [--format csv|json]    backup|check <lib>    probe <file>    play <file>
  capabilities                       bench <new-lib> [--rows N]   (synthetic catalog/search benchmark)
```

## Repository layout

```text
include/mso/<module>/   all headers          src/<module>/   implementation (.cpp only)
  domain     identity, text, hashing, errors (std::expected)
  catalog    SQLite catalog, migrations, history/undo, scans, analysis queue
  search     query language, SQL compiler, paged/federated search
  media      bounded probes and streaming decoders
  files      file identity and the importer
  audio      real-time preview engine
  analysis   DSP (tempo, key, waveform, loudness, hashes) and the worker protocol/process
  app        workspace, application services, analysis supervisor, export
  cli        the mso tool        ui   the Qt desktop app        worker   mso-worker
tests/      doctest suites per area          deps/   vendored libraries (+ patches/)
SKILLS/     task guides for coding agents    docs/   specification and build docs
```

## The specification

This repository started as a specification-only engineering handoff. The numbered documents in `docs/`
and `contracts/schema.sql` remain the authoritative requirements; `MUSIC_SAMPLE_ORGANIZER_SPEC.md` is the
generated single-file version (`python tools/build_handoff.py`), and `python tools/validate_spec.py`
checks the documents and reference schema. Where the implementation deliberately differs from the
specification (for example, Linux is now a target too), `IMPLEMENTATION_STATUS.md` says so.
