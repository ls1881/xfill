"""Across Lite .puz binary format -- reader and writer.

Spec verified against alexdej/puzpy (github.com/alexdej/puzpy), a widely
used reference implementation, rather than reconstructed from memory:
byte-exact checksums are required for a .puz file to open in real
crossword software, so guessing at offsets here would silently produce
files that don't interoperate.

Header layout (52 bytes, struct format '<H 11s xH Q 4s 2sH 12s BBHHH'):

  offset  size  field
  0x00    2     global checksum (uint16 LE)
  0x02    11    magic string "ACROSS&DOWN" (no null)
  0x0D    1     null pad byte (completes the 12-byte "ACROSS&DOWN\0")
  0x0E    2     header/CIB checksum
  0x10    8     masked checksum region (4 low bytes + 4 high bytes, XORed
                 with MASKSTRING "ICHEATED")
  0x18    4     file version string, e.g. b"1.3\\0"
  0x1C    2     reserved (unk1)
  0x1E    2     scrambled checksum (0 = not scrambled)
  0x20    12    reserved (unk2)
  0x2C    1     width
  0x2D    1     height
  0x2E    2     number of clues
  0x30    2     puzzle type (0x0001 = normal)
  0x32    2     solution state (0x0000 = unlocked)
  0x34    --    grid data starts here

Followed by: solution grid (w*h bytes), fill grid (w*h bytes), then
NUL-terminated strings: title, author, copyright, then `numclues` clue
strings (across/down interleaved by grid number -- see `_clue_order`),
then notes.

Rebus (GRBS/RTBL/RUSR), timer (LTIM) and markup (GEXT) extension sections
are not written, and are ignored on read: this is a constructor tool for
plain letter grids, not a player-facing app, and none of xfill's solving
or this GUI's editing features use them.
"""

from __future__ import annotations

import struct

from grid_model import EMPTY, Puzzle

MASKSTRING = b"ICHEATED"
HEADER_FORMAT = "<H11sxH8s4s2sH12sBBHHH"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
ACROSSDOWN = b"ACROSS&DOWN"


def _data_cksum(data: bytes, cksum: int = 0) -> int:
    for b in data:
        lowbit = cksum & 1
        cksum >>= 1
        if lowbit:
            cksum |= 0x8000
        cksum = (cksum + b) & 0xFFFF
    return cksum


def _zstring(s: str) -> bytes:
    """Null-terminated string for the *byte stream*: always includes the
    terminator, even for an empty string -- title/author/copyright/each
    clue/notes are fixed slots a reader walks by scanning for the next
    \\x00, so omitting the terminator when a field happens to be empty
    would misalign every field after it. This is deliberately different
    from what contributes to text_cksum() below, which (matching puzpy's
    own text_cksum) skips an empty field's bytes entirely -- the file
    layout's "always present, possibly empty" rule and the checksum's
    "only non-empty fields count" rule are two separate things."""
    return s.encode("latin-1", errors="replace") + b"\x00"


def _clue_order(puzzle: Puzzle) -> list[tuple[str, str]]:
    """(slot_id, clue_text) pairs in .puz's required order: reading order
    over numbered cells, across before down at a cell that starts both."""
    slots_by_key = {(s.direction, s.row, s.col): s for s in puzzle.compute_slots()}
    ordered: list[tuple[str, str]] = []
    w, h = puzzle.width, puzzle.height

    def open_cell(r, c):
        return 0 <= r < h and 0 <= c < w and not puzzle.blocks[r][c]

    for r in range(h):
        for c in range(w):
            if not open_cell(r, c):
                continue
            starts_across = not open_cell(r, c - 1) and open_cell(r, c + 1)
            starts_down = not open_cell(r - 1, c) and open_cell(r + 1, c)
            if starts_across:
                s = slots_by_key[("across", r, c)]
                ordered.append((s.id, puzzle.clues.get(s.id, "")))
            if starts_down:
                s = slots_by_key[("down", r, c)]
                ordered.append((s.id, puzzle.clues.get(s.id, "")))
    return ordered


def _grid_bytes(puzzle: Puzzle) -> bytes:
    """Grid bytes in Across Lite's own convention -- block cells are '.'
    (NOT grid_model's '#', which is this project's own internal/display
    convention for xfill's plain-text grid spec and has nothing to do with
    .puz; puzpy's is_blacksquare() only recognizes '.' or ':'). Used for
    both the solution and fill sections: a constructor grid has one state,
    not a separate "player's fill," so both get the same bytes -- open,
    unfilled cells become '-', matching what Across Lite itself writes for
    a blank player grid."""
    w, h = puzzle.width, puzzle.height
    out = bytearray(w * h)
    for r in range(h):
        for c in range(w):
            idx = r * w + c
            if puzzle.blocks[r][c]:
                out[idx] = ord(".")
            else:
                ch = puzzle.letters[r][c]
                out[idx] = ord(ch) if ch != EMPTY else ord("-")
    return bytes(out)


def to_puz_bytes(puzzle: Puzzle) -> bytes:
    w, h = puzzle.width, puzzle.height
    solution = _grid_bytes(puzzle)
    fill = solution

    clue_pairs = _clue_order(puzzle)
    clue_strings = [text for _, text in clue_pairs]

    def header_cksum() -> int:
        return _data_cksum(struct.pack("<BBHHH", w, h, len(clue_strings), 1, 0))

    def text_cksum() -> int:
        cksum = 0
        if puzzle.title:
            cksum = _data_cksum(_zstring(puzzle.title), cksum)
        if puzzle.author:
            cksum = _data_cksum(_zstring(puzzle.author), cksum)
        if puzzle.copyright:
            cksum = _data_cksum(_zstring(puzzle.copyright), cksum)
        for clue in clue_strings:
            if clue:
                cksum = _data_cksum(clue.encode("latin-1", errors="replace"), cksum)
        if puzzle.notes:
            cksum = _data_cksum(_zstring(puzzle.notes), cksum)
        return cksum

    hc = header_cksum()
    sol_c = _data_cksum(solution)
    fill_c = _data_cksum(fill)
    txt_c = text_cksum()

    # global_cksum chains header -> solution -> fill -> text (the same
    # regions text_cksum() walks), so continue that chain from a
    # solution/fill-seeded checksum instead of duplicating text_cksum's body.
    seed = _data_cksum(fill, _data_cksum(solution, hc))
    if puzzle.title:
        seed = _data_cksum(_zstring(puzzle.title), seed)
    if puzzle.author:
        seed = _data_cksum(_zstring(puzzle.author), seed)
    if puzzle.copyright:
        seed = _data_cksum(_zstring(puzzle.copyright), seed)
    for clue in clue_strings:
        if clue:
            seed = _data_cksum(clue.encode("latin-1", errors="replace"), seed)
    if puzzle.notes:
        seed = _data_cksum(_zstring(puzzle.notes), seed)
    global_cksum = seed

    low = bytes(
        [
            (hc & 0xFF) ^ MASKSTRING[0],
            (sol_c & 0xFF) ^ MASKSTRING[1],
            (fill_c & 0xFF) ^ MASKSTRING[2],
            (txt_c & 0xFF) ^ MASKSTRING[3],
        ]
    )
    high = bytes(
        [
            ((hc >> 8) & 0xFF) ^ MASKSTRING[4],
            ((sol_c >> 8) & 0xFF) ^ MASKSTRING[5],
            ((fill_c >> 8) & 0xFF) ^ MASKSTRING[6],
            ((txt_c >> 8) & 0xFF) ^ MASKSTRING[7],
        ]
    )
    masked = low + high

    header = struct.pack(
        HEADER_FORMAT,
        global_cksum,
        ACROSSDOWN,
        hc,
        masked,
        b"1.3\x00",
        b"\x00\x00",
        0,
        b"\x00" * 12,
        w,
        h,
        len(clue_strings),
        1,
        0,
    )

    out = bytearray()
    out += header
    out += solution
    out += fill
    out += _zstring(puzzle.title)
    out += _zstring(puzzle.author)
    out += _zstring(puzzle.copyright)
    for clue in clue_strings:
        out += _zstring(clue)
    # Always present (at minimum a lone terminator) for the 1.3 file
    # version this writer always emits -- see _zstring's doc comment for
    # why this can't be conditioned on `puzzle.notes` the way its
    # checksum contribution is.
    out += _zstring(puzzle.notes)
    return bytes(out)


def from_puz_bytes(data: bytes) -> Puzzle:
    header = data[:HEADER_SIZE]
    (_global_cksum, magic, _hc, _masked, _version, _unk1, _scrambled, _unk2, w, h,
     numclues, _ptype, _sstate) = struct.unpack(HEADER_FORMAT, header)
    if magic != ACROSSDOWN:
        raise ValueError("not a .puz file (bad magic string)")

    pos = HEADER_SIZE
    solution = data[pos:pos + w * h]
    pos += w * h
    pos += w * h  # skip the fill grid -- solution is authoritative for a constructor grid

    def read_zstring() -> str:
        nonlocal pos
        end = data.index(b"\x00", pos)
        s = data[pos:end].decode("latin-1")
        pos = end + 1
        return s

    title = read_zstring()
    author = read_zstring()
    copyright_ = read_zstring()
    clue_strings = [read_zstring() for _ in range(numclues)]
    notes = read_zstring() if pos < len(data) else ""

    puzzle = Puzzle.blank(w, h)
    for r in range(h):
        for c in range(w):
            ch = chr(solution[r * w + c])
            # '.' (and the rarely-used ':') are Across Lite's own block
            # markers -- NOT '#', which is only this project's internal
            # display convention (see _grid_bytes above).
            if ch in (".", ":"):
                puzzle.blocks[r][c] = True
            elif ch != "-":
                puzzle.letters[r][c] = ch
    puzzle.title = title
    puzzle.author = author
    puzzle.copyright = copyright_
    puzzle.notes = notes

    clue_pairs = _clue_order(puzzle)
    for (slot_id, _), text in zip(clue_pairs, clue_strings):
        if text:
            puzzle.clues[slot_id] = text
    return puzzle
