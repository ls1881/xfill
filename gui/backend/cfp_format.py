"""CrossFire's .cfp format -- BEST-EFFORT ONLY, NOT VERIFIED.

Unlike .puz (verified byte-for-byte against alexdej/puzpy) and .ipuz
(verified against the public ipuz v2 spec and the independent `ipuz`
package), no authoritative specification or reference implementation for
CrossFire's own .cfp format was available while writing this. Public
sources describe it only as "XML-based"; no field names or structure are
documented anywhere accessible.

This reader/writer therefore uses a reasonable, self-consistent XML shape
of our own design -- NOT confirmed to be byte-compatible with what actual
CrossFire produces or expects. A file exported here will very likely NOT
open correctly in real CrossFire, and a real CrossFire .cfp file will
likely NOT import correctly here. This exists so round-tripping through
this GUI alone still works, and as a starting point to correct once a
real .cfp sample file is available to diff against.

If you have a real .cfp file, share it and this can be rewritten to match
the actual format exactly.
"""

from __future__ import annotations

import xml.etree.ElementTree as ET

from grid_model import EMPTY, Puzzle

CFP_UNVERIFIED_WARNING = (
    "This .cfp reader/writer is best-effort and NOT verified against real "
    "CrossFire files (no spec or sample was available). Files may not be "
    "compatible with the actual CrossFire application."
)


def to_cfp_bytes(puzzle: Puzzle) -> bytes:
    root = ET.Element("crossword-puzzle", attrib={"format-note": "best-effort, unverified"})
    meta = ET.SubElement(root, "metadata")
    ET.SubElement(meta, "title").text = puzzle.title
    ET.SubElement(meta, "author").text = puzzle.author
    ET.SubElement(meta, "copyright").text = puzzle.copyright
    ET.SubElement(meta, "notes").text = puzzle.notes

    grid = ET.SubElement(root, "grid", attrib={"width": str(puzzle.width), "height": str(puzzle.height)})
    for r in range(puzzle.height):
        row = ET.SubElement(grid, "row", attrib={"index": str(r)})
        for c in range(puzzle.width):
            if puzzle.blocks[r][c]:
                ET.SubElement(row, "cell", attrib={"col": str(c), "block": "true"})
            else:
                ch = puzzle.letters[r][c]
                attrib = {"col": str(c)}
                if ch != EMPTY:
                    attrib["letter"] = ch
                ET.SubElement(row, "cell", attrib=attrib)

    clues = ET.SubElement(root, "clues")
    for s in puzzle.compute_slots():
        ET.SubElement(
            clues,
            "clue",
            attrib={"number": str(s.number), "direction": s.direction, "slot": s.id},
        ).text = puzzle.clues.get(s.id, "")

    return b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="unicode").encode("utf-8")


def from_cfp_bytes(data: bytes) -> Puzzle:
    root = ET.fromstring(data)
    grid_el = root.find("grid")
    width = int(grid_el.get("width"))
    height = int(grid_el.get("height"))
    puzzle = Puzzle.blank(width, height)

    for row_el in grid_el.findall("row"):
        r = int(row_el.get("index"))
        for cell_el in row_el.findall("cell"):
            c = int(cell_el.get("col"))
            if cell_el.get("block") == "true":
                puzzle.blocks[r][c] = True
            else:
                letter = cell_el.get("letter")
                if letter:
                    # Kept whole, not truncated to one character -- the
                    # writer below already puts a rebus square's full
                    # answer in this same attribute unabridged.
                    puzzle.letters[r][c] = letter.upper()

    meta_el = root.find("metadata")
    if meta_el is not None:
        puzzle.title = (meta_el.findtext("title") or "").strip()
        puzzle.author = (meta_el.findtext("author") or "").strip()
        puzzle.copyright = (meta_el.findtext("copyright") or "").strip()
        puzzle.notes = (meta_el.findtext("notes") or "").strip()

    clues_el = root.find("clues")
    if clues_el is not None:
        for clue_el in clues_el.findall("clue"):
            slot_id = clue_el.get("slot")
            if slot_id and clue_el.text:
                puzzle.clues[slot_id] = clue_el.text

    return puzzle
