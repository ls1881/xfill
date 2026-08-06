#!/usr/bin/env python3
"""Re-runs only the (solver, grid) pairs that show TIMEOUT in results.csv,
at RERUN_TIMEOUT_SECONDS instead of run_benchmark.py's TIMEOUT_SECONDS, and
merges the results back in place. Everything that already finished
(SOLVED/UNSAT) within the original cap is left untouched -- this targets
exactly the pairs the original cap was too aggressive for, without
re-running (and re-paying the wall-clock cost for) everything that already
worked.

Requires results.csv, results_trials.csv, and results/scratch_grids/ (the
per-grid .grid/.savin.txt files) to already exist from a prior
run_benchmark.py run.

Usage:
    .venv/bin/python3 rerun_timeouts.py
"""
import csv
import sys

import run_benchmark as rb

RERUN_TIMEOUT_SECONDS = 600.0
rb.TIMEOUT_SECONDS = RERUN_TIMEOUT_SECONDS

RESULTS_CSV = rb.OUT_DIR / "results.csv"
TRIALS_CSV = rb.OUT_DIR / "results_trials.csv"
SCRATCH = rb.OUT_DIR / "scratch_grids"


def grid_path_for(name, source):
    d = rb.CURATED_DIR if source == "curated" else rb.SCRAPED_DIR
    return d / f"{name}.txt"


def write_all(rows, trial_rows):
    fieldnames = list(rows[0].keys())
    with open(RESULTS_CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)

    trial_rows_sorted = sorted(trial_rows, key=lambda t: (t["grid"], t["solver"], int(t["trial"])))
    with open(TRIALS_CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["grid", "source", "solver", "trial", "status", "time"])
        w.writeheader()
        w.writerows(trial_rows_sorted)


def main():
    with open(RESULTS_CSV) as f:
        rows = list(csv.DictReader(f))
    with open(TRIALS_CSV) as f:
        trial_rows = list(csv.DictReader(f))

    # savin_crossword timed out on every grid at 120s, including trivial
    # 5x5/7x7 ones, and a 300s spot-check on those same trivial grids still
    # timed out (see results_120s_backup.csv / the rerun log) -- confirming
    # this is not a timeout-budget artifact. It is excluded from further
    # reruns; its existing 120s-cap TIMEOUT/UNSAT entries are left as-is.
    RANDOMIZED = [("xfill", rb.run_xfill), ("orca", rb.run_orca)]
    SINGLE_RUN = [("ingrid", rb.run_ingrid), ("composer", rb.run_composer)]

    changed = False
    for row in rows:
        name, source = row["grid"], row["source"]
        grid_path = grid_path_for(name, source)
        orca_path = SCRATCH / f"{name}.grid"

        for solver, run_fn in RANDOMIZED:
            if row[f"{solver}_status"] != "TIMEOUT":
                continue
            arg = orca_path if solver == "orca" else grid_path
            print(f"rerunning {name} {solver} (median of {rb.N_TRIALS_RANDOMIZED} "
                  f"trials, {RERUN_TIMEOUT_SECONDS:.0f}s cap)", file=sys.stderr, flush=True)
            trials = [run_fn(arg) for _ in range(rb.N_TRIALS_RANDOMIZED)]
            trial_rows = [t for t in trial_rows
                          if not (t["grid"] == name and t["solver"] == solver)]
            for trial_num, (status, wall) in enumerate(trials, 1):
                trial_rows.append({"grid": name, "source": source, "solver": solver,
                                    "trial": trial_num, "status": status, "time": wall})
            median_status, median_time = sorted(trials, key=lambda t: t[1])[len(trials) // 2]
            row[f"{solver}_status"], row[f"{solver}_time"] = median_status, median_time
            changed = True
            print(f"  -> {median_status} {median_time:.2f}s (was TIMEOUT)",
                  file=sys.stderr, flush=True)
            write_all(rows, trial_rows)

        for solver, run_fn in SINGLE_RUN:
            if row[f"{solver}_status"] != "TIMEOUT":
                continue
            print(f"rerunning {name} {solver} ({RERUN_TIMEOUT_SECONDS:.0f}s cap)",
                  file=sys.stderr, flush=True)
            status, wall = run_fn(grid_path)
            row[f"{solver}_status"], row[f"{solver}_time"] = status, wall
            changed = True
            print(f"  -> {status} {wall:.2f}s (was TIMEOUT)", file=sys.stderr, flush=True)
            write_all(rows, trial_rows)

    if not changed:
        print("nothing left to rerun", file=sys.stderr)
        return

    print(f"updated {RESULTS_CSV} and {TRIALS_CSV}", file=sys.stderr)


if __name__ == "__main__":
    main()
