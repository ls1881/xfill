"""CrossFire's .cfp format -- reader and writer.

Verified against a real CrossFire-exported file (a completed 21x21 NYT
Sunday-style puzzle), not reconstructed from memory or guessed at -- the
structure below (root `<CROSSFIRE>` element, flat `<TITLE>`/`<AUTHOR>`/
`<COPYRIGHT>` metadata, `<GRID width="W">` holding the grid as raw
newline-separated text rows, `<WORDS>` holding one `<WORD dir="ACROSS"|
"DOWN" num="N">clue text</WORD>` per slot, `<NOTES/>`) all matches that
sample byte-for-byte in the places that matter (only whitespace/
indentation differs, which XML doesn't treat as meaningful).

Two things remain genuine guesses, since the one sample available is a
fully-solved grid with no blank cells and every clue's `isTheme="false"`:
  - The character an OPEN, not-yet-filled cell uses in `<GRID>` (there's
    no unfilled cell in the sample to observe). "-" is used here, matching
    every other format writer in this project (see puz_format.py) and
    this project's own EMPTY sentinel -- a reasonable default, not a
    confirmed one. A space is also accepted on read, defensively.
  - Each `<WORD>`'s `id` attribute: observed to be some CrossFire-internal
    per-word identifier with no derivable formula from grid position or
    clue number (every ACROSS id happens to equal that word's 0-indexed
    rank by clue number, but DOWN ids don't follow any such pattern this
    project could reverse-engineer) -- but every actual clue-to-answer
    reference in the sample (both in the app's own model and in clue text
    like "Pitt who portrayed 80-Down") goes through `num`+`dir`, never
    `id`, so this project's own writer just assigns fresh sequential ids
    (0-indexed, ACROSS then DOWN, matching the sample's own observed id
    ordering for ACROSS) rather than trying to replicate CrossFire's
    unknown internal scheme -- read back in, `id` is ignored entirely in
    favor of `num`+`dir`, so this can't lose information either way.
`isTheme` is written "false" unconditionally (this project has no
matching concept) and ignored on read.
"""

from __future__ import annotations

import xml.etree.ElementTree as ET

from grid_model import EMPTY, Puzzle

CFP_UNVERIFIED_WARNING = (
    "This .cfp reader/writer is verified against a real CrossFire file's "
    "structure, but two details (the open-cell character in an unsolved "
    "grid, and the WORD id scheme) are reasonable guesses rather than "
    "confirmed -- see cfp_format.py's module docstring."
)


def _xml_escape_text(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def to_cfp_bytes(puzzle: Puzzle) -> bytes:
    slots = puzzle.compute_slots()
    across = [s for s in slots if s.direction == "across"]
    down = [s for s in slots if s.direction == "down"]

    lines = [
        '<?xml version="1.0" encoding="utf-8" standalone="no"?>',
        "<CROSSFIRE>",
        "    <VERSION>1</VERSION>",
        f"    <TITLE>{_xml_escape_text(puzzle.title)}</TITLE>",
        f"    <AUTHOR>{_xml_escape_text(puzzle.author)}</AUTHOR>",
        f"    <COPYRIGHT>{_xml_escape_text(puzzle.copyright)}</COPYRIGHT>",
        f'    <GRID width="{puzzle.width}">',
    ]
    for r in range(puzzle.height):
        row_chars = []
        for c in range(puzzle.width):
            if puzzle.blocks[r][c]:
                row_chars.append(".")
            else:
                ch = puzzle.letters[r][c]
                # solving_letter(), not the raw cell content: like every
                # other fixed-width-per-cell grid text this project writes
                # (to_grid_spec, .puz's main solution grid), a rebus
                # square's full multi-character answer can't fit in one
                # position here without corrupting every column after it.
                row_chars.append(puzzle.solving_letter(r, c) if ch != EMPTY else "-")
        lines.append("".join(row_chars))
    lines.append("</GRID>")

    lines.append("    <WORDS>")
    word_id = 0
    for s in across:
        clue = _xml_escape_text(puzzle.clues.get(s.id, ""))
        lines.append(f'        <WORD dir="ACROSS" id="{word_id}" isTheme="false" num="{s.number}">{clue}</WORD>')
        word_id += 1
    for s in down:
        clue = _xml_escape_text(puzzle.clues.get(s.id, ""))
        lines.append(f'        <WORD dir="DOWN" id="{word_id}" isTheme="false" num="{s.number}">{clue}</WORD>')
        word_id += 1
    lines.append("    </WORDS>")

    lines.append(f"    <NOTES>{_xml_escape_text(puzzle.notes)}</NOTES>" if puzzle.notes else "    <NOTES/>")
    lines.append("</CROSSFIRE>")
    return ("\n".join(lines) + "\n").encode("utf-8")


def from_cfp_bytes(data: bytes) -> Puzzle:
    root = ET.fromstring(data)
    if root.tag != "CROSSFIRE":
        raise ValueError(f"not a CrossFire .cfp file: root element is <{root.tag}>, expected <CROSSFIRE>")

    grid_el = root.find("GRID")
    if grid_el is None:
        raise ValueError("missing <GRID> element")
    width_attr = grid_el.get("width")
    if width_attr is None:
        raise ValueError("<GRID> is missing its width attribute")
    width = int(width_attr)

    rows = [line for line in (grid_el.text or "").split("\n") if line.strip()]
    if not rows:
        raise ValueError("<GRID> has no rows")
    for i, line in enumerate(rows):
        if len(line) != width:
            raise ValueError(f"grid row {i + 1} has length {len(line)}, expected width={width}")

    puzzle = Puzzle.blank(width, len(rows))
    for r, line in enumerate(rows):
        for c, ch in enumerate(line):
            if ch == ".":
                puzzle.blocks[r][c] = True
            elif ch not in ("-", " "):
                puzzle.letters[r][c] = ch.upper()

    puzzle.title = (root.findtext("TITLE") or "").strip()
    puzzle.author = (root.findtext("AUTHOR") or "").strip()
    puzzle.copyright = (root.findtext("COPYRIGHT") or "").strip()
    puzzle.notes = (root.findtext("NOTES") or "").strip()

    # Clue text is matched onto this app's own slot ids via num+dir (the
    # only identifiers a real clue reference -- e.g. "Pitt who portrayed
    # 80-Down" -- ever uses), never via WORD's own `id` attribute: see the
    # module docstring for why that attribute isn't trustworthy to key off.
    slot_id_by_key = {(s.direction, s.number): s.id for s in puzzle.compute_slots()}
    words_el = root.find("WORDS")
    if words_el is not None:
        for word_el in words_el.findall("WORD"):
            direction = (word_el.get("dir") or "").lower()
            number_attr = word_el.get("num")
            if direction not in ("across", "down") or number_attr is None:
                continue
            slot_id = slot_id_by_key.get((direction, int(number_attr)))
            if slot_id is not None and word_el.text:
                puzzle.clues[slot_id] = word_el.text

    return puzzle
