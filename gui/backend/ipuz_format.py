"""ipuz crossword format (JSON) -- reader and writer.

Schema verified against the public ipuz v2 spec (libipuz.org/ipuz-spec.html,
the canonical reference implementation's own spec page) rather than
reconstructed from memory:

  - "puzzle" grid: block cells are the literal string "#"; a numbered open
    cell is the plain integer clue number; an unnumbered open cell is 0.
  - "solution" grid: block cells are "#"; filled cells are the plain
    uppercase letter string, e.g. "A" -- or, for a rebus square, the whole
    multi-character answer, e.g. "STAR"; the spec allows a solution
    value of any length, no separate rebus section needed (unlike .puz).
  - "clues.Across" / "clues.Down": arrays of [number, "clue text"] pairs.
"""

from __future__ import annotations

import json

from grid_model import EMPTY, Puzzle

KIND = ["http://ipuz.org/crossword#1"]
VERSION = "http://ipuz.org/v2"


def to_ipuz_dict(puzzle: Puzzle) -> dict:
    slots = puzzle.compute_slots()
    number_at: dict[tuple[int, int], int] = {}
    for s in slots:
        number_at[(s.row, s.col)] = s.number

    puzzle_grid = []
    solution_grid = []
    for r in range(puzzle.height):
        prow, srow = [], []
        for c in range(puzzle.width):
            if puzzle.blocks[r][c]:
                prow.append("#")
                srow.append("#")
            else:
                prow.append(number_at.get((r, c), 0))
                ch = puzzle.letters[r][c]
                srow.append(ch if ch != EMPTY else "")
        puzzle_grid.append(prow)
        solution_grid.append(srow)

    clues_across = [[s.number, puzzle.clues.get(s.id, "")] for s in slots if s.direction == "across"]
    clues_down = [[s.number, puzzle.clues.get(s.id, "")] for s in slots if s.direction == "down"]

    doc = {
        "version": VERSION,
        "kind": KIND,
        "dimensions": {"width": puzzle.width, "height": puzzle.height},
        "puzzle": puzzle_grid,
        "solution": solution_grid,
        "clues": {"Across": clues_across, "Down": clues_down},
    }
    if puzzle.title:
        doc["title"] = puzzle.title
    if puzzle.author:
        doc["author"] = puzzle.author
    if puzzle.copyright:
        doc["copyright"] = puzzle.copyright
    if puzzle.notes:
        doc["notes"] = puzzle.notes
    return doc


def to_ipuz_bytes(puzzle: Puzzle) -> bytes:
    return json.dumps(to_ipuz_dict(puzzle), ensure_ascii=False, indent=2).encode("utf-8")


def from_ipuz_bytes(data: bytes) -> Puzzle:
    doc = json.loads(data.decode("utf-8"))
    dims = doc["dimensions"]
    width, height = int(dims["width"]), int(dims["height"])
    puzzle = Puzzle.blank(width, height)

    solution = doc.get("solution")
    grid = doc.get("puzzle")
    block_marker = doc.get("block", "#")

    for r in range(height):
        for c in range(width):
            src_row = solution[r] if solution is not None else grid[r]
            cell = src_row[c]
            if cell == block_marker or cell == "#":
                puzzle.blocks[r][c] = True
                continue
            if solution is not None:
                # Kept whole, not truncated to one character: the ipuz spec
                # allows a solution cell's value to be more than one
                # character for a rebus square, and this project's own
                # writer (to_ipuz_dict above) already round-trips one that
                # way -- truncating here would silently lose it on import.
                letter = cell if isinstance(cell, str) else ""
                if letter:
                    puzzle.letters[r][c] = letter.upper()
            # else: no solution section -- leave the cell open/unfilled;
            # numbering in `grid` doesn't tell us the answer letter.

    puzzle.title = doc.get("title", "")
    puzzle.author = doc.get("author", "")
    puzzle.copyright = doc.get("copyright", "")
    puzzle.notes = doc.get("notes", "")

    clues = doc.get("clues", {})
    for direction, key in (("across", "Across"), ("down", "Down")):
        for entry in clues.get(key, []):
            if isinstance(entry, list) and len(entry) >= 2:
                number, text = entry[0], entry[1]
            elif isinstance(entry, dict):
                number, text = entry.get("number"), entry.get("clue", "")
            else:
                continue
            slot_id = f"{'A' if direction == 'across' else 'D'}{number}"
            puzzle.clues[slot_id] = text

    return puzzle
