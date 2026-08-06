#!/usr/bin/env python3
"""Two targeted experiments isolating specifically what xfill contributes,
run separately from run_benchmark.py's general 5-solver sweep because they
need a harder grid set (the earlier sweep's grids mostly solve in well
under a second, too fast for either effect to show up) and, for the
ablation, a purpose-built toggle that doesn't exist in any other solver.

1. Shared-conflict-weight ablation (HARD_GRIDS): xfill run twice per grid,
   identical in every way except the XFILL_DISABLE_SHARED_WEIGHTS env var
   -- isolates this paper's actual mechanism from every other difference
   between solvers (dictionary, language, everything), which comparing
   against a different codebase (orca-solver) cannot do.
2. The same ablation, on STANDARD_GRIDS instead: the exact 12-grid scraped
   sample every other scraped-grid figure in the paper uses, rather than
   the separately-curated "known hard" list above. HARD_GRIDS answers "how
   does the mechanism behave on especially hard grids"; STANDARD_GRIDS
   answers "how does it behave on the same grids the rest of the paper's
   architecture comparison already uses" -- a real gap otherwise, since
   the mechanism was never actually tested against the paper's own main
   corpus.
3. Thread-count scaling, xfill vs. orca-solver, on HARD_GRIDS -- shows the
   two architectures respond to added parallelism in opposite ways.

HARD_GRIDS is a fixed, already-curated "known hard" list reused from this
project's own earlier development-time benchmarking (documented in
docs/design.md), not cherry-picked for this paper -- these grids were
identified as hard for restart-based search before this paper existed.
"""
import csv
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TESTBENCH_DIR = Path(__file__).resolve().parent
EXTERNAL = TESTBENCH_DIR / "external_solvers"

XFILL_CLI = REPO / "build" / "xfill_cli"
ORCA_BIN = EXTERNAL / "orca-solver" / "target" / "release" / "orca"
SCORED_DICT = TESTBENCH_DIR / "results" / "dict_min40.dict"
SCRAPED_DIR = REPO / "benchmarks" / "grids" / "scraped_15x15"

HARD_GRIDS = ["grid_045", "grid_053", "grid_058", "grid_072", "grid_115",
              "grid_120", "grid_217", "grid_303", "grid_309", "grid_347",
              "grid_380", "grid_457"]

# The exact 12 grids run_benchmark.py samples from scraped_15x15 (fixed
# seed 20260807) -- the same corpus every other scraped-grid figure in the
# paper uses. HARD_GRIDS above was never tested against this corpus, only
# against an older, separately-curated "known hard" list -- this constant
# closes that gap, so the ablation and the main comparison rest on the
# same grids instead of two different ones.
STANDARD_GRIDS = ["grid_018", "grid_030", "grid_149", "grid_161", "grid_170",
                   "grid_186", "grid_222", "grid_229", "grid_244", "grid_395",
                   "grid_423", "grid_479"]

ABLATION_TIMEOUT = 45.0
ABLATION_TRIALS = 2
ABLATION_THREADS = 14

STANDARD_ABLATION_TIMEOUT = 300.0
STANDARD_ABLATION_TRIALS = 5
STANDARD_ABLATION_THREADS = 14

SCALING_TIMEOUT = 45.0
SCALING_THREADS = [1, 4, 8, 14, 21, 28, 42]
SCALING_GRIDS = ["grid_303", "grid_115", "grid_120"]  # a spread: partition-
                                                        # favoring, restart-
                                                        # favoring, and easy

OUT_DIR = TESTBENCH_DIR / "results"
SCRATCH = OUT_DIR / "scratch_grids"


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def run_xfill(grid_path, threads, disable_shared_weights=False, timeout=ABLATION_TIMEOUT):
    env = os.environ.copy()
    if disable_shared_weights:
        env["XFILL_DISABLE_SHARED_WEIGHTS"] = "1"
    start = time.time()
    try:
        proc = subprocess.run(
            [str(XFILL_CLI), str(grid_path), str(SCORED_DICT), "0", str(threads)],
            capture_output=True, text=True, timeout=timeout, env=env, cwd=TESTBENCH_DIR,
        )
    except subprocess.TimeoutExpired:
        return "TIMEOUT", timeout, None
    wall = time.time() - start
    if "No solution found" in proc.stdout:
        return "UNSAT", wall, None
    m = re.search(r"nodes=(\d+).*time=([\d.eE+-]+)s", proc.stdout)
    if m:
        return "SOLVED", float(m.group(2)), int(m.group(1))
    return "ERROR", wall, None


def run_orca(orca_grid_path, threads, timeout=SCALING_TIMEOUT):
    start = time.time()
    try:
        proc = subprocess.run(
            [str(ORCA_BIN), "fill", str(orca_grid_path), str(SCORED_DICT), "-n", "1",
             "-j", str(threads), "--no-browser", "--disallow-shared-substring", "0"],
            capture_output=True, text=True, timeout=timeout, cwd=TESTBENCH_DIR,
        )
    except subprocess.TimeoutExpired:
        return "TIMEOUT", timeout
    wall = time.time() - start
    if "Solution 1" in proc.stdout:
        return "SOLVED", wall
    if "Final stats" in proc.stderr:
        return "UNSAT", wall
    return "ERROR", wall


def to_orca_grid(xfill_path, out_path):
    rows = [l for l in xfill_path.read_text().splitlines() if l.strip()]
    out_path.write_text(f"{len(rows)} {len(rows[0])}\n" + "\n".join(rows) + "\n")


def ablation():
    log("=== ablation: shared conflict weights on/off, threads=14, "
        f"{ABLATION_TRIALS} trials/config ===")
    out_csv = OUT_DIR / "ablation.csv"
    fieldnames = ["grid", "with_median_time", "with_median_nodes",
                  "without_median_time", "without_median_nodes",
                  "with_status", "without_status"]
    with open(out_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for name in HARD_GRIDS:
            grid_path = SCRAPED_DIR / f"{name}.txt"
            with_times, with_nodes, with_status = [], [], "SOLVED"
            without_times, without_nodes, without_status = [], [], "SOLVED"
            for trial in range(ABLATION_TRIALS):
                status, t, nodes = run_xfill(grid_path, ABLATION_THREADS, disable_shared_weights=False)
                with_status = status
                with_times.append(t if status == "SOLVED" else ABLATION_TIMEOUT)
                if nodes:
                    with_nodes.append(nodes)
                status, t, nodes = run_xfill(grid_path, ABLATION_THREADS, disable_shared_weights=True)
                without_status = status
                without_times.append(t if status == "SOLVED" else ABLATION_TIMEOUT)
                if nodes:
                    without_nodes.append(nodes)
            row = {
                "grid": name,
                "with_median_time": statistics.median(with_times),
                "with_median_nodes": statistics.median(with_nodes) if with_nodes else "",
                "without_median_time": statistics.median(without_times),
                "without_median_nodes": statistics.median(without_nodes) if without_nodes else "",
                "with_status": with_status,
                "without_status": without_status,
            }
            log(f"{name:10s} WITH shared-weights: {with_status:8s} "
                f"median={row['with_median_time']:.2f}s  |  "
                f"WITHOUT: {without_status:8s} median={row['without_median_time']:.2f}s")
            writer.writerow(row)
            f.flush()
    log(f"wrote {out_csv}")


def ablation_standard_corpus():
    log("=== ablation on the standard scraped-15x15 corpus: shared conflict "
        f"weights on/off, threads={STANDARD_ABLATION_THREADS}, "
        f"{STANDARD_ABLATION_TRIALS} trials/config ===")
    out_csv = OUT_DIR / "ablation_standard.csv"
    fieldnames = ["grid", "config", "trial", "time", "solved"]
    with open(out_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for name in STANDARD_GRIDS:
            grid_path = SCRAPED_DIR / f"{name}.txt"
            for config, disable in [("without", True), ("with", False)]:
                for trial in range(1, STANDARD_ABLATION_TRIALS + 1):
                    status, t, _ = run_xfill(grid_path, STANDARD_ABLATION_THREADS,
                                              disable_shared_weights=disable,
                                              timeout=STANDARD_ABLATION_TIMEOUT)
                    solved = 1 if status in ("SOLVED", "UNSAT") else 0
                    wall = t if status in ("SOLVED", "UNSAT") else STANDARD_ABLATION_TIMEOUT
                    log(f"{name:10s} {config:8s} trial {trial}/{STANDARD_ABLATION_TRIALS}: "
                        f"{status:8s} {wall:.2f}s")
                    writer.writerow({"grid": name, "config": config, "trial": trial,
                                      "time": wall, "solved": solved})
                    f.flush()
    log(f"wrote {out_csv}")


def thread_scaling():
    log("=== thread-count scaling: xfill vs orca-solver ===")
    SCRATCH.mkdir(exist_ok=True)
    out_csv = OUT_DIR / "thread_scaling.csv"
    fieldnames = ["grid", "threads", "xfill_status", "xfill_time", "orca_status", "orca_time"]
    with open(out_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for name in SCALING_GRIDS:
            grid_path = SCRAPED_DIR / f"{name}.txt"
            orca_path = SCRATCH / f"{name}.grid"
            to_orca_grid(grid_path, orca_path)
            for threads in SCALING_THREADS:
                xstat, xtime, _ = run_xfill(grid_path, threads, timeout=SCALING_TIMEOUT)
                ostat, otime = run_orca(orca_path, threads, timeout=SCALING_TIMEOUT)
                log(f"{name:10s} threads={threads:3d}  xfill={xstat:8s}{xtime:6.2f}s  "
                    f"orca={ostat:8s}{otime:6.2f}s")
                writer.writerow({"grid": name, "threads": threads,
                                  "xfill_status": xstat, "xfill_time": xtime,
                                  "orca_status": ostat, "orca_time": otime})
                f.flush()
    log(f"wrote {out_csv}")


if __name__ == "__main__":
    ablation()
    ablation_standard_corpus()
    thread_scaling()
    log("=== ALL DONE ===")
