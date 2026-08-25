"""Subprocess bridge to the C++ xfill_cli solver.

The GUI's "options for this slot" panel uses dict_lookup.py directly (a
plain pattern match is all that needs); this module is only for the
"Fill" action, which needs the real CSP solver -- full constraint
propagation across every slot at once, not just one slot's pattern.
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile

from grid_model import Puzzle

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
XFILL_CLI = REPO_ROOT / "build" / "xfill_cli"


class SolveError(RuntimeError):
    pass


def solve(
    puzzle: Puzzle,
    across_dict_path: str,
    across_min_score: int,
    down_dict_path: str,
    down_min_score: int,
    threads: int = 0,
    timeout_seconds: float | None = 60.0,
) -> dict:
    if not XFILL_CLI.exists():
        raise SolveError(
            f"xfill_cli not found at {XFILL_CLI} -- build it first "
            "(cmake --build build --target xfill_cli)"
        )

    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(puzzle.to_grid_spec())
        grid_path = f.name

    try:
        cmd = [
            str(XFILL_CLI),
            grid_path,
            "--across-dict", across_dict_path,
            "--across-min", str(across_min_score),
            "--down-dict", down_dict_path,
            "--down-min", str(down_min_score),
            "--threads", str(threads),
            "--json",
        ]
        try:
            proc = subprocess.run(
                cmd, capture_output=True, text=True, timeout=timeout_seconds
            )
        except subprocess.TimeoutExpired as e:
            raise SolveError(f"solve timed out after {timeout_seconds}s") from e

        # xfill_cli writes its one JSON line to stdout; stderr carries
        # nothing in --json mode (see main.cpp), but guard anyway in case
        # a bad dictionary path throws before the JSON write.
        stdout = proc.stdout.strip()
        if not stdout:
            raise SolveError(proc.stderr.strip() or "xfill_cli produced no output")
        try:
            return json.loads(stdout.splitlines()[-1])
        except json.JSONDecodeError as e:
            raise SolveError(f"could not parse xfill_cli output: {stdout!r}") from e
    finally:
        pathlib.Path(grid_path).unlink(missing_ok=True)


def apply_solution(puzzle: Puzzle, result: dict) -> None:
    """Writes a solve() result's "grid" rows back into `puzzle.letters` in
    place. No-op if the grid wasn't solved."""
    if not result.get("solved") or result.get("grid") is None:
        return
    for r, row in enumerate(result["grid"]):
        for c, ch in enumerate(row):
            if ch != "#":
                puzzle.letters[r][c] = ch
