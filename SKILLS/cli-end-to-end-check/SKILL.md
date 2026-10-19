---
name: cli-end-to-end-check
description: Verify a change end to end with the mso command-line tool on a generated folder of real audio files (import, search, edit, undo, analyze, export) without touching the user's workspace.
---

# End-to-end check with the `mso` CLI

Unit tests prove pieces; this proves the whole pipeline on real files. Always pass `--workspace` so the
user's real library registry is never touched.

## 1. Generate audio (ffmpeg on PATH)

```sh
S=$(mktemp -d); F=$S/samples; mkdir -p "$F/Drums" "$F/Loops" "$F/FX"
ffmpeg -loglevel error -f lavfi -i "sine=frequency=60:duration=0.4" "$F/Drums/Kick 01.wav"
ffmpeg -loglevel error -f lavfi -i "sine=frequency=220:duration=3" -c:a flac "$F/Loops/Pad_Am.flac"
ffmpeg -loglevel error -f lavfi -i "anoisesrc=d=2" -c:a libmp3lame "$F/FX/Riser.mp3"
ffmpeg -loglevel error -f lavfi -i "sine=frequency=880:duration=1" -c:a libopus "$F/FX/Bell.opus"
# a 128 BPM kick loop: one 50 ms click per beat, repeated
ffmpeg -loglevel error -f lavfi -i "sine=frequency=55:duration=0.05" -af apad=pad_dur=0.41875 -t 0.46875 /tmp/beat.wav
ffmpeg -loglevel error -stream_loop 15 -i /tmp/beat.wav "$F/Loops/Kick_Loop_128bpm.wav"
```

## 2. Drive the pipeline

```sh
M="build/debug/mso --workspace $S/ws"; L=$S/lib
$M create $L --name Test && $M add-root $L "$F" && $M import $L
$M search -l $L "kick bpm:120..130" --count
$M edit $L 1 --rating 4 --tag Drums/Kick && $M undo $L && $M redo $L && $M history $L
$M analyze $L --profile Full --timeout 600   # explicit analysis through mso-worker
$M show $L 1                          # observations with provenance
$M export $L $S/out.csv && $M check $L
$M play "$F/Drums/Kick 01.wav" --backend null   # decode and play without an audio device
```

The CLI is explicit: it analyzes only on `analyze`. The desktop app runs the same pipeline
automatically (default library, rescans, the `Full` profile after each import). `--worker <path>`
selects a specific `mso-worker` (default: the one next to `mso`). `analyze` cannot hang: it stops on
a paused queue, a missing worker, `--timeout`, 15 minutes without a finished job, or Ctrl+C.

Check exit codes, not only output: 0 ok, 1 error, 2 usage, 3 partial result, 130 interrupted. While the
desktop app has a library open, the CLI opens it read-only (`library.lock`) and commands that write
fail; use a scratch library.

For scale, `mso bench <new-lib> --rows 100000 --repetitions 30` measures catalog inserts and search
latency on synthetic metadata (no audio), through SQLite and through the in-memory index; use a release
build and record results in `IMPLEMENTATION_STATUS.md`.

## 3. Things to look for

- Import counts match the files (non-audio such as `.txt` is skipped).
- Renaming a file and re-running `import` reports `1 renamed` and keeps tags/ratings.
- Deleting a file reports `1 marked missing`; the row shows `missing` and stays searchable.
- `check` prints `integrity: ok; foreign keys: 0 violation(s); search index: ok`.
- `analyze` finds ~128 BPM on the loop and **no** tempo for a one-shot.
