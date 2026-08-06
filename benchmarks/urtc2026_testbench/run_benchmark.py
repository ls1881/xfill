#!/usr/bin/env python3
"""Reproducible multi-solver crossword-filling benchmark for the URTC 2026
paper (paper/xfill_urtc2026.md). See README.md in this directory for full
methodology, solver provenance (versions/commit hashes), and how to
re-obtain each external solver.

Solvers compared:
  - xfill        (this repo's build/xfill_cli)
  - orca-solver  (partition-based parallel CSP solver)
  - ingrid_core  (single-threaded reference solver)
  - crossword-composer (static-order recursive backtracker, no restarts)

A fifth solver, savin_crossword (textbook AC-3 + backtracking, CS50-style
baseline), is implemented (see run_savin()) but excluded from the default
sweep: it timed out on all 20 grids tested, including trivial 5x5/7x7
ones, at both a 120s and a 300s cap, ruling out "the cap was too short"
as the explanation. Re-running it costs real wall time for zero new
information, so it is not part of this script's main loop.

Dictionary: derived deterministically from this repo's own
data/spreadthewordlist_caps.txt (a freely available wordlist, already
committed), filtered to score >= MIN_SCORE. No paid/licensed wordlist is
used anywhere in this testbench, so results are fully reproducible by
anyone who clones this repo -- unlike some of this project's other
benchmarks, which used a paid dictionary and are documented as such
elsewhere.

Grids: the repo's existing size-graded `curated` set (5x5 through 21x21)
plus a fixed-seed random sample of the `scraped_15x15` corpus. Both sets
are already committed to this repo, so grid selection is reproducible
without re-downloading anything.

Timeout: a fixed, moderate per-(solver, grid) cap (see TIMEOUT_SECONDS),
deliberately much shorter than the multi-hour uncapped runs documented in
docs/design.md -- this testbench is meant to be re-run by a reader in a
bounded amount of time on a laptop, not to find the absolute limit of any
one solver.

Trials: xfill and orca-solver both race independent workers (a restart
portfolio and a partition-based search, respectively), so wall time on a
given (solver, grid) pair genuinely varies run to run -- OS thread
scheduling decides which worker gets there first, not just the solver's
own logic. Both run N_TRIALS_RANDOMIZED times per grid; results.csv
reports the median trial, and every individual trial is logged to
results_trials.csv for full transparency. ingrid_core and
crossword-composer are single-threaded with no restart logic -- given the
same grid and dictionary they are deterministic, so each runs once.
"""
import csv
import random
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TESTBENCH_DIR = Path(__file__).resolve().parent
EXTERNAL = TESTBENCH_DIR / "external_solvers"  # see README.md to populate

XFILL_CLI = REPO / "build" / "xfill_cli"
ORCA_BIN = EXTERNAL / "orca-solver" / "target" / "release" / "orca"
INGRID_BIN = EXTERNAL / "ingrid_core" / "target" / "release" / "ingrid_core"
COMPOSER_BIN = EXTERNAL / "crossword-composer" / "target" / "release" / "cli"
SAVIN_DIR = EXTERNAL / "savin_crossword"

SOURCE_WORDLIST = REPO / "data" / "spreadthewordlist_caps.txt"
MIN_SCORE = 40
TIMEOUT_SECONDS = 600.0
RANDOM_SEED = 20260807  # URTC 2026 submission deadline, used as a fixed,
                        # documented seed rather than an arbitrary one
N_SCRAPED_SAMPLE = 12

# xfill and orca-solver both race independent workers (restart-portfolio and
# partition-based respectively): which attempt/partition a thread lands on,
# and which one finishes first, depends on OS thread scheduling, so wall
# time genuinely varies run to run for these two specifically -- exactly
# the variance the shared-weight ablation elsewhere in this project already
# documents empirically. ingrid_core, crossword-composer, and savin_crossword
# are single-threaded with no restart logic and are deterministic given the
# same grid and dictionary, so repeating them adds no information (and, for
# savin_crossword, mostly just burns TIMEOUT_SECONDS for nothing). Only
# xfill/orca-solver get multiple trials; the other three run once.
N_TRIALS_RANDOMIZED = 5

CURATED_DIR = REPO / "benchmarks" / "grids" / "curated"
SCRAPED_DIR = REPO / "benchmarks" / "grids" / "scraped_15x15"

OUT_DIR = TESTBENCH_DIR / "results"
SCORED_DICT = OUT_DIR / "dict_min40.dict"       # WORD;SCORE, for xfill/orca/ingrid
PLAIN_DICT = OUT_DIR / "dict_min40_plain.txt"   # one word per line, for the two naive solvers


def build_dictionaries():
    """Deterministically derive both dictionary formats from the repo's
    own committed public wordlist -- never from a paid one."""
    words = []
    with open(SOURCE_WORDLIST) as f:
        for line in f:
            line = line.strip()
            if not line or ";" not in line:
                continue
            word, score = line.rsplit(";", 1)
            if int(score) >= MIN_SCORE:
                words.append((word, int(score)))
    words.sort()
    with open(SCORED_DICT, "w") as f:
        for w, s in words:
            f.write(f"{w};{s}\n")
    with open(PLAIN_DICT, "w") as f:
        for w, _ in words:
            f.write(w + "\n")
    print(f"dictionary: {len(words)} entries (score >= {MIN_SCORE}), "
          f"derived from {SOURCE_WORDLIST.name}", file=sys.stderr)


def select_grids():
    curated = sorted(CURATED_DIR.glob("*.txt"))
    all_scraped = sorted(SCRAPED_DIR.glob("*.txt"))
    rng = random.Random(RANDOM_SEED)
    scraped_sample = sorted(rng.sample(all_scraped, N_SCRAPED_SAMPLE), key=lambda p: p.name)
    grids = [("curated", p) for p in curated] + [("scraped", p) for p in scraped_sample]
    print(f"grids: {len(curated)} curated + {len(scraped_sample)} scraped "
          f"(seed={RANDOM_SEED}) = {len(grids)} total", file=sys.stderr)
    return grids


def grid_dims(path):
    rows = [l for l in path.read_text().splitlines() if l.strip()]
    return len(rows), (len(rows[0]) if rows else 0)


def to_orca_grid(xfill_path, out_path):
    rows = [l for l in xfill_path.read_text().splitlines() if l.strip()]
    out_path.write_text(f"{len(rows)} {len(rows[0])}\n" + "\n".join(rows) + "\n")


def to_savin_structure(xfill_path, out_path):
    # xfill: '.' open, '#' block. savin_crossword: '_' open, '#' block.
    rows = [l for l in xfill_path.read_text().splitlines() if l.strip()]
    out_path.write_text("\n".join(r.replace(".", "_") for r in rows) + "\n")


def run_capped(cmd, cwd=None):
    start = time.time()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=TIMEOUT_SECONDS, cwd=cwd)
        return proc, time.time() - start, False
    except subprocess.TimeoutExpired:
        return None, TIMEOUT_SECONDS, True


def run_xfill(grid_path):
    proc, wall, timed_out = run_capped(
        [str(XFILL_CLI), str(grid_path), str(SCORED_DICT), "0", "0"])
    if timed_out:
        return "TIMEOUT", TIMEOUT_SECONDS
    if "No solution found" in proc.stdout:
        return "UNSAT", wall
    m = re.search(r"time=([\d.eE+-]+)s", proc.stdout)
    return ("SOLVED", float(m.group(1))) if m else ("ERROR", wall)


def run_orca(orca_grid_path):
    proc, wall, timed_out = run_capped(
        [str(ORCA_BIN), "fill", str(orca_grid_path), str(SCORED_DICT), "-n", "1",
         "-j", "14", "--no-browser", "--disallow-shared-substring", "0"])
    if timed_out:
        return "TIMEOUT", TIMEOUT_SECONDS
    if "Solution 1" in proc.stdout:
        return "SOLVED", wall
    if "Final stats" in proc.stderr or "Unfillable" in proc.stderr:
        return "UNSAT", wall
    return "ERROR", wall


def run_ingrid(grid_path):
    proc, wall, timed_out = run_capped(
        [str(INGRID_BIN), "--wordlist", str(SCORED_DICT), "--min-score", "0",
         "--time", str(grid_path)])
    if timed_out:
        return "TIMEOUT", TIMEOUT_SECONDS
    if proc.returncode != 0:
        return ("UNSAT", wall) if "Unfillable" in proc.stderr else ("ERROR", wall)
    m = re.search(r"([\d.]+)(ms|s) finding fill", proc.stderr)
    if not m:
        return "ERROR", wall
    val, unit = float(m.group(1)), m.group(2)
    return "SOLVED", (val / 1000.0 if unit == "ms" else val)


def run_composer(grid_path):
    if not COMPOSER_BIN.exists():
        return "MISSING", 0.0
    proc, wall, timed_out = run_capped([str(COMPOSER_BIN), str(grid_path), str(PLAIN_DICT)])
    if timed_out:
        return "TIMEOUT", TIMEOUT_SECONDS
    if proc.returncode != 0:
        return "ERROR", wall
    if proc.stdout.startswith("SOLVED"):
        m = re.search(r"time=([\d.]+)s", proc.stdout)
        return "SOLVED", float(m.group(1)) if m else wall
    return "UNSAT", wall


def run_savin(structure_path):
    """Kept for reproducibility/documentation, but not called by main()'s
    default sweep: savin_crossword timed out on every one of 20 grids
    tested, including trivial 5x5/7x7 grids, at both a 120s and a 300s cap
    -- its least-constraining-value step recomputes an O(|domain|^2) score
    from scratch at every node, which the ~184,000-word dictionary used
    here makes prohibitive regardless of grid difficulty. Confirming that
    on the easiest grids in the corpus (not just the hardest) is what rules
    out "the cap was too short" as the explanation. See README.md."""
    if not (SAVIN_DIR / "generate.py").exists():
        return "MISSING", 0.0
    proc, wall, timed_out = run_capped(
        [sys.executable, "generate.py", str(structure_path), str(PLAIN_DICT)],
        cwd=str(SAVIN_DIR))
    if timed_out:
        return "TIMEOUT", TIMEOUT_SECONDS
    if proc.returncode != 0:
        return "ERROR", wall
    if "No solution" in proc.stdout:
        return "UNSAT", wall
    return "SOLVED", wall


def run_with_trials(run_fn, arg, solver_name, grid_name, source, trials_writer, trials_f):
    """Runs a randomized (multi-threaded, racing-workers) solver
    N_TRIALS_RANDOMIZED times and returns the median trial by wall time
    (timed-out trials sort at TIMEOUT_SECONDS). Every individual trial is
    also logged to trials_csv for transparency -- the median is a summary
    of that raw data, not a separate measurement."""
    trials = [run_fn(arg) for _ in range(N_TRIALS_RANDOMIZED)]
    for trial_num, (status, wall) in enumerate(trials, 1):
        trials_writer.writerow({"grid": grid_name, "source": source, "solver": solver_name,
                                 "trial": trial_num, "status": status, "time": wall})
    trials_f.flush()
    median_status, median_time = sorted(trials, key=lambda t: t[1])[len(trials) // 2]
    return median_status, median_time


def main():
    OUT_DIR.mkdir(exist_ok=True)
    scratch = OUT_DIR / "scratch_grids"
    scratch.mkdir(exist_ok=True)

    build_dictionaries()
    grids = select_grids()

    fieldnames = ["grid", "source", "rows", "cols",
                  "xfill_status", "xfill_time",
                  "orca_status", "orca_time",
                  "ingrid_status", "ingrid_time",
                  "composer_status", "composer_time"]
    out_csv = OUT_DIR / "results.csv"
    f = open(out_csv, "w", newline="")
    writer = csv.DictWriter(f, fieldnames=fieldnames)
    writer.writeheader()

    trials_csv = OUT_DIR / "results_trials.csv"
    trials_f = open(trials_csv, "w", newline="")
    trials_writer = csv.DictWriter(trials_f, fieldnames=["grid", "source", "solver", "trial", "status", "time"])
    trials_writer.writeheader()

    print(f"xfill/orca-solver: {N_TRIALS_RANDOMIZED} trials each, median reported "
          f"(both race independent workers, so wall time varies run to run; "
          f"ingrid_core/crossword-composer are single-threaded and deterministic, "
          f"so they run once; savin_crossword is excluded -- see run_savin()'s "
          f"docstring and README.md for why)", file=sys.stderr)

    for i, (source, grid_path) in enumerate(grids, 1):
        name = grid_path.stem
        rows, cols = grid_dims(grid_path)
        orca_path = scratch / f"{name}.grid"
        to_orca_grid(grid_path, orca_path)

        row = {"grid": name, "source": source, "rows": rows, "cols": cols}
        row["xfill_status"], row["xfill_time"] = run_with_trials(
            run_xfill, grid_path, "xfill", name, source, trials_writer, trials_f)
        row["orca_status"], row["orca_time"] = run_with_trials(
            run_orca, orca_path, "orca", name, source, trials_writer, trials_f)
        row["ingrid_status"], row["ingrid_time"] = run_ingrid(grid_path)
        row["composer_status"], row["composer_time"] = run_composer(grid_path)

        print(f"[{i:2d}/{len(grids)}] {name:24s} {rows:2d}x{cols:<2d} "
              f"xfill(med5)={row['xfill_status']:8s}{row['xfill_time']:6.2f}s  "
              f"orca(med5)={row['orca_status']:8s}{row['orca_time']:6.2f}s  "
              f"ingrid={row['ingrid_status']:8s}{row['ingrid_time']:6.2f}s  "
              f"composer={row['composer_status']:8s}{row['composer_time']:6.2f}s",
              flush=True)
        writer.writerow(row)
        f.flush()

    f.close()
    trials_f.close()
    print(f"\nwrote {out_csv}", file=sys.stderr)
    print(f"wrote {trials_csv} ({N_TRIALS_RANDOMIZED} raw trials per grid for xfill/orca-solver)",
          file=sys.stderr)


if __name__ == "__main__":
    main()
