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
import threading
from collections.abc import Iterator

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


# The GUI runs one fill at a time (the frontend's own `filling` guard
# already prevents overlapping requests), so tracking a single in-flight
# subprocess here -- rather than per-job handles keyed by an id the
# frontend would have to generate, thread through, and clean up -- is
# sufficient and matches this app's single-user, stateless-otherwise
# design (see app.py's module docstring). The lock only protects the
# pointer itself; terminate()/poll() are safe to call concurrently with
# the subprocess's own lifecycle.
_current_process: subprocess.Popen | None = None
_current_process_lock = threading.Lock()


def cancel_current_fill() -> bool:
    """Terminates the currently-running xfill_cli subprocess, if any.
    Returns whether there actually was one running to cancel."""
    with _current_process_lock:
        proc = _current_process
    if proc is None or proc.poll() is not None:
        return False
    proc.terminate()
    return True


def solve_stream(
    puzzle: Puzzle,
    across_dict_path: str,
    across_min_score: int,
    down_dict_path: str,
    down_min_score: int,
    threads: int = 0,
    track_for_cancel: bool = True,
) -> Iterator[dict]:
    """Yields {"type": "progress", "nodes": N} dicts as xfill_cli reports
    them (see main.cpp's --progress), then exactly one final dict:
    {"type": "done", "solved": bool, "grid": [...] or None, ...stats} on a
    normal finish, {"type": "cancelled"} if cancel_current_fill() killed
    it, or {"type": "error", "message": str} if it exited abnormally on
    its own. Raises SolveError immediately (before yielding anything) only
    for problems that mean the solve was never actually attempted --
    xfill_cli missing, or a spawn failure.

    No automatic timeout: cancel_current_fill() is how a caller now bounds
    how long it's willing to wait, a deliberate replacement for what used
    to be a fixed timeout_seconds here -- with real cancellation and live
    progress both available, a silent timeout would just be a second,
    redundant way to give up that the user can no longer see coming or
    override.

    `track_for_cancel`: whether this call's subprocess is the one
    cancel_current_fill() (the Cancel button) can terminate. False for the
    background per-candidate feasibility checks in app.py's
    /api/options/verify -- those aren't user-cancellable individually (the
    frontend just stops *issuing* more of them once its batch goes stale,
    see updateOptionsPanel's doc comment), and must NOT be reachable by a
    Cancel click aimed at an unrelated, actually-running Fill, nor allowed
    to clobber that Fill's own tracked process out from under it.
    """
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

    proc: subprocess.Popen | None = None
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
            "--progress",
        ]
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1
            )
        except OSError as e:
            raise SolveError(f"could not start xfill_cli: {e}") from e

        if track_for_cancel:
            global _current_process
            with _current_process_lock:
                _current_process = proc

        final_result: dict | None = None
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue  # tolerate stray non-JSON noise rather than aborting the stream
            if obj.get("progress"):
                yield {"type": "progress", "nodes": obj.get("nodes", 0)}
            else:
                final_result = obj  # the terminal line has no "progress" key; keep the last one

        returncode = proc.wait()

        if final_result is not None:
            yield {"type": "done", **final_result}
        elif returncode < 0:
            # Killed by a signal (terminate()/kill() from
            # cancel_current_fill() send SIGTERM/-15, SIGKILL/-9) rather
            # than exiting on its own -- report as a cancellation, not an
            # error.
            yield {"type": "cancelled"}
        else:
            stderr_text = proc.stderr.read().strip() if proc.stderr else ""
            yield {
                "type": "error",
                "message": stderr_text or f"xfill_cli exited with code {returncode} and no output",
            }
    finally:
        if track_for_cancel:
            with _current_process_lock:
                if _current_process is proc:
                    _current_process = None
        pathlib.Path(grid_path).unlink(missing_ok=True)
        if across_is_temp:
            pathlib.Path(across_path).unlink(missing_ok=True)
        if down_is_temp:
            pathlib.Path(down_path).unlink(missing_ok=True)


def solve_blocking(
    puzzle: Puzzle,
    across_dict_path: str,
    across_min_score: int,
    down_dict_path: str,
    down_min_score: int,
    threads: int = 0,
    track_for_cancel: bool = True,
) -> dict:
    """Runs solve_stream to completion and returns just its final (non-
    "progress") event. For a caller that only wants the end result --
    e.g. /api/options/verify, which checks one candidate word at a time
    and has nothing to do with an intermediate node count."""
    final: dict | None = None
    for event in solve_stream(
        puzzle, across_dict_path, across_min_score, down_dict_path, down_min_score,
        threads=threads, track_for_cancel=track_for_cancel,
    ):
        if event["type"] != "progress":
            final = event
    return final if final is not None else {"type": "error", "message": "no result produced"}


def apply_solution(puzzle: Puzzle, result: dict) -> None:
    """Writes a solve()-result-shaped dict's "grid" rows back into
    `puzzle.letters` in place. No-op if the grid wasn't solved."""
    if not result.get("solved") or result.get("grid") is None:
        return
    for r, row in enumerate(result["grid"]):
        for c, ch in enumerate(row):
            if ch != "#":
                puzzle.letters[r][c] = ch
