#!/usr/bin/env python3
"""Generate the figures used in paper/xfill_urtc2026.md from
results/results.csv. Each figure is built to make one specific point the
paper actually argues, not a generic "solver X vs solver Y" dump -- see
the comment above each function for which claim it backs.

Run after run_benchmark.py has produced results/results.csv:
    .venv/bin/python3 generate_figures.py
"""
import csv
import statistics
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np

TESTBENCH_DIR = Path(__file__).resolve().parent
RESULTS_CSV = TESTBENCH_DIR / "results" / "results.csv"
FIG_DIR = TESTBENCH_DIR / "results" / "figures"
TIMEOUT_SECONDS = 300.0

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

# savin_crossword is excluded from every figure here (and from
# results.csv itself, see run_benchmark.py): it timed out on all 20
# grids, including trivial 5x5/7x7 ones, at both a 120s and a 300s cap,
# ruling out "the cap was too short." Its color/label are kept only
# because generate_figures.py's docstring/comments still reference it.
SOLVER_COLOR = {
    "xfill": "#1b5e8f",       # blue -- this project
    "orca": "#c0392b",        # red -- closest sophisticated competitor
    "ingrid": "#7f8c8d",      # grey -- reference baseline
    "composer": "#e08e2b",    # orange -- naive-but-real backtracker
}
SOLVER_LABEL = {
    "xfill": "xfill",
    "orca": "orca-solver",
    "ingrid": "ingrid_core",
    "composer": "crossword-composer",
}
SOLVERS = ["xfill", "orca", "ingrid", "composer"]


def load_rows():
    with open(RESULTS_CSV) as f:
        return list(csv.DictReader(f))


def solved_time(row, solver):
    status = row[f"{solver}_status"]
    t = float(row[f"{solver}_time"])
    return t if status == "SOLVED" else None


# Figure 1: on the curated size-graded set, does xfill's edge over
# orca-solver survive as grid size grows, or is it only about which
# grids get solved at all? A binary solved/not-solved view (an earlier
# draft of this figure) hides the answer, since xfill, orca-solver, and
# ingrid_core mostly succeed on the *same* grids here -- the real
# difference is how long each takes to get there, sometimes by two to
# three orders of magnitude, which only shows up once time is the axis.
def fig_success_by_size(rows):
    curated = [r for r in rows if r["source"] == "curated"]
    curated.sort(key=lambda r: int(r["rows"]) * int(r["cols"]))
    sizes = [f"{r['rows']}x{r['cols']}" for r in curated]
    x = np.arange(len(sizes))
    TIMEOUT_MARK = TIMEOUT_SECONDS  # plotted at the cap, marked hollow

    fig, ax = plt.subplots(figsize=(6.4, 3.2))
    for solver in ["xfill", "orca", "ingrid", "composer"]:  # savin never
                                                             # solves anything
                                                             # at any size here
        ys, solved_mask = [], []
        for r in curated:
            status = r[f"{solver}_status"]
            if status in ("SOLVED", "UNSAT"):
                ys.append(max(float(r[f"{solver}_time"]), 1e-3))  # floor for log scale
                solved_mask.append(True)
            else:
                ys.append(TIMEOUT_MARK)
                solved_mask.append(False)
        ax.plot(x, ys, "-", color=SOLVER_COLOR[solver], linewidth=1.4, zorder=2)
        filled_x = [xi for xi, m in zip(x, solved_mask) if m]
        filled_y = [yi for yi, m in zip(ys, solved_mask) if m]
        hollow_x = [xi for xi, m in zip(x, solved_mask) if not m]
        hollow_y = [yi for yi, m in zip(ys, solved_mask) if not m]
        ax.scatter(filled_x, filled_y, color=SOLVER_COLOR[solver], s=26, zorder=3,
                   label=SOLVER_LABEL[solver], edgecolors="black", linewidths=0.4)
        ax.scatter(hollow_x, hollow_y, facecolors="none", edgecolors=SOLVER_COLOR[solver],
                   s=40, zorder=3, marker="^", linewidths=1.2)
    ax.axhline(TIMEOUT_SECONDS, color="#cccccc", linewidth=1, linestyle="--", zorder=1)
    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels(sizes, rotation=0, fontsize=7)
    ax.set_xlabel(f"grid size (curated size-graded set, {TIMEOUT_SECONDS:.0f}s cap)")
    ax.set_ylabel("time to solve or prove UNSAT (s, log scale)")
    ax.set_title("Time to solve by grid size (open triangle = timed out)")
    ax.legend(loc="upper left", ncol=2, fontsize=7, frameon=False)
    fig.tight_layout()
    fig.savefig(FIG_DIR / "fig1_success_by_size.pdf")
    fig.savefig(FIG_DIR / "fig1_success_by_size.png")
    plt.close(fig)


# Figure 1b: the same point (xfill vs. orca-solver, scraped 15x15 grids,
# both absolute time per grid), now with all four solvers shown side by
# side rather than just the two sophisticated ones -- so ingrid_core's
# and crossword-composer's gap from xfill/orca-solver isn't just a count
# reported in prose (Section V-C), it is directly visible, grid by grid,
# next to the two solvers that do compete. Solvers that time out on a
# given grid are drawn as hatched, hollow bars at the timeout cap rather
# than omitted, matching the "hollow triangle = timed out" convention
# already used in Fig. 1. xfill/orca-solver speedup ratio is kept as a
# label above those two bars, unchanged from the previous version.
#
# savin_crossword is excluded from SOLVERS entirely (see the comment
# above SOLVER_COLOR): it timed out on every single grid, including
# trivial 5x5/7x7 ones, at both a 120s and a 300s cap, ruling out "the
# cap was too short." A bar that would be 100% hatched on every single
# grid carries no information a reader doesn't already get from the
# one-sentence count in prose, so it would just be clutter here.
def fig_speedup(rows):
    scraped = [r for r in rows if r["source"] == "scraped"]

    def sort_key(r):
        if r["xfill_status"] in ("SOLVED", "UNSAT"):
            return (0, float(r["xfill_time"]))
        return (1, 0.0)

    scraped.sort(key=sort_key)
    names = [r["grid"] for r in scraped]
    x = np.arange(len(names))
    n = len(SOLVERS)
    width = 0.8 / n

    fig, ax = plt.subplots(figsize=(7.8, 2.3))
    for i, solver in enumerate(SOLVERS):
        offsets = x + (i - (n - 1) / 2) * width
        solid_x, solid_y, hollow_x, hollow_y = [], [], [], []
        for xi, r in zip(offsets, scraped):
            status = r[f"{solver}_status"]
            if status in ("SOLVED", "UNSAT"):
                solid_x.append(xi)
                solid_y.append(max(float(r[f"{solver}_time"]), 1e-3))
            else:
                hollow_x.append(xi)
                hollow_y.append(TIMEOUT_SECONDS)
        ax.bar(solid_x, solid_y, width, color=SOLVER_COLOR[solver])
        ax.bar(hollow_x, hollow_y, width, facecolor="none",
               edgecolor=SOLVER_COLOR[solver], hatch="////", linewidth=0.5)
    ax.axhline(TIMEOUT_SECONDS, color="#cccccc", linewidth=1, linestyle="--", zorder=0)

    # xfill/orca-solver speedup label, kept from the previous version of
    # this figure, computed only where both actually solved.
    ratios = []
    for xi, r in zip(x, scraped):
        if r["xfill_status"] == "SOLVED" and r["orca_status"] == "SOLVED":
            xt, ot = float(r["xfill_time"]), float(r["orca_time"])
            if xt > 0:
                ratio = ot / xt
                ratios.append(ratio)
                label = f"{ratio:.0f}x" if ratio >= 1 else f"1/{1 / ratio:.1f}x"
                color = SOLVER_COLOR["xfill"] if ratio >= 1 else SOLVER_COLOR["orca"]
                ax.text(xi, max(xt, ot) * 1.6, label, ha="center", va="bottom",
                         fontsize=6, color=color, fontweight="bold")

    ax.set_yscale("log")
    ax.set_ylim(top=TIMEOUT_SECONDS * 12)
    ax.set_xticks(x)
    ax.set_xticklabels(names, rotation=60, ha="right", fontsize=6.5)
    ax.set_ylabel(f"wall time to solve (s, log scale; {TIMEOUT_SECONDS:.0f}s cap)")

    import matplotlib.patches as mpatches
    handles = [mpatches.Patch(facecolor=SOLVER_COLOR[s], label=SOLVER_LABEL[s]) for s in SOLVERS]
    handles.append(mpatches.Patch(facecolor="white", edgecolor="black", hatch="////",
                                   label="timed out"))
    ax.legend(handles=handles, fontsize=6, frameon=False, ncol=3, loc="upper center")

    geomean = statistics.geometric_mean(ratios)
    ax.set_title(f"Wall time per grid, four solvers, scraped 15x15 grids (geomean {geomean:.0f}x)")
    fig.tight_layout()
    fig.savefig(FIG_DIR / "fig1b_speedup.pdf")
    fig.savefig(FIG_DIR / "fig1b_speedup.png")
    plt.close(fig)
    print(f"\nspeedup (scraped 15x15 only): n={len(ratios)}, median={statistics.median(ratios):.1f}x, "
          f"geomean={geomean:.1f}x, min={min(ratios):.2f}x, max={max(ratios):.1f}x")


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


# NOTE: an earlier version of this script also produced a
# fig3_xfill_orca_overlap bar chart (both-solve/xfill-only/orca-only/
# etc. counts). It was removed: on this particular 20-grid sample the
# two solvers are close enough (16 both, 1 xfill-only, 0 orca-only) that
# the chart mostly restated the summary counts already given in the
# paper's prose, rather than showing a trend, distribution, or
# relationship a reader couldn't get from one sentence -- see the
# "Real Grids" subsection in paper/xfill_urtc2026.tex for how that
# near-total overlap is instead used as a (textual) motivation for why
# Sections V-C/V-G turn to a deliberately harder grid list.


# Figure 4: the paper's core mechanism, isolated -- reported honestly.
# Same solver, same dictionary, same threads, same grid; the only
# variable is the XFILL_DISABLE_SHARED_WEIGHTS toggle. On this
# specific "hardest grids" set, 6 trials per config show NOT a clean
# win but a variance story: shared weights don't reliably move the
# median here and on one grid produce a catastrophic outlier, while the
# unshared baseline is consistently tighter. Drawn as individual trial
# points (not bars of a single summary stat), because the spread is
# the actual finding -- a bar chart of medians alone would hide it.
def fig_ablation_refined():
    path = TESTBENCH_DIR / "results" / "ablation_refined.csv"
    if not path.exists():
        return
    with open(path) as f:
        rows = list(csv.DictReader(f))
    grids = sorted(set(r["grid"] for r in rows), key=lambda g: [r["grid"] for r in rows].index(g))

    fig, axes = plt.subplots(1, len(grids), figsize=(2.4 * len(grids), 3.0), sharey=False)
    if len(grids) == 1:
        axes = [axes]
    for ax, grid in zip(axes, grids):
        for i, config in enumerate(["without", "with"]):
            times = [float(r["time"]) for r in rows if r["grid"] == grid and r["config"] == config]
            color = "#b0b0b0" if config == "without" else SOLVER_COLOR["xfill"]
            jitter = np.random.default_rng(0).uniform(-0.08, 0.08, size=len(times))
            ax.scatter([i + j for j in jitter], times, color=color, s=22, zorder=3,
                       edgecolors="black", linewidths=0.4)
            ax.hlines(statistics.median(times), i - 0.15, i + 0.15, color=color, linewidth=2, zorder=2)
        ax.set_xticks([0, 1])
        ax.set_xticklabels(["off", "on"], fontsize=8)
        ax.set_title(grid, fontsize=9)
    axes[0].set_ylabel("wall time to solve (s)")
    fig.suptitle("Shared conflict weights: 6 trials per config, 14 threads "
                 "(bar = median; grid 120 trial 6 timed out at 45s, off-scale)", fontsize=8)
    fig.tight_layout()
    fig.savefig(FIG_DIR / "fig4_ablation.pdf")
    fig.savefig(FIG_DIR / "fig4_ablation.png")
    plt.close(fig)

    print("\nablation (6 trials, 14 threads):")
    for grid in grids:
        for config in ["without", "with"]:
            times = [float(r["time"]) for r in rows if r["grid"] == grid and r["config"] == config]
            print(f"  {grid:10s} {config:8s} median={statistics.median(times):.3f}s  "
                  f"range=[{min(times):.3f}, {max(times):.3f}]")


# Figure 5: the architectural claim in its clearest form -- xfill's
# restart-portfolio keeps improving as threads are added well past the
# physical core count, while orca-solver's partition-based search does
# not scale the same way. One panel per grid so the comparison isn't
# averaged away.
def fig_thread_scaling():
    path = TESTBENCH_DIR / "results" / "thread_scaling.csv"
    if not path.exists():
        return
    with open(path) as f:
        rows = list(csv.DictReader(f))
    grids = sorted(set(r["grid"] for r in rows), key=lambda g: [r["grid"] for r in rows].index(g))

    fig, axes = plt.subplots(1, len(grids), figsize=(2.2 * len(grids), 2.8), sharey=False)
    if len(grids) == 1:
        axes = [axes]
    for ax, grid in zip(axes, grids):
        grows = [r for r in rows if r["grid"] == grid]
        grows.sort(key=lambda r: int(r["threads"]))
        threads = [int(r["threads"]) for r in grows]
        xt = [float(r["xfill_time"]) if r["xfill_status"] == "SOLVED" else None for r in grows]
        ot = [float(r["orca_time"]) if r["orca_status"] == "SOLVED" else None for r in grows]
        if any(v is not None for v in xt):
            ax.plot([t for t, v in zip(threads, xt) if v is not None],
                    [v for v in xt if v is not None],
                    "o-", color=SOLVER_COLOR["xfill"], label="xfill", linewidth=1.6, markersize=3)
        if any(v is not None for v in ot):
            ax.plot([t for t, v in zip(threads, ot) if v is not None],
                    [v for v in ot if v is not None],
                    "s-", color=SOLVER_COLOR["orca"], label="orca-solver", linewidth=1.6, markersize=3)
        ax.axvline(14, color="#cccccc", linewidth=1, linestyle="--", zorder=0)
        ax.set_title(grid, fontsize=9)
        ax.set_xlabel("threads")
    axes[0].set_ylabel("wall time (s)")
    axes[0].legend(fontsize=7, frameon=False, loc="upper right")
    fig.suptitle("Thread-count scaling: restart portfolio vs. partitioned search "
                  "(dashed line = physical core count)", fontsize=9)
    fig.tight_layout()
    fig.savefig(FIG_DIR / "fig5_thread_scaling.pdf")
    fig.savefig(FIG_DIR / "fig5_thread_scaling.png")
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
    fig_speedup(rows)
    fig_15x15_times(rows)
    fig_ablation_refined()
    fig_thread_scaling()
    print_summary_table(rows)
    print(f"\nfigures written to {FIG_DIR}")


if __name__ == "__main__":
    main()
