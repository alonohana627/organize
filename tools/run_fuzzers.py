#!/usr/bin/env python3
"""Runs every libFuzzer target of a fuzz build for a fixed time and reports what each one found.

Usage:
    cmake --preset fuzz && cmake --build --preset fuzz
    python tools/run_fuzzers.py                               # every target, 60 s each, 4 at a time
    python tools/run_fuzzers.py --seconds 600 query_parser    # one target, longer
    python tools/run_fuzzers.py --build-dir build/agent-fuzz --jobs 2

Each target fuzz_<name> starts from its checked-in seed corpus tests/fuzz/corpus/<name>/, which is only
read: new inputs go to a scratch corpus in a temporary directory (deleted afterwards unless --keep-corpus
names a directory). A crash, a sanitizer report, a leak, a timeout (one input running longer than
--input-timeout seconds) or running out of memory makes libFuzzer exit non-zero and write the input to
--artifacts (default: <build-dir>/fuzz-artifacts/<name>/), where it can be replayed with
`fuzz_<name> <artifact>`. The script exits with 1 when any target failed, so CI fails with it.

On Windows, run it from the MSYS2 CLANG64 shell (or with C:\\msys64\\clang64\\bin on PATH): the
targets load libc++ and the sanitizer runtimes from there.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
CORPUS_ROOT = REPOSITORY_ROOT / "tests" / "fuzz" / "corpus"
# Lines of a failing target's output shown in the summary (the sanitizer report is near the end).
REPORT_TAIL_LINES = 60


@dataclass
class Outcome:
    """What one fuzz run ended with."""

    name: str
    exit_code: int
    seconds: float
    output: str

    @property
    def passed(self) -> bool:
        return self.exit_code == 0


def find_targets(build_directory: Path) -> dict[str, Path]:
    """Every fuzz target executable in the build, by name (fuzz_<name>[.exe] -> name)."""
    fuzz_directory = build_directory / "tests" / "fuzz"
    targets = {}
    for path in sorted(fuzz_directory.glob("fuzz_*")):
        if path.is_file() and (path.suffix in ("", ".exe")):
            targets[path.stem.removeprefix("fuzz_")] = path
    return targets


def run_target(name: str, executable: Path, arguments: argparse.Namespace, scratch_root: Path) -> Outcome:
    """Runs one target for the configured time from a scratch corpus plus its seed corpus."""
    seeds = CORPUS_ROOT / name
    scratch = scratch_root / name
    scratch.mkdir(parents=True, exist_ok=True)
    artifacts = Path(arguments.artifacts or arguments.build_dir / "fuzz-artifacts") / name
    artifacts.mkdir(parents=True, exist_ok=True)
    command = [str(executable),
               f"-max_total_time={arguments.seconds}",
               f"-timeout={arguments.input_timeout}",
               f"-rss_limit_mb={arguments.rss_limit_mb}",
               f"-artifact_prefix={artifacts.as_posix()}/",
               "-print_final_stats=1",
               str(scratch), str(seeds)]
    started = time.monotonic()
    # The whole budget plus a margin for start-up and the final merge; a target that hangs beyond
    # that is a failure too.
    try:
        completed = subprocess.run(command, capture_output=True, text=True, errors="replace",
                                   timeout=arguments.seconds + 120, check=False)
        exit_code, output = completed.returncode, completed.stdout + completed.stderr
    except subprocess.TimeoutExpired as expired:
        exit_code = -1
        output = f"{expired.output or ''}{expired.stderr or ''}\nrun_fuzzers: the target did not stop in time"
    except OSError as error:
        exit_code, output = -1, f"run_fuzzers: cannot start {executable}: {error}"
    return Outcome(name, exit_code, time.monotonic() - started, output)


def summary_line(outcome: Outcome) -> str:
    """The libFuzzer statistics of a run in one line (executions and coverage)."""
    statistics = {}
    for line in outcome.output.splitlines():
        if line.startswith("stat::"):
            key, _, value = line.removeprefix("stat::").partition(":")
            statistics[key.strip()] = value.strip()
    executions = statistics.get("number_of_executed_units", "?")
    coverage = next((line.split("cov: ")[1].split()[0] for line in reversed(outcome.output.splitlines())
                     if "cov: " in line), "?")
    verdict = "ok" if outcome.passed else f"FAILED (exit {outcome.exit_code})"
    return f"{outcome.name:<18} {verdict:<18} {outcome.seconds:6.0f} s  {executions:>10} runs  coverage {coverage}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="targets to run (default: every target in the build)")
    parser.add_argument("--build-dir", type=Path, default=REPOSITORY_ROOT / "build" / "fuzz",
                        help="the fuzz build (default: build/fuzz, the fuzz preset's directory)")
    parser.add_argument("--seconds", type=int, default=60, help="fuzzing time per target (default 60)")
    parser.add_argument("--jobs", type=int, default=4, help="targets run at the same time (default 4)")
    parser.add_argument("--input-timeout", type=int, default=25,
                        help="seconds one input may take before it counts as a hang (default 25)")
    parser.add_argument("--rss-limit-mb", type=int, default=2048, help="memory limit per target (default 2048)")
    parser.add_argument("--artifacts", type=Path, help="where crashing inputs go (default <build-dir>/fuzz-artifacts)")
    parser.add_argument("--keep-corpus", type=Path, help="keep the grown corpora in this directory")
    arguments = parser.parse_args()

    arguments.build_dir = arguments.build_dir.resolve()
    targets = find_targets(arguments.build_dir)
    if not targets:
        parser.error(f"no fuzz targets in {arguments.build_dir / 'tests' / 'fuzz'}; build the fuzz preset first")
    unknown = set(arguments.names) - set(targets)
    if unknown:
        parser.error(f"unknown targets: {', '.join(sorted(unknown))}; built: {', '.join(targets)}")
    selected = {name: path for name, path in targets.items() if not arguments.names or name in arguments.names}
    missing_seeds = [name for name in selected if not (CORPUS_ROOT / name).is_dir()]
    if missing_seeds:
        parser.error(f"no seed corpus for: {', '.join(missing_seeds)} (tests/fuzz/corpus/<name>/)")

    print(f"Fuzzing {len(selected)} targets for {arguments.seconds} s each, {arguments.jobs} at a time", flush=True)
    scratch_root = Path(tempfile.mkdtemp(prefix="mso-fuzz-"))
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, arguments.jobs)) as pool:
            futures = [pool.submit(run_target, name, path, arguments, scratch_root) for name, path in selected.items()]
            outcomes = []
            for future in concurrent.futures.as_completed(futures):
                outcome = future.result()
                outcomes.append(outcome)
                print(summary_line(outcome), flush=True)
        if arguments.keep_corpus:
            shutil.copytree(scratch_root, arguments.keep_corpus, dirs_exist_ok=True)
    finally:
        shutil.rmtree(scratch_root, ignore_errors=True)

    failed = [outcome for outcome in outcomes if not outcome.passed]
    for outcome in sorted(failed, key=lambda item: item.name):
        tail = "\n".join(outcome.output.splitlines()[-REPORT_TAIL_LINES:])
        print(f"\n===== fuzz_{outcome.name} failed (exit {outcome.exit_code}) =====\n{tail}", flush=True)
    print(f"\n{len(outcomes) - len(failed)} of {len(outcomes)} targets ran clean", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
