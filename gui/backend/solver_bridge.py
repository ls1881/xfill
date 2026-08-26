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


# Score injected for a user-typed, already-complete entry that isn't in
# the chosen dictionary (see _locked_words_by_direction/_augment_dict
# below) -- comfortably above the score UI's max="100" input, so it always
# clears whatever min_score threshold is set.
_LOCKED_WORD_SCORE = 999


def _locked_words_by_direction(puzzle: Puzzle) -> dict[str, set[str]]:
    """Every already-fully-typed slot's word, grouped by direction. A slot
    counts as "fully typed" only if every one of its cells already has a
    letter -- a partially-filled slot is left alone and still has to match
    a real dictionary word, same as before."""
    words: dict[str, set[str]] = {"across": set(), "down": set()}
    for slot in puzzle.compute_slots():
        letters = [puzzle.letters[r][c] for r, c in slot.cells]
        if all(ch != "-" for ch in letters):
            words[slot.direction].add("".join(letters))
    return words


def _augment_dict(original_path: str, locked_words: set[str], min_score: int) -> tuple[str, bool]:
    """Returns a path to solve with for this direction, plus whether it's a
    temp file the caller must clean up. If there's nothing to inject, this
    is just `original_path` unchanged -- the common case, so no extra I/O
    or temp file when nothing on the grid needs it."""
    if not locked_words:
        return original_path, False
    with open(original_path, encoding="utf-8", errors="replace") as f:
        original_content = f.read()
    # A word already present is only "already fine" if it would actually
    # clear the chosen min_score -- present-but-below-threshold is exactly
    # the case a low-scored real word typed into the grid needs this same
    # override for, not just an absent one.
    already_included = set()
    for line in original_content.splitlines():
        if not line.strip():
            continue
        word, _, score_s = line.partition(";")
        try:
            score = int(score_s)
        except ValueError:
            score = 0
        if score >= min_score:
            already_included.add(word.strip().upper())
    to_add = locked_words - already_included
    if not to_add:
        return original_path, False
    with tempfile.NamedTemporaryFile(
        "w", suffix=".dict", delete=False, encoding="utf-8"
    ) as f:
        f.write(original_content)
        if original_content and not original_content.endswith("\n"):
            f.write("\n")
        for word in sorted(to_add):
            f.write(f"{word};{_LOCKED_WORD_SCORE}\n")
        return f.name, True


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

    # A slot the user has already completely typed out shouldn't be
    # rejected just because that exact word isn't in the chosen
    # dictionary -- it's a given, not something the solver is choosing,
    # so it's injected into a per-direction working copy of that
    # dictionary (at a score that always clears the min_score filter)
    # rather than taught to the C++ engine as a new "trust this input"
    # concept. The grid spec's existing per-cell letter constraints (see
    # to_grid_spec) then pin the slot to exactly this word during solving,
    # the same mechanism a partially-filled slot already relies on.
    locked = _locked_words_by_direction(puzzle)
    across_path, across_is_temp = _augment_dict(across_dict_path, locked["across"], across_min_score)
    down_path, down_is_temp = _augment_dict(down_dict_path, locked["down"], down_min_score)

    try:
        cmd = [
            str(XFILL_CLI),
            grid_path,
            "--across-dict", across_path,
            "--across-min", str(across_min_score),
            "--down-dict", down_path,
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
        if across_is_temp:
            pathlib.Path(across_path).unlink(missing_ok=True)
        if down_is_temp:
            pathlib.Path(down_path).unlink(missing_ok=True)


def apply_solution(puzzle: Puzzle, result: dict) -> None:
    """Writes a solve() result's "grid" rows back into `puzzle.letters` in
    place. No-op if the grid wasn't solved."""
    if not result.get("solved") or result.get("grid") is None:
        return
    for r, row in enumerate(result["grid"]):
        for c, ch in enumerate(row):
            if ch != "#":
                puzzle.letters[r][c] = ch
