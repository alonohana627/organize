# The `mso` command-line tool

`mso` works on the same libraries as the desktop app. Unlike the app, it does nothing automatically:
it scans on `import` and analyzes only on `analyze`. It uses the same search syntax as the app (see
the [README](../README.md#searching)).

```text
mso [--workspace DIR] [--worker PATH] [--json] <command>       mso --version
  create <lib> [--name N]            add-root <lib> <folder>          import <lib> [--root ID]
  info <lib>                         search -l <lib> [-l <lib2>] "<query>" [--sort F] [--desc] [--count]
  show <lib> <id>                    edit <lib> <ids...> [--rating N] [--tag P] [--untag P] [--category P]
                                          [--favorite yes|no] [--notes T] [--bpm X|auto|unknown] [--key K]
  undo|redo|history <lib>            tag <lib> <id> --rename N | --move-under ID | --merge-into ID
  collection <lib> --create N [--smart Q] | --id ID --rename N | --delete
  tags|collections <lib>             analyze <lib> [query] --profile Waveforms|Musical|Duplicates|Full [--timeout S]
  export <lib> <file> [--format csv|json]    backup|check <lib>    probe <file>    play <file> [--backend null]
  capabilities                       bench <new-lib> [--rows N] [--repetitions N]   (synthetic benchmark: SQLite and in-memory index)
```

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Error |
| 2 | Usage error |
| 3 | Partial result, for example an offline library or failed jobs |
| 130 | Interrupted (Ctrl+C) |

Output and waiting:

- `--json` is a global option, so it goes before the command. `mso --json search ... --count` prints
  one JSON document.
- Progress lines are printed only to a terminal, never with `--json`.
- `analyze` waits for its runs, but it cannot hang. It stops on any of these:
  - a paused queue;
  - a missing worker;
  - `--timeout`;
  - 15 minutes without a finished job;
  - Ctrl+C, which also cancels its runs.
- `--worker` points at a specific `mso-worker`.

While the desktop app has a library open, `mso` opens that library read-only, so commands that change
it fail. Close the app or use another library.
