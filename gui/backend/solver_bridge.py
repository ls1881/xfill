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
from collections.abc import Callable, Iterator

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
    a real dictionary word, same as before. Built from the raw cell
    content (`"".join(letters)`), which already naturally produces a
    rebus slot's real, full spelled-out word ("AD"+"A"+"P"+"T"+"S" =
    "ADAPTS") via plain string concatenation -- no is_rebus special-casing
    needed. This relies on the C++ solver itself understanding a rebus
    cell's real content (see to_grid_spec's trailing rebus section and
    xfill's Grid::RebusContent/Slot::cell_lengths): it now searches a
    rebus-containing slot at its true expanded length, so the correct
    word to lock is the real one, not solving_letter()'s single-character
    stand-in -- locking the stand-in (e.g. "AAPTS") would target a word
    length the solver isn't searching for anymore."""
    words: dict[str, set[str]] = {"across": set(), "down": set()}
    for slot in puzzle.compute_slots():
        letters = [puzzle.letters[r][c] for r, c in slot.cells]
        if all(ch != "-" for ch in letters):
            words[slot.direction].add("".join(letters))
    return words


def _format_min_overrides(overrides: dict[int, int]) -> str:
    """"<length>:<score>,<length>:<score>,..." -- the wire format
    main.cpp's --across-min-overrides/--down-min-overrides parse (see
    ParseLengthScoreMap in main.cpp)."""
    return ",".join(f"{length}:{score}" for length, score in overrides.items())


def _min_score_resolver(default_min_score: int, overrides: dict[int, int] | None) -> Callable[[int], int]:
    """`overrides` (length -> min score) takes priority over
    `default_min_score` for the lengths it lists, same rule the C++ engine
    applies (see MinScoreByLength::For in dictionary.hpp)."""
    overrides = overrides or {}
    return lambda length: overrides.get(length, default_min_score)


def _augment_dict(
    original_path: str, locked_words: set[str], min_score_for: Callable[[int], int]
) -> tuple[str, bool]:
    """Returns a path to solve with for this direction, plus whether it's a
    temp file the caller must clean up. If there's nothing to inject, this
    is just `original_path` unchanged -- the common case, so no extra I/O
    or temp file when nothing on the grid needs it.

    `min_score_for(length)`: since `locked_words` can span multiple word
    lengths, and per-length min-score overrides mean different lengths can
    have different thresholds, "already clears the threshold" has to be
    checked per word, at that word's own length -- there's no single
    scalar that's correct for every locked word at once.
    """
    if not locked_words:
        return original_path, False
    with open(original_path, encoding="utf-8", errors="replace") as f:
        original_content = f.read()
    # A word already present is only "already fine" if it would actually
    # clear its length's min_score -- present-but-below-threshold is
    # exactly the case a low-scored real word typed into the grid needs
    # this same override for, not just an absent one.
    already_included = set()
    for line in original_content.splitlines():
        if not line.strip():
            continue
        word, _, score_s = line.partition(";")
        try:
            score = int(score_s)
        except ValueError:
            score = 0
        word = word.strip().upper()
        if score >= min_score_for(len(word)):
            already_included.add(word)
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


# The GUI runs one *Fill* at a time (the frontend's own `filling` guard
# prevents overlapping Fill requests), but verify-checks (see
# /api/options/verify) are a different story: updateOptionsPanel starts a
# fresh background batch every time the selected slot's pattern changes,
# and that batch's first request is already in flight -- its subprocess
# already spawned -- by the time a newer batch supersedes it, since JS
# staleness checks only stop a batch from issuing its *next* request, not
# reach into one already sent. A user clicking through several slots
# faster than one verify solve completes was leaving each of those
# now-nobody-cares-about subprocesses running to completion on its own,
# unbounded, with nothing tracking or able to stop it -- multiple
# xfill_cli processes still burning CPU with the app not even open,
# confirmed directly. So verify subprocesses ARE tracked too, in their own
# set (never the same slot as _current_process, so a Fill's Cancel button
# can't reach a verify-check and vice versa), and every new batch cancels
# every previously-tracked one before issuing its own first request (see
# app.py's /api/options/verify/cancel-all and app.js's
# startVerificationBatch). A bounded timeout (see solve_stream's
# `timeout_seconds`) is the second, independent backstop for this same
# failure mode -- kept even with the cancel-all fix in place, since that
# fix depends on the frontend actually running and calling it (a closed
# tab, a crashed page, or some other gap the frontend can't reach still
# shouldn't leave a solve running forever).
_current_process: subprocess.Popen | None = None
_current_process_lock = threading.Lock()
_verify_processes: set[subprocess.Popen] = set()
_verify_processes_lock = threading.Lock()


def cancel_current_fill() -> bool:
    """Terminates the currently-running Fill's xfill_cli subprocess, if
    any. Returns whether there actually was one running to cancel."""
    with _current_process_lock:
        proc = _current_process
    if proc is None or proc.poll() is not None:
        return False
    proc.terminate()
    return True


def cancel_all_verify_checks() -> int:
    """Terminates every currently-tracked verify-check subprocess (never
    touches a Fill's). Returns how many were actually still running."""
    with _verify_processes_lock:
        procs = list(_verify_processes)
    killed = 0
    for proc in procs:
        if proc.poll() is None:
            proc.terminate()
            killed += 1
    return killed


def solve_stream(
    puzzle: Puzzle,
    across_dict_path: str,
    across_min_score: int,
    down_dict_path: str,
    down_min_score: int,
    threads: int = 0,
    kind: str = "fill",
    timeout_seconds: float | None = None,
    maximize: bool = False,
    across_min_overrides: dict[int, int] | None = None,
    down_min_overrides: dict[int, int] | None = None,
) -> Iterator[dict]:
    """Yields {"type": "progress", "nodes": N} dicts as xfill_cli reports
    them (see main.cpp's --progress), then exactly one final dict:
    {"type": "done", "solved": bool, "grid": [...] or None, ...stats} on a
    normal finish, {"type": "cancelled"} if this call's subprocess was
    terminated (by cancel_current_fill(), cancel_all_verify_checks(), or
    timeout_seconds elapsing) rather than exiting on its own, or
    {"type": "error", "message": str} if it exited abnormally on its own.
    Raises SolveError immediately (before yielding anything) only for
    problems that mean the solve was never actually attempted -- xfill_cli
    missing, or a spawn failure.

    `maximize`: runs xfill_cli's separate --maximize branch-and-bound
    search instead of the default first-solution search (see main.cpp's
    --maximize and Solver::MaximizeScoreParallel's doc comment). This is
    an anytime search -- every time it finds a complete fill scoring
    higher than any found so far, it's reported immediately as a
    {"type": "improved", "score": N, "grid": [...]} event (zero or more
    of these precede the final "done"), rather than waiting for the
    single first-found result the default search yields. The final
    "done" event's "grid"/"score" are the best fill found by the time the
    search stopped -- either because it proved optimality on its own or
    because the caller cancelled it (same cancel_current_fill() as the
    default search; there's deliberately no separate cancel path for
    this mode) -- not necessarily a proven global optimum.

    `across_min_overrides`/`down_min_overrides`: length -> min score,
    for word lengths that need a different threshold than
    across_min_score/down_min_score's default (see main.cpp's
    --across-min-overrides/--down-min-overrides and
    MinScoreByLength in dictionary.hpp). A length not listed here still
    uses the direction's plain min_score.

    `kind`: which tracked-process set this call's subprocess belongs to --
    "fill" (the default) is cancel_current_fill()'s (the Cancel button);
    "verify" is cancel_all_verify_checks()'s, used for the background
    per-candidate feasibility checks in app.py's /api/options/verify. Kept
    as two disjoint sets so a Cancel click aimed at an actual Fill can't
    reach an unrelated verify-check, and starting a fresh verify batch
    can't clobber a real Fill's own tracked process.

    `timeout_seconds`: an upper bound after which this call's subprocess
    is terminated on its own, reported the same way an explicit cancel is.
    None (the default, used for a real Fill) means no bound -- the Cancel
    button is the only thing that can end a deliberate, user-initiated
    Fill, since a silent timeout would just be a second, redundant way to
    give up that the user can no longer see coming or override. Verify
    checks pass a real bound: they're automatic, not something the user
    explicitly asked *this one* to run, and rely on the caller
    (app.js's startVerificationBatch) proactively cancelling anything
    stale as the primary defense -- but that defense only works while the
    frontend that's supposed to call it is actually running; this is the
    backstop for when it isn't (a closed tab, a crashed page, or anything
    else that gap doesn't cover), confirmed necessary directly: multiple
    xfill_cli processes were found still running with the app not even
    open, from exactly this gap before both fixes existed.
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
    across_min_for = _min_score_resolver(across_min_score, across_min_overrides)
    down_min_for = _min_score_resolver(down_min_score, down_min_overrides)
    across_path, across_is_temp = _augment_dict(across_dict_path, locked["across"], across_min_for)
    down_path, down_is_temp = _augment_dict(down_dict_path, locked["down"], down_min_for)

    proc: subprocess.Popen | None = None
    watchdog: threading.Timer | None = None
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
        if across_min_overrides:
            cmd += ["--across-min-overrides", _format_min_overrides(across_min_overrides)]
        if down_min_overrides:
            cmd += ["--down-min-overrides", _format_min_overrides(down_min_overrides)]
        if maximize:
            cmd.append("--maximize")
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1
            )
        except OSError as e:
            raise SolveError(f"could not start xfill_cli: {e}") from e

        if kind == "fill":
            global _current_process
            with _current_process_lock:
                _current_process = proc
        else:
            with _verify_processes_lock:
                _verify_processes.add(proc)

        if timeout_seconds is not None:
            watchdog = threading.Timer(timeout_seconds, proc.terminate)
            watchdog.daemon = True
            watchdog.start()

        # Drained concurrently on its own thread, not read after proc.wait()
        # the way this used to work: stdout and stderr are two independent
        # OS pipes, each with a bounded buffer (~64KB). The loop below only
        # ever reads stdout; if xfill_cli wrote enough to stderr while still
        # running (e.g. many "nogood depth=..." lines under
        # XFILL_DEBUG_NOGOODS) to fill that pipe's buffer, the child would
        # block on its own stderr write, which stalls its stdout too -- and
        # the parent, still stuck in the stdout loop below, would never
        # reach the old post-wait() stderr read that could unblock it: a
        # genuine two-pipe deadlock, not just a theoretical one. Draining
        # stderr on a separate thread from the moment the process starts
        # means that pipe can never back up regardless of what else this
        # generator is doing.
        stderr_lines: list[str] = []

        def _drain_stderr() -> None:
            assert proc is not None and proc.stderr is not None
            for line in proc.stderr:
                stderr_lines.append(line)

        stderr_thread = threading.Thread(target=_drain_stderr, daemon=True)
        stderr_thread.start()

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
            elif obj.get("type") == "improved":
                yield {"type": "improved", "score": obj.get("score"), "grid": obj.get("grid")}
            else:
                final_result = obj  # the terminal line has no "progress" key; keep the last one

        returncode = proc.wait()
        stderr_thread.join()
        if watchdog is not None:
            watchdog.cancel()

        if final_result is not None:
            yield {"type": "done", **final_result}
        elif returncode < 0:
            # Killed by a signal (terminate()/kill() from
            # cancel_current_fill(), cancel_all_verify_checks(), or the
            # watchdog above send SIGTERM/-15, SIGKILL/-9) rather than
            # exiting on its own -- report as a cancellation, not an
            # error.
            yield {"type": "cancelled"}
        else:
            stderr_text = "".join(stderr_lines).strip()
            yield {
                "type": "error",
                "message": stderr_text or f"xfill_cli exited with code {returncode} and no output",
            }
    finally:
        if watchdog is not None:
            watchdog.cancel()
        if kind == "fill":
            with _current_process_lock:
                if _current_process is proc:
                    _current_process = None
        elif proc is not None:
            with _verify_processes_lock:
                _verify_processes.discard(proc)
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
    kind: str = "fill",
    timeout_seconds: float | None = None,
    maximize: bool = False,
    across_min_overrides: dict[int, int] | None = None,
    down_min_overrides: dict[int, int] | None = None,
) -> dict:
    """Runs solve_stream to completion and returns just its final "done"/
    "cancelled"/"error" event, discarding "progress" and (if `maximize`)
    "improved" events along the way. For a caller that only wants the end
    result -- e.g. /api/options/verify, which checks one candidate word at
    a time and has nothing to do with intermediate updates."""
    final: dict | None = None
    for event in solve_stream(
        puzzle, across_dict_path, across_min_score, down_dict_path, down_min_score,
        threads=threads, kind=kind, timeout_seconds=timeout_seconds, maximize=maximize,
        across_min_overrides=across_min_overrides, down_min_overrides=down_min_overrides,
    ):
        if event["type"] not in ("progress", "improved"):
            final = event
    return final if final is not None else {"type": "error", "message": "no result produced"}


def apply_solution(puzzle: Puzzle, result: dict) -> None:
    """Writes a solve()-result-shaped dict's "grid" rows back into
    `puzzle.letters` in place. No-op if the grid wasn't solved.

    Skips any cell that's currently a rebus square (Puzzle.is_rebus): the
    solver only ever sees and echoes back that cell's single-character
    solving_letter() (see to_grid_spec), which is correct as a crossing
    constraint but is not the real answer -- writing it back here would
    silently collapse "STAR" down to just "S"."""
    if not result.get("solved") or result.get("grid") is None:
        return
    for r, row in enumerate(result["grid"]):
        for c, ch in enumerate(row):
            if ch != "#" and not puzzle.is_rebus(r, c):
                puzzle.letters[r][c] = ch
