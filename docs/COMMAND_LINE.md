# The `mso` command-line tool

`mso` works on the same libraries as the desktop app. Unlike the app, it does nothing automatically:
it scans on `import` and analyzes only on `analyze`. It uses the same search syntax as the app (see
the [README](../README.md#searching)).

```text
mso [--workspace DIR] [--worker PATH] [--json] <command>
mso --version | --help | <command> --help

  create <lib> [--name N]
  add-root <lib> <folder> [--name N]
  import <lib> [--root ID]
  info|tags|collections|history|undo|redo|backup|check <lib>
  search -l <lib> [-l <lib2>]... ["<query>"] [--sort name|added|duration|size|rate|bpm|key|rating|format|relevance]
         [--desc] [--limit N] [--count]
  show <lib> <id>
  edit <lib> <ids...> [--rating 0-5] [--favorite yes|no] [--notes T] [--name N] [--tag P]... [--untag P]...
         [--category P | --clear-category] [--bpm X|auto|unknown] [--key K|auto|unknown]
         [--add-to ID | --remove-from ID] [--remove | --restore] [--operation UUID]
  tag <lib> <id> --rename N | --move-under ID | --move-to-top | --merge-into ID
  collection <lib> --create N [--smart Q] | --id ID --rename N | --id ID --delete
  analyze <lib> [query] [--profile Waveforms|Musical|Duplicates|Full] [--force] [--timeout S]
  export <lib> <file> [--format csv|json] [--query Q] [--raw] [--absolute-paths]
  probe <file>
  play <file> [--volume DB] [--seconds S] [--backend system|null]
  capabilities
  bench <new-lib> [--rows N] [--repetitions N] [--names realistic|short]
```

Defaults: `search` sorts by name and prints at most 50 rows (`--limit`, up to 10,000,000); `analyze`
uses the Musical profile and waits without a time limit (`--timeout 0`); `play` plays the whole
file (at most an hour) at -6 dB on the default output device; `bench` inserts 100,000 rows with
realistic file names.

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Error |
| 2 | Usage error |
| 3 | Partial result, for example an offline library, or jobs that failed, were blocked or were paused |
| 130 | Interrupted (Ctrl+C) |

Output and waiting:

- `--json` is a global option, so it goes before the command. `mso --json search ...` prints one
  JSON array of rows, one row per line as they are found; with `--count` it prints one JSON object
  (`{"rows": [...], "total": n, "complete": true}`).
- A library that cannot be searched (for example its drive is offline) is reported once on stderr,
  and the exit code is 3.
- Progress lines are printed only to a terminal, never with `--json`.
- `analyze` waits for its runs, but it cannot hang. It stops on any of these:
  - a paused queue, including a missing worker or a worker that keeps failing to start (a single
    failed start is retried);
  - `--timeout`;
  - 35 minutes without a finished job (longer than the 30-minute limit of one job);
  - Ctrl+C, which also cancels its runs.

  Jobs of a run that was paused elsewhere (for example in the app) are reported as paused, with exit
  code 3.
- `--worker` points at a specific `mso-worker`.
- Ctrl+C stops a command cleanly and keeps the work already committed. Press it a second time to end
  `mso` at once.

While the desktop app has a library open, `mso` opens that library read-only, so commands that change
it fail, `backup` included. Close the app or use another library. The other way round, the app opens a
library read-only while `mso` is changing it, and makes it writable again by itself once `mso` is done.
