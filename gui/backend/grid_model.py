"""Shared crossword grid model used by every format reader/writer and the API.

A constructor tool (unlike a solving app) has no meaningful distinction
between "solution" and "player's fill" -- there's exactly one grid of
letters, the answer itself, being built up. `Puzzle.letters` holds that.
"""

from __future__ import annotations

from dataclasses import dataclass, field


BLOCK = "#"
EMPTY = "-"  # open cell with no letter yet


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
    letters: list[list[str]]  # letters[r][c]: 'A'-'Z' or EMPTY; ignored where blocked
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
        like "C-T" ready for dictionary pattern matching."""
        return "".join(self.letters[r][c] if self.letters[r][c] != EMPTY else "?" for r, c in slot.cells)

    def to_grid_spec(self) -> str:
        """xfill's plain-text grid format: '.'=open, '#'=block, A-Z=prefilled.
        One row per line, no header."""
        lines = []
        for r in range(self.height):
            row_chars = []
            for c in range(self.width):
                if self.blocks[r][c]:
                    row_chars.append("#")
                elif self.letters[r][c] != EMPTY:
                    row_chars.append(self.letters[r][c])
                else:
                    row_chars.append(".")
            lines.append("".join(row_chars))
        return "\n".join(lines) + "\n"

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
                    letter_freq[ch] = letter_freq.get(ch, 0) + 1
        return {
            "word_count": word_count,
            "avg_word_length": round(avg_length, 2),
            "block_count": block_count,
            "block_percent": round(100.0 * block_count / total_cells, 1) if total_cells else 0.0,
            "letter_count": letter_count,
            "length_breakdown": dict(sorted(length_breakdown.items())),
            "letter_counts": letter_freq,
        }
