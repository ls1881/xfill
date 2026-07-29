#!/usr/bin/env python3
"""Runs xfill_cli over a reproducible random subset of the scraped 15x15
grids and reports nodes/backtracks/restarts/time, for iterating on solver
performance against real-world grid layouts (as opposed to the small
curated set in benchmarks/grids/).

Usage:
    python3 benchmarks/bench_subset.py
    python3 benchmarks/bench_subset.py --n 20 --seed 42 --timeout 20
    python3 benchmarks/bench_subset.py --save baseline.csv
    python3 benchmarks/bench_subset.py --compare baseline.csv
"""

import argparse
import csv
import random
import re
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GRIDS_DIR = REPO_ROOT / "benchmarks" / "grids" / "scraped_15x15"
STATS_RE = re.compile(r"nodes=(\d+) backtracks=(\d+) restarts=(\d+) time=([\d.eE+-]+)s")


def pick_subset(n, seed):
    grids = sorted(GRIDS_DIR.glob("grid_*.txt"))
    rng = random.Random(seed)
    return sorted(rng.sample(grids, n), key=lambda p: p.name)


def run_one(cli, dict_path, grid_path, min_score, timeout):
    start = time.time()
    try:
        proc = subprocess.run(
            [str(cli), str(grid_path), str(dict_path), str(min_score)],
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return {"grid": grid_path.name, "status": "TIMEOUT", "nodes": None,
                "backtracks": None, "restarts": None, "time": timeout}

    wall = time.time() - start
    if "No solution found" in proc.stdout:
        status = "UNSAT"
    elif STATS_RE.search(proc.stdout):
        status = "SOLVED"
    else:
        status = "ERROR"

    m = STATS_RE.search(proc.stdout)
    if m:
        nodes, backtracks, restarts, solve_time = m.groups()
        return {"grid": grid_path.name, "status": status, "nodes": int(nodes),
                "backtracks": int(backtracks), "restarts": int(restarts),
                "time": float(solve_time)}
    return {"grid": grid_path.name, "status": status, "nodes": None,
            "backtracks": None, "restarts": None, "time": wall}


def load_csv(path):
    with open(path, newline="") as f:
        return {row["grid"]: row for row in csv.DictReader(f)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--n", type=int, default=20)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--timeout", type=float, default=20.0, help="seconds per grid")
    parser.add_argument("--min-score", type=int, default=50)
    parser.add_argument("--cli", default=str(REPO_ROOT / "build" / "xfill_cli"))
    parser.add_argument("--dict", default=str(REPO_ROOT / "data" / "spreadthewordlist_caps.txt"))
    parser.add_argument("--save", help="write results to this CSV path")
    parser.add_argument("--compare", help="CSV from a previous --save run to diff against")
    args = parser.parse_args()

    subset = pick_subset(args.n, args.seed)
    prior = load_csv(args.compare) if args.compare else None

    results = []
    for grid_path in subset:
        r = run_one(args.cli, args.dict, grid_path, args.min_score, args.timeout)
        results.append(r)
        line = f"{r['grid']:16s} {r['status']:8s} nodes={str(r['nodes']):>8s} backtracks={str(r['backtracks']):>7s} restarts={str(r['restarts']):>4s} time={r['time']:.3f}s"
        if prior and r["grid"] in prior:
            p = prior[r["grid"]]
            if p["status"] == r["status"] == "SOLVED":
                delta = r["time"] - float(p["time"])
                line += f"  (prev {float(p['time']):.3f}s, {delta:+.3f}s)"
            elif p["status"] != r["status"]:
                line += f"  (prev status: {p['status']})"
        print(line)
        sys.stdout.flush()

    solved = [r for r in results if r["status"] == "SOLVED"]
    unsat = [r for r in results if r["status"] == "UNSAT"]
    timeout = [r for r in results if r["status"] == "TIMEOUT"]
    errors = [r for r in results if r["status"] == "ERROR"]

    print(f"\n{len(solved)} solved, {len(unsat)} unsat, {len(timeout)} timeout, {len(errors)} error"
          f" (n={len(results)}, timeout={args.timeout}s, min_score={args.min_score})")
    if solved:
        total_time = sum(r["time"] for r in solved)
        total_nodes = sum(r["nodes"] for r in solved)
        print(f"solved: total_time={total_time:.2f}s avg_time={total_time/len(solved):.3f}s"
              f" total_nodes={total_nodes} avg_nodes={total_nodes/len(solved):.1f}")

    if args.save:
        with open(args.save, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["grid", "status", "nodes", "backtracks", "restarts", "time"])
            writer.writeheader()
            for r in results:
                writer.writerow(r)
        print(f"saved to {args.save}")


if __name__ == "__main__":
    main()
