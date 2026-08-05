#!/usr/bin/env python3
"""Generate the figures used in paper/xfill_urtc2026.md from
results/results.csv. Each figure is built to make one specific point the
paper actually argues, not a generic "solver X vs solver Y" dump -- see
the comment above each function for which claim it backs.

Run after run_benchmark.py has produced results/results.csv:
    .venv/bin/python3 generate_figures.py
"""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np

TESTBENCH_DIR = Path(__file__).resolve().parent
RESULTS_CSV = TESTBENCH_DIR / "results" / "results.csv"
FIG_DIR = TESTBENCH_DIR / "results" / "figures"
TIMEOUT_SECONDS = 30.0

# Shared, restrained publication style: no gridlines competing with data,
# a single accent color reused consistently per solver across all figures.
plt.rcParams.update({
    "font.family": "serif",
    "font.size": 9,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.linewidth": 0.8,
    "figure.dpi": 200,
    "savefig.dpi": 300,
    "savefig.bbox": "tight",
})

SOLVER_COLOR = {
    "xfill": "#1b5e8f",       # blue -- this project
    "orca": "#c0392b",        # red -- closest sophisticated competitor
    "ingrid": "#7f8c8d",      # grey -- reference baseline
    "composer": "#e08e2b",    # orange -- naive-but-real backtracker
    "savin": "#8e44ad",       # purple -- textbook CS50-style baseline
}
SOLVER_LABEL = {
    "xfill": "xfill",
    "orca": "orca-solver",
    "ingrid": "ingrid_core",
    "composer": "crossword-composer",
    "savin": "savin_crossword",
}
SOLVERS = ["xfill", "orca", "ingrid", "composer", "savin"]


def load_rows():
    with open(RESULTS_CSV) as f:
        return list(csv.DictReader(f))


def solved_time(row, solver):
    status = row[f"{solver}_status"]
    t = float(row[f"{solver}_time"])
    return t if status == "SOLVED" else None


# Figure 1: does each solver's success rate hold up as grid size grows?
# This is the figure motivating why architecture (restarts/parallelism/
# heuristics) matters at all -- if every solver scaled fine, the rest of
# the paper's comparison would be moot.
def fig_success_by_size(rows):
    curated = [r for r in rows if r["source"] == "curated"]
    curated.sort(key=lambda r: int(r["rows"]) * int(r["cols"]))
    sizes = [f"{r['rows']}x{r['cols']}" for r in curated]
    x = np.arange(len(sizes))

    fig, ax = plt.subplots(figsize=(6.2, 3.0))
    width = 0.15
    for i, solver in enumerate(SOLVERS):
        solved = [1 if r[f"{solver}_status"] == "SOLVED" else 0 for r in curated]
        ax.bar(x + (i - 2) * width, solved, width,
               label=SOLVER_LABEL[solver], color=SOLVER_COLOR[solver])
    ax.set_xticks(x)
    ax.set_xticklabels(sizes, rotation=0)
    ax.set_ylim(0, 1.15)
    ax.set_yticks([0, 1])
    ax.set_yticklabels(["timeout/UNSAT", "solved"])
    ax.set_xlabel(f"grid size (curated size-graded set, {TIMEOUT_SECONDS:.0f}s cap)")
    ax.set_title("Solve success by grid size")
    ax.legend(loc="lower left", ncol=3, fontsize=7, frameon=False)
    fig.savefig(FIG_DIR / "fig1_success_by_size.pdf")
    fig.savefig(FIG_DIR / "fig1_success_by_size.png")
    plt.close(fig)


# Figure 2: on the 15x15 scale where a real crossword actually lives, how
# do wall times compare among the solvers that finish at all? Naive
# baselines are expected to be nearly absent from this figure by 15x15 --
# that absence *is* the point, not a flaw in the figure.
def fig_15x15_times(rows):
    fifteens = [r for r in rows if r["rows"] == "15" and r["cols"] == "15"]
    fig, ax = plt.subplots(figsize=(6.2, 3.2))
    for i, solver in enumerate(SOLVERS):
        times = sorted(t for r in fifteens if (t := solved_time(r, solver)) is not None)
        if not times:
            continue
        y = np.arange(1, len(times) + 1) / len(fifteens)
        ax.step(times, y, where="post", label=SOLVER_LABEL[solver],
                 color=SOLVER_COLOR[solver], linewidth=1.6)
    ax.set_xscale("log")
    ax.set_xlabel("wall time to solve (s, log scale)")
    ax.set_ylabel(f"fraction of {len(fifteens)} 15x15 grids solved")
    ax.set_ylim(0, 1.02)
    ax.set_title("Cumulative solve rate on 15x15 grids")
    ax.legend(fontsize=7, frameon=False)
    fig.savefig(FIG_DIR / "fig2_15x15_cumulative.pdf")
    fig.savefig(FIG_DIR / "fig2_15x15_cumulative.png")
    plt.close(fig)


# Figure 3: the paper's actual claim about xfill/orca specifically --
# they succeed on different, only partially-overlapping subsets, which a
# single "solver A beats solver B" framing would hide. Drawn as a 2x2
# outcome grid (both / xfill-only / orca-only / neither) rather than a
# bar chart, since the *overlap structure* is the point being made.
def fig_xfill_orca_overlap(rows):
    both = xfill_only = orca_only = both_unsat = neither = 0
    for r in rows:
        xstat, ostat = r["xfill_status"], r["orca_status"]
        xs, os_ = xstat == "SOLVED", ostat == "SOLVED"
        if xs and os_:
            both += 1
        elif xs:
            xfill_only += 1
        elif os_:
            orca_only += 1
        elif xstat == "UNSAT" and ostat == "UNSAT":
            both_unsat += 1  # a success (agreeing proof), not a failure -- kept
                              # separate from genuine timeouts below
        else:
            neither += 1

    fig, ax = plt.subplots(figsize=(5.2, 3.2))
    cats = ["both\nsolve", "xfill\nonly", "orca-solver\nonly", "both prove\nUNSAT", "neither\nresolved"]
    vals = [both, xfill_only, orca_only, both_unsat, neither]
    colors = ["#4a4a4a", SOLVER_COLOR["xfill"], SOLVER_COLOR["orca"], "#2e7d4f", "#c7c7c7"]
    bars = ax.bar(cats, vals, color=colors)
    for b, v in zip(bars, vals):
        ax.text(b.get_x() + b.get_width() / 2, v + 0.2, str(v),
                 ha="center", va="bottom", fontsize=9)
    ax.set_ylabel(f"number of grids (of {len(rows)})")
    ax.set_title("xfill vs. orca-solver: outcome overlap")
    fig.savefig(FIG_DIR / "fig3_xfill_orca_overlap.pdf")
    fig.savefig(FIG_DIR / "fig3_xfill_orca_overlap.png")
    plt.close(fig)


def print_summary_table(rows):
    print("\nSummary (for Table in paper):")
    print(f"{'solver':22s} {'solved':>8s} {'timeout':>8s} {'unsat':>8s} {'error':>8s}")
    for solver in SOLVERS:
        statuses = [r[f"{solver}_status"] for r in rows]
        print(f"{SOLVER_LABEL[solver]:22s} "
              f"{statuses.count('SOLVED'):8d} "
              f"{statuses.count('TIMEOUT'):8d} "
              f"{statuses.count('UNSAT'):8d} "
              f"{sum(1 for s in statuses if s not in ('SOLVED','TIMEOUT','UNSAT')):8d}")


def main():
    FIG_DIR.mkdir(parents=True, exist_ok=True)
    rows = load_rows()
    fig_success_by_size(rows)
    fig_15x15_times(rows)
    fig_xfill_orca_overlap(rows)
    print_summary_table(rows)
    print(f"\nfigures written to {FIG_DIR}")


if __name__ == "__main__":
    main()
