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

import pathlib

from fastapi import FastAPI, HTTPException, UploadFile
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

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


@app.post("/api/puzzle/slots")
def puzzle_slots(puzzle: PuzzleModel):
    p = puzzle.to_puzzle()
    return {"slots": _slots_payload(p), "stats": p.stats()}


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
    resp = {"puzzle": PuzzleModel.from_puzzle(p), "slots": _slots_payload(p)}
    if ext == "cfp":
        resp["warning"] = cfp_format.CFP_UNVERIFIED_WARNING
    return resp


@app.post("/api/puzzle/export")
def export_puzzle(puzzle: PuzzleModel, format: str):
    writer = _WRITERS.get(format)
    if writer is None:
        raise HTTPException(400, f"unsupported format: {format} (expected puz, ipuz, or cfp)")
    p = puzzle.to_puzzle()
    data = writer(p)
    filename = f"{(p.title or 'puzzle').strip().replace(' ', '_')}.{format}"
    return Response(
        content=data,
        media_type=_MEDIA_TYPES[format],
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )


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
    dest.write_bytes(data)
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


# ---------------------------------------------------------------------------
# Fill (full solve)
# ---------------------------------------------------------------------------

class FillRequest(BaseModel):
    puzzle: PuzzleModel
    across_dict_path: str
    across_min_score: int = 0
    down_dict_path: str
    down_min_score: int = 0
    threads: int = 0


@app.post("/api/fill")
def fill(req: FillRequest):
    p = req.puzzle.to_puzzle()
    for path in (req.across_dict_path, req.down_dict_path):
        if not pathlib.Path(path).exists():
            raise HTTPException(400, f"dictionary not found: {path}")
    try:
        result = solver_bridge.solve(
            p,
            req.across_dict_path, req.across_min_score,
            req.down_dict_path, req.down_min_score,
            threads=req.threads,
        )
    except solver_bridge.SolveError as e:
        raise HTTPException(500, str(e)) from e
    solver_bridge.apply_solution(p, result)
    return {
        "solved": result.get("solved", False),
        "puzzle": PuzzleModel.from_puzzle(p),
        "stats": {
            k: result.get(k) for k in ("nodes", "backtracks", "restarts", "time_seconds", "threads")
        },
    }


# ---------------------------------------------------------------------------
# Static frontend
# ---------------------------------------------------------------------------

app.mount("/", StaticFiles(directory=str(FRONTEND_DIR), html=True), name="frontend")
