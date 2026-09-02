"""FastAPI backend for the xfill crossword construction GUI.

Serves the frontend (gui/frontend/) and a small JSON API for editing a
grid, importing/exporting .puz/.ipuz/.cfp, listing dictionaries, getting
candidate words for a slot, and running the xfill solver.

The frontend holds no server-side session -- every request carries the
full puzzle state (grid, letters, clues, metadata) and dictionary
selection; the server is a stateless translator to/from file formats and
the solver, not a database. Run it and it's all local: nothing here talks
to the network beyond localhost.
"""

from __future__ import annotations

import json
import pathlib
import re

import anyio.to_thread
from fastapi import FastAPI, HTTPException, UploadFile
from fastapi.responses import Response, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, model_validator

import cfp_format
import dict_lookup
import ipuz_format
import puz_format
import solver_bridge
from grid_model import EMPTY, Puzzle

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GUI_ROOT = pathlib.Path(__file__).resolve().parents[1]
FRONTEND_DIR = GUI_ROOT / "frontend"
DICT_DIR = GUI_ROOT / "dictionaries"
DICT_DIR.mkdir(parents=True, exist_ok=True)
DEFAULT_DICT = REPO_ROOT / "data" / "spreadthewordlist_caps.txt"
SAVES_DIR = GUI_ROOT / "saves"
SAVES_DIR.mkdir(parents=True, exist_ok=True)

app = FastAPI(title="xfill crossword GUI")


# ---------------------------------------------------------------------------
# Wire format: the frontend sends/receives the whole puzzle as plain JSON,
# not our internal dataclasses.
# ---------------------------------------------------------------------------

class PuzzleModel(BaseModel):
    width: int
    height: int
    blocks: list[list[bool]]
    letters: list[list[str]]
    title: str = ""
    author: str = ""
    copyright: str = ""
    notes: str = ""
    clues: dict[str, str] = {}

    @model_validator(mode="after")
    def _check_dimensions(self) -> "PuzzleModel":
        # Nothing else validated this before to_puzzle() handed a malformed
        # payload straight to Puzzle -- a client sending fewer rows/columns
        # than width/height claims (a truncated payload, a frontend bug)
        # would pass right through, and the first out-of-range access deep
        # inside grid_model.py (e.g. compute_slots()'s blocked[r][c]) would
        # raise a raw, unhandled IndexError -- a 500 with a confusing
        # traceback instead of a clear 400 pointing at the actual problem.
        if len(self.blocks) != self.height:
            raise ValueError(f"blocks has {len(self.blocks)} rows, expected height={self.height}")
        if len(self.letters) != self.height:
            raise ValueError(f"letters has {len(self.letters)} rows, expected height={self.height}")
        for r, row in enumerate(self.blocks):
            if len(row) != self.width:
                raise ValueError(f"blocks[{r}] has {len(row)} columns, expected width={self.width}")
        for r, row in enumerate(self.letters):
            if len(row) != self.width:
                raise ValueError(f"letters[{r}] has {len(row)} columns, expected width={self.width}")
        return self

    def to_puzzle(self) -> Puzzle:
        return Puzzle(
            width=self.width,
            height=self.height,
            blocks=[row[:] for row in self.blocks],
            letters=[[c if c else EMPTY for c in row] for row in self.letters],
            title=self.title,
            author=self.author,
            copyright=self.copyright,
            notes=self.notes,
            clues=dict(self.clues),
        )

    @staticmethod
    def from_puzzle(p: Puzzle) -> "PuzzleModel":
        return PuzzleModel(
            width=p.width,
            height=p.height,
            blocks=p.blocks,
            letters=p.letters,
            title=p.title,
            author=p.author,
            copyright=p.copyright,
            notes=p.notes,
            clues=p.clues,
        )


def _slots_payload(p: Puzzle) -> list[dict]:
    return [
        {
            "id": s.id,
            "number": s.number,
            "direction": s.direction,
            "row": s.row,
            "col": s.col,
            "length": s.length,
            "cells": [[r, c] for r, c in s.cells],
            "clue": p.clues.get(s.id, ""),
            "pattern": p.slot_pattern(s),
        }
        for s in p.compute_slots()
    ]


@app.post("/api/puzzle/new")
def new_puzzle(width: int, height: int):
    if not (1 <= width <= 50 and 1 <= height <= 50):
        raise HTTPException(400, "width/height must be between 1 and 50")
    p = Puzzle.blank(width, height)
    return {"puzzle": PuzzleModel.from_puzzle(p), "slots": _slots_payload(p)}


def _slot_word(p: Puzzle, s) -> str | None:
    """This slot's word if every one of its cells is filled in, else
    None -- a partially- or un-filled slot has no score to look up. A
    rebus cell contributes its full content (e.g. "AD"), same as
    slot_pattern() -- the word this returns is exactly what a rebus-aware
    dictionary lookup (or a human) would read the slot as spelling out,
    e.g. "ADAPTS" for a 5-cell slot whose first cell holds "AD", not a
    5-character string that's missing a letter."""
    letters = [p.letters[r][c] for r, c in s.cells]
    if any(ch == EMPTY for ch in letters):
        return None
    return "".join(letters)


@app.post("/api/puzzle/slots")
def puzzle_slots(
    puzzle: PuzzleModel,
    across_dict_path: str | None = None,
    down_dict_path: str | None = None,
):
    """`across_dict_path`/`down_dict_path` are optional: when given (the
    frontend always passes its current dictionary selections), each fully-
    filled slot's entry in the response gets a "score" field -- that
    word's score in the relevant direction's dictionary (unfiltered by
    whatever min-score threshold is currently selected there, since this
    reports the word's real score, not whether it'd currently pass a
    filter). A slot that isn't fully filled, or whose direction has no
    dictionary selected at all, gets no "score" key -- there's nothing
    meaningful to report yet. A slot that IS fully filled but whose word
    isn't in that dictionary gets "score": null (JSON), distinguishable
    from the key being absent -- the frontend's Clues tab shows that as
    "(N/A)" rather than silently showing nothing, same as a real score
    but flagging it as unrecognized instead of scored. `stats` gets a
    parallel "avg_word_score": the mean of every REAL score found this
    way (N/A entries don't count, same as an unfilled slot), or None if
    none were.
    """
    p = puzzle.to_puzzle()
    word_lists: dict[str, dict_lookup.WordList] = {}
    for direction, path in (("across", across_dict_path), ("down", down_dict_path)):
        if path and pathlib.Path(path).exists():
            word_lists[direction] = dict_lookup.get_word_list(path, 0)

    slots = _slots_payload(p)
    total_score = 0
    scored_count = 0
    for entry, s in zip(slots, p.compute_slots()):
        word_list = word_lists.get(s.direction)
        if word_list is None:
            continue
        word = _slot_word(p, s)
        if word is None:
            continue
        score = word_list.score_of(word)
        entry["score"] = score  # a real int if found, else None -> JSON null ("N/A")
        if score is not None:
            total_score += score
            scored_count += 1

    stats = p.stats()
    stats["avg_word_score"] = round(total_score / scored_count, 1) if scored_count else None
    return {"slots": slots, "stats": stats}


# ---------------------------------------------------------------------------
# Import / export
# ---------------------------------------------------------------------------

_READERS = {"puz": puz_format.from_puz_bytes, "ipuz": ipuz_format.from_ipuz_bytes, "cfp": cfp_format.from_cfp_bytes}
_WRITERS = {"puz": puz_format.to_puz_bytes, "ipuz": ipuz_format.to_ipuz_bytes, "cfp": cfp_format.to_cfp_bytes}
_MEDIA_TYPES = {"puz": "application/octet-stream", "ipuz": "application/json", "cfp": "application/xml"}


@app.post("/api/puzzle/import")
async def import_puzzle(file: UploadFile):
    ext = (file.filename or "").rsplit(".", 1)[-1].lower()
    reader = _READERS.get(ext)
    if reader is None:
        raise HTTPException(400, f"unsupported extension: .{ext} (expected .puz, .ipuz, or .cfp)")
    data = await file.read()
    try:
        p = reader(data)
    except Exception as e:
        raise HTTPException(400, f"could not parse {file.filename}: {e}") from e
    return {"puzzle": PuzzleModel.from_puzzle(p), "slots": _slots_payload(p)}


def _safe_filename_stem(name: str, fallback: str) -> str:
    """Arbitrary user text (a puzzle title, a save name) is not safe to
    embed as-is in a filename or a Content-Disposition header -- a '"'
    would break the header's quoting, CR/LF could inject additional
    headers, and '/' or '..' could escape the intended directory entirely
    (a real concern for the save/load endpoints below, which use this to
    build a path on disk from a name the client fully controls). Stripping
    down to a conservative safe set avoids all of that at once, while
    still keeping the name recognizable."""
    base = re.sub(r"[^A-Za-z0-9 _-]", "", name.strip())
    base = re.sub(r"\s+", "_", base).strip("_")
    return base or fallback


def _safe_download_filename(title: str, ext: str) -> str:
    return f"{_safe_filename_stem(title, 'puzzle')}.{ext}"


@app.post("/api/puzzle/export")
def export_puzzle(puzzle: PuzzleModel, format: str):
    writer = _WRITERS.get(format)
    if writer is None:
        raise HTTPException(400, f"unsupported format: {format} (expected puz, ipuz, or cfp)")
    p = puzzle.to_puzzle()
    data = writer(p)
    filename = _safe_download_filename(p.title, format)
    return Response(
        content=data,
        media_type=_MEDIA_TYPES[format],
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )


# ---------------------------------------------------------------------------
# Save / Load -- an in-app named save slot, distinct from Import/Export:
# those round-trip through real crossword-editor formats (.puz/.ipuz/.cfp)
# for interop with other tools; this is just "remember this puzzle under a
# name I pick, on this machine, so clicking Save again later doesn't need a
# file picker" (the frontend prompts for a name once, then reuses it -- see
# app.js's saveToServer). Stored as plain PuzzleModel JSON, one file per
# save, under SAVES_DIR.
# ---------------------------------------------------------------------------

class SaveRequest(BaseModel):
    puzzle: PuzzleModel
    name: str


class LoadRequest(BaseModel):
    name: str


def _save_path(name: str) -> pathlib.Path:
    # _safe_filename_stem strips '/' and '..' along with everything else
    # unsafe, so the result can't escape SAVES_DIR regardless of what the
    # client sends as `name`.
    return SAVES_DIR / f"{_safe_filename_stem(name, 'untitled')}.json"


@app.get("/api/puzzle/saves")
def list_saves():
    return {"saves": sorted(p.stem for p in SAVES_DIR.glob("*.json"))}


@app.post("/api/puzzle/save")
def save_puzzle(req: SaveRequest):
    path = _save_path(req.name)
    path.write_text(json.dumps(req.puzzle.model_dump()), encoding="utf-8")
    return {"name": path.stem}


@app.post("/api/puzzle/load")
def load_puzzle(req: LoadRequest):
    path = _save_path(req.name)
    if not path.exists():
        raise HTTPException(404, f"no save named {req.name!r}")
    p = PuzzleModel(**json.loads(path.read_text(encoding="utf-8"))).to_puzzle()
    return {"name": path.stem, "puzzle": PuzzleModel.from_puzzle(p), "slots": _slots_payload(p)}


@app.delete("/api/puzzle/saves")
def delete_save(name: str):
    path = _save_path(name)
    if path.exists():
        path.unlink()
    return {"deleted": True}


# ---------------------------------------------------------------------------
# Dictionaries
# ---------------------------------------------------------------------------

@app.get("/api/dictionaries")
def list_dictionaries():
    dicts = []
    if DEFAULT_DICT.exists():
        dicts.append({"id": "default", "name": "spreadthewordlist (built-in)", "path": str(DEFAULT_DICT)})
    for f in sorted(DICT_DIR.glob("*")):
        if f.is_file():
            dicts.append({"id": f.name, "name": f.name, "path": str(f)})
    return {"dictionaries": dicts}


@app.post("/api/dictionaries/upload")
async def upload_dictionary(file: UploadFile):
    dest = DICT_DIR / pathlib.Path(file.filename).name
    data = await file.read()
    # dest.write_bytes is a blocking, synchronous disk write -- calling it
    # directly in this async handler would stall the single event loop
    # thread for its whole duration, freezing every OTHER concurrent
    # request (in-flight /api/fill progress streaming included) until it
    # finishes. A large dictionary upload makes this a real, reproducible
    # stall, not just a theoretical one.
    await anyio.to_thread.run_sync(dest.write_bytes, data)
    # Re-uploading an existing filename overwrites it on disk -- without
    # this, /api/options would keep serving dict_lookup's cached,
    # pre-overwrite word list for this path indefinitely (see
    # dict_lookup.invalidate's docstring).
    dict_lookup.invalidate(str(dest))
    return {"id": dest.name, "name": dest.name, "path": str(dest)}


class OptionsRequest(BaseModel):
    pattern: str  # letters, '?'/'-' for blank
    dict_path: str
    min_score: int = 0
    limit: int = 50


@app.post("/api/options")
def slot_options(req: OptionsRequest):
    if not pathlib.Path(req.dict_path).exists():
        raise HTTPException(400, f"dictionary not found: {req.dict_path}")
    pattern = req.pattern.upper().replace("-", "?").replace(EMPTY, "?")
    wl = dict_lookup.get_word_list(req.dict_path, req.min_score)
    candidates = wl.candidates(pattern, limit=req.limit)
    return {"candidates": [{"word": w, "score": s} for w, s in candidates]}


class VerifyOptionRequest(BaseModel):
    puzzle: PuzzleModel
    slot_id: str
    word: str
    across_dict_path: str
    across_min_score: int = 0
    down_dict_path: str
    down_min_score: int = 0
    across_min_overrides: dict[int, int] = {}
    down_min_overrides: dict[int, int] = {}
    threads: int = 2


@app.post("/api/options/verify")
def verify_option(req: VerifyOptionRequest):
    """Checks whether locking `word` into slot `slot_id` still allows the
    rest of the grid to be completed -- a real solve, not the plain
    pattern match slot_options above does. Used by the frontend to tell a
    candidate that's merely dictionary-valid apart from one actually known
    to lead to a complete fill; expensive (one full solve per call), so
    the frontend only calls this for a handful of candidates at a time,
    sequentially, in the background.
    """
    p = req.puzzle.to_puzzle()
    for path in (req.across_dict_path, req.down_dict_path):
        if not pathlib.Path(path).exists():
            raise HTTPException(400, f"dictionary not found: {path}")

    slot = next((s for s in p.compute_slots() if s.id == req.slot_id), None)
    if slot is None:
        raise HTTPException(400, f"no such slot: {req.slot_id}")
    word = req.word.strip().upper()
    # Not `len(word) != slot.length` (a naive 1-char-per-cell assumption,
    # broken by any rebus cell -- a slot's spelled-out word can be LONGER
    # than its physical cell count) -- slice_word_for_slot already raises
    # a clear error if `word` doesn't add up to what the cells (rebus or
    # not) actually expect.
    try:
        chunks = p.slice_word_for_slot(slot, word)
    except ValueError as e:
        raise HTTPException(400, str(e)) from e

    for (r, c), chunk in zip(slot.cells, chunks):
        p.letters[r][c] = chunk

    result = solver_bridge.solve_blocking(
        p,
        req.across_dict_path, req.across_min_score,
        req.down_dict_path, req.down_min_score,
        threads=req.threads,
        kind="verify",
        across_min_overrides=req.across_min_overrides,
        down_min_overrides=req.down_min_overrides,
        # A bound the automatic verification batch can't itself request
        # cancellation for individually (see solve_stream's docstring) --
        # the frontend proactively cancels a whole stale batch via
        # /api/options/verify/cancel-all, but this is the backstop for
        # whenever that doesn't happen (a closed tab, a crashed page, ...).
        timeout_seconds=20.0,
    )
    if result.get("type") == "done":
        if result.get("solved"):
            return {"feasible": True, "grid": result.get("grid")}
        return {"feasible": False, "grid": None}
    # An "error" (xfill_cli missing, crashed, timed out, etc.) is not the
    # same finding as a genuine infeasibility -- don't let a transient
    # problem here get treated as "this word doesn't work" and silently
    # removed from the list; the frontend leaves it unchecked instead.
    return {"feasible": None, "grid": None, "error": result.get("message", "verify failed")}


@app.post("/api/options/verify/cancel-all")
def verify_cancel_all():
    """Terminates every currently-running verify-check subprocess. Called
    by the frontend right before starting a fresh verification batch (a
    newly-selected slot supersedes whatever the previous one was checking)
    and right before a real Fill starts -- see solve_stream's docstring
    for why this exists: without it, a batch's already-in-flight (already
    spawned) check has no way to be stopped once superseded, and multiple
    such orphaned solves were confirmed piling up and running indefinitely
    with the app not even open."""
    return {"killed": solver_bridge.cancel_all_verify_checks()}


# ---------------------------------------------------------------------------
# Fill (full solve)
# ---------------------------------------------------------------------------

class FillRequest(BaseModel):
    puzzle: PuzzleModel
    across_dict_path: str
    across_min_score: int = 0
    down_dict_path: str
    down_min_score: int = 0
    across_min_overrides: dict[int, int] = {}
    down_min_overrides: dict[int, int] = {}
    threads: int = 0
    maximize: bool = False


@app.post("/api/fill")
def fill(req: FillRequest):
    """Streams newline-delimited JSON: zero or more
    {"type":"progress","nodes":N} lines while solving, plus (only when
    `maximize` is set) zero or more {"type":"improved","score":N,
    "puzzle":{...}} lines, one each time the search finds a complete fill
    scoring higher than any found so far (see solver_bridge.solve_stream's
    docstring) -- then exactly one final line: {"type":"done",...},
    {"type":"cancelled"} (see POST /api/fill/cancel, which also cancels a
    maximize search in progress), or {"type":"error","message":...}. With
    `maximize` unset (the default), behavior is unchanged from before this
    mode existed: exactly one {"type":"done",...} after zero or more
    progress lines, no "improved" lines.

    A plain non-streaming JSON response (what this returned before) can't
    carry live progress at all -- it isn't sent until the whole request
    finishes -- so a client that wants progress updates has to read this
    response body incrementally (e.g. the Fetch API's
    response.body.getReader(), which the frontend uses) rather than
    awaiting a parsed JSON body in one shot.
    """
    p = req.puzzle.to_puzzle()
    for path in (req.across_dict_path, req.down_dict_path):
        if not pathlib.Path(path).exists():
            raise HTTPException(400, f"dictionary not found: {path}")

    def generate():
        try:
            for event in solver_bridge.solve_stream(
                p,
                req.across_dict_path, req.across_min_score,
                req.down_dict_path, req.down_min_score,
                threads=req.threads,
                maximize=req.maximize,
                across_min_overrides=req.across_min_overrides,
                down_min_overrides=req.down_min_overrides,
            ):
                if event["type"] == "improved":
                    # Not a real Solution-shaped dict (no "solved" key --
                    # WriteJsonImprovement in main.cpp only ever emits
                    # this once a complete fill exists), so shape one
                    # here rather than teaching apply_solution a second
                    # input shape.
                    solver_bridge.apply_solution(p, {"solved": True, "grid": event.get("grid")})
                    payload = {
                        "type": "improved",
                        "score": event.get("score"),
                        "puzzle": PuzzleModel.from_puzzle(p).model_dump(),
                    }
                elif event["type"] == "done":
                    solver_bridge.apply_solution(p, event)
                    payload = {
                        "type": "done",
                        "solved": event.get("solved", False),
                        "puzzle": PuzzleModel.from_puzzle(p).model_dump(),
                        "stats": {
                            k: event.get(k)
                            for k in ("nodes", "backtracks", "restarts", "time_seconds", "threads")
                        },
                        # [row, col] pairs whose letter had no real
                        # alternative -- see main.cpp's ForcedCells. Only
                        # ever populated by the default (non-maximize)
                        # search; absent (defaults to []) in maximize mode,
                        # where "forced" isn't a meaningful concept.
                        "forced_cells": event.get("forced_cells", []),
                    }
                    if req.maximize:
                        payload["stats"]["score"] = event.get("score")
                else:
                    payload = event
                yield json.dumps(payload) + "\n"
        except solver_bridge.SolveError as e:
            yield json.dumps({"type": "error", "message": str(e)}) + "\n"

    return StreamingResponse(generate(), media_type="application/x-ndjson")


@app.post("/api/fill/cancel")
def fill_cancel():
    return {"cancelled": solver_bridge.cancel_current_fill()}


# ---------------------------------------------------------------------------
# Static frontend
# ---------------------------------------------------------------------------

app.mount("/", StaticFiles(directory=str(FRONTEND_DIR), html=True), name="frontend")
