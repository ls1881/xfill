"""Shared crossword grid model used by every format reader/writer and the API.

A constructor tool (unlike a solving app) has no meaningful distinction
between "solution" and "player's fill" -- there's exactly one grid of
letters, the answer itself, being built up. `Puzzle.letters` holds that.
"""

from __future__ import annotations

from dataclasses import dataclass, field


BLOCK = "#"
EMPTY = "-"  # open cell with no letter yet

# Standard English Scrabble tile point values -- used by Puzzle.stats()'s
# "scrabble_avg" (see there for how a rebus cell's several letters are
# each counted individually, same as letter_counts already does).
_SCRABBLE_VALUES = {
    "A": 1, "E": 1, "I": 1, "O": 1, "U": 1, "L": 1, "N": 1, "S": 1, "T": 1, "R": 1,
    "D": 2, "G": 2,
    "B": 3, "C": 3, "M": 3, "P": 3,
    "F": 4, "H": 4, "V": 4, "W": 4, "Y": 4,
    "K": 5,
    "J": 8, "X": 8,
    "Q": 10, "Z": 10,
}


@dataclass
class Slot:
    number: int
    direction: str  # "across" | "down"
    row: int
    col: int
    length: int
    cells: list[tuple[int, int]]

    @property
    def id(self) -> str:
        return f"{'A' if self.direction == 'across' else 'D'}{self.number}"


@dataclass
class Puzzle:
    width: int
    height: int
    blocks: list[list[bool]]  # blocks[r][c]
    # letters[r][c]: EMPTY, a single letter, or (a rebus square) a whole
    # multi-character string like "STAR" -- ignored where blocked. See
    # solving_letter() for the single-character stand-in a rebus cell uses
    # everywhere that genuinely needs exactly one character per cell (the
    # solver, pattern matching): its real content is never lost, just
    # represented differently for those specific consumers.
    letters: list[list[str]]
    title: str = ""
    author: str = ""
    copyright: str = ""
    notes: str = ""
    clues: dict[str, str] = field(default_factory=dict)  # slot id -> clue text

    @staticmethod
    def blank(width: int, height: int) -> "Puzzle":
        return Puzzle(
            width=width,
            height=height,
            blocks=[[False] * width for _ in range(height)],
            letters=[[EMPTY] * width for _ in range(height)],
        )

    def is_symmetric_block(self, r: int, c: int) -> bool:
        return self.blocks[self.height - 1 - r][self.width - 1 - c]

    def toggle_block(self, r: int, c: int, symmetric: bool = False) -> None:
        new_state = not self.blocks[r][c]
        self.blocks[r][c] = new_state
        if not new_state:
            self.letters[r][c] = EMPTY
        else:
            self.letters[r][c] = EMPTY
        if symmetric:
            sr, sc = self.height - 1 - r, self.width - 1 - c
            self.blocks[sr][sc] = new_state
            self.letters[sr][sc] = EMPTY

    def set_letter(self, r: int, c: int, letter: str) -> None:
        if self.blocks[r][c]:
            return
        self.letters[r][c] = letter.upper() if letter else EMPTY

    def is_rebus(self, r: int, c: int) -> bool:
        return len(self.letters[r][c]) > 1

    def solving_letter(self, r: int, c: int) -> str:
        """The single character this cell contributes wherever exactly one
        character per cell is required -- the C++ solver's grid spec, and
        slot_pattern()'s dictionary-matching pattern. A rebus cell's
        first character is the well-established convention for this (also
        what .puz's own solution grid stores for a rebus square, alongside
        the full answer in its separate GRBS/RTBL sections -- see
        puz_format.py), not an arbitrary choice: the real, full content
        always stays in `letters` itself and is never touched by this."""
        letter = self.letters[r][c]
        return letter[0] if letter else EMPTY

    def compute_slots(self) -> list[Slot]:
        """Numbers cells and derives across/down slots.

        Matches the standard crossword/.puz numbering convention: scan
        cells in reading order (row-major); a cell gets a number if it
        starts an across run of length >= 2 or a down run of length >= 2
        (a numbered cell may start both, in which case it gets one number
        shared by both slots). Across slots are listed before down slots
        for the same number, matching .puz's clue ordering (see
        puz_format.py).
        """
        w, h = self.width, self.height
        blocked = self.blocks

        def open_cell(r: int, c: int) -> bool:
            return 0 <= r < h and 0 <= c < w and not blocked[r][c]

        slots: list[Slot] = []
        num = 0
        for r in range(h):
            for c in range(w):
                if not open_cell(r, c):
                    continue
                starts_across = not open_cell(r, c - 1) and open_cell(r, c + 1)
                starts_down = not open_cell(r - 1, c) and open_cell(r + 1, c)
                if not (starts_across or starts_down):
                    continue
                num += 1
                if starts_across:
                    cells = []
                    cc = c
                    while open_cell(r, cc):
                        cells.append((r, cc))
                        cc += 1
                    slots.append(Slot(num, "across", r, c, len(cells), cells))
                if starts_down:
                    cells = []
                    rr = r
                    while open_cell(rr, c):
                        cells.append((rr, c))
                        rr += 1
                    slots.append(Slot(num, "down", r, c, len(cells), cells))
        return slots

    def slot_pattern(self, slot: Slot) -> str:
        """This slot's current letters, EMPTY where unfilled -- a pattern
        like "C-T" ready for dictionary pattern matching. A rebus cell
        contributes its solving_letter() (see that method's doc comment),
        not its full content -- a pattern is exactly `slot.length`
        characters, one per cell, by construction."""
        return "".join(
            self.solving_letter(r, c) if self.letters[r][c] != EMPTY else "?" for r, c in slot.cells
        )

    def to_grid_spec(self) -> str:
        """xfill's plain-text grid format: '.'=open, '#'=block, A-Z=prefilled.
        One row per line, no header. A rebus cell (see is_rebus) is written
        as its solving_letter() -- the C++ solver has no concept of a
        multi-character cell; this pins the crossing constraint using the
        rebus's first letter without corrupting the row width."""
        lines = []
        for r in range(self.height):
            row_chars = []
            for c in range(self.width):
                if self.blocks[r][c]:
                    row_chars.append("#")
                elif self.letters[r][c] != EMPTY:
                    row_chars.append(self.solving_letter(r, c))
                else:
                    row_chars.append(".")
            lines.append("".join(row_chars))
        return "\n".join(lines) + "\n"

    @staticmethod
    def from_grid_spec(text: str) -> "Puzzle":
        """Inverse of to_grid_spec() -- reads xfill's own plain-text grid
        format ('.'=open, '#'=block, any other character=a prefilled
        letter, one row per line, blank lines ignored). Meant for a
        constructor who wants to hand-author a bare grid layout (the CLI's
        --input accepts this directly, via a .txt extension) without
        needing a full .puz/.ipuz file just to describe the block pattern.
        Every row must be the same width; raises ValueError otherwise,
        since a ragged grid has no single `width` to report."""
        lines = [line for line in text.splitlines() if line.strip()]
        if not lines:
            raise ValueError("grid spec is empty")
        width = len(lines[0])
        for i, line in enumerate(lines):
            if len(line) != width:
                raise ValueError(f"line {i + 1} has length {len(line)}, expected {width} (every row must match the first row's width)")
        puzzle = Puzzle.blank(width, len(lines))
        for r, line in enumerate(lines):
            for c, ch in enumerate(line):
                if ch == "#":
                    puzzle.blocks[r][c] = True
                elif ch != ".":
                    puzzle.letters[r][c] = ch.upper()
        return puzzle

    def stats(self) -> dict:
        slots = self.compute_slots()
        word_count = len(slots)
        lengths = [s.length for s in slots]
        avg_length = sum(lengths) / word_count if word_count else 0.0
        total_cells = self.width * self.height
        block_count = sum(1 for row in self.blocks for b in row if b)
        letter_count = sum(
            1
            for r in range(self.height)
            for c in range(self.width)
            if not self.blocks[r][c] and self.letters[r][c] != EMPTY
        )
        length_breakdown: dict[int, int] = {}
        for length in lengths:
            length_breakdown[length] = length_breakdown.get(length, 0) + 1
        letter_freq: dict[str, int] = {}
        for r in range(self.height):
            for c in range(self.width):
                ch = self.letters[r][c]
                if not self.blocks[r][c] and ch != EMPTY:
                    # A rebus cell counts toward every one of its own
                    # letters individually ("STAR" contributes to S, T, A,
                    # and R), not as one opaque multi-character bucket --
                    # matches the frontend's letter-count click-to-highlight
                    # feature, which looks for a letter anywhere in a cell.
                    for single in ch:
                        letter_freq[single] = letter_freq.get(single, 0) + 1

        # Average Scrabble tile value across every filled letter (a rebus
        # cell's several letters each count individually, same as
        # letter_freq above) -- a rough, standard proxy for how "easy" the
        # current fill's letters were to place. A character with no
        # Scrabble value (a symbol or digit in an unusual rebus) is simply
        # skipped rather than raising, same spirit as letter_counts
        # tolerating anything is_rebus produces.
        scrabble_total = 0
        scrabble_letter_count = 0
        for r in range(self.height):
            for c in range(self.width):
                if self.blocks[r][c]:
                    continue
                ch = self.letters[r][c]
                if ch == EMPTY:
                    continue
                for single in ch:
                    value = _SCRABBLE_VALUES.get(single)
                    if value is not None:
                        scrabble_total += value
                        scrabble_letter_count += 1
        scrabble_avg = round(scrabble_total / scrabble_letter_count, 2) if scrabble_letter_count else None

        # "Open" squares: open cells that don't touch the grid's outer
        # border and have no blocked orthogonal neighbor either -- a rough
        # measure of how much of the interior is unconstrained by any
        # nearby block, since those are exactly the squares whose crossing
        # entries can't lean on a block for a shorter, easier word.
        open_square_count = 0
        for r in range(self.height):
            for c in range(self.width):
                if self.blocks[r][c]:
                    continue
                if r == 0 or r == self.height - 1 or c == 0 or c == self.width - 1:
                    continue
                if self.blocks[r - 1][c] or self.blocks[r + 1][c] or self.blocks[r][c - 1] or self.blocks[r][c + 1]:
                    continue
                open_square_count += 1

        return {
            "word_count": word_count,
            "avg_word_length": round(avg_length, 2),
            "block_count": block_count,
            "block_percent": round(100.0 * block_count / total_cells, 1) if total_cells else 0.0,
            "letter_count": letter_count,
            "length_breakdown": dict(sorted(length_breakdown.items())),
            "letter_counts": letter_freq,
            "scrabble_avg": scrabble_avg,
            "open_square_count": open_square_count,
        }
