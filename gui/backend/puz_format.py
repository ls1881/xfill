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

Rebus squares are supported via the standard GRBS/RTBL extra sections
(also verified against puzpy): the main solution/fill grid bytes hold only
a rebus cell's *first* letter (Across Lite's own convention for what a
plain, rebus-unaware reader should show there), while GRBS/RTBL carry the
real, full multi-character answer. These two sections are appended after
the notes only when the puzzle actually has a rebus square -- an ordinary
puzzle's file layout is completely unchanged.

Timer (LTIM) and markup (GEXT) extension sections are still not written,
and are ignored on read: nothing in this GUI has a concept of a solve
timer or per-cell markup (circles/shading) to round-trip.

Extra-section layout (GRBS, RTBL, and any other, following the main
notes field): 4-byte ASCII title, uint16 LE data length, uint16 LE
checksum of the data bytes (same running algorithm as _data_cksum), then
the data itself, then a single NUL terminator byte -- see
_extra_section/_read_extra_sections below. These sections sit outside the
header's own global/text checksums entirely; adding them never changes
those.
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
    a blank player grid. A rebus cell writes only its solving_letter()
    (its first character) here -- the real, full answer goes in the
    GRBS/RTBL sections instead (see _rebus_sections); this is the standard
    .puz convention, not a lossy shortcut."""
    w, h = puzzle.width, puzzle.height
    out = bytearray(w * h)
    for r in range(h):
        for c in range(w):
            idx = r * w + c
            if puzzle.blocks[r][c]:
                out[idx] = ord(".")
            else:
                ch = puzzle.letters[r][c]
                out[idx] = ord(puzzle.solving_letter(r, c)) if ch != EMPTY else ord("-")
    return bytes(out)


def _extra_section(title: bytes, data: bytes) -> bytes:
    """One GRBS/RTBL-shaped extra section -- see this module's docstring
    for the layout."""
    return struct.pack("<4sHH", title, len(data), _data_cksum(data)) + data + b"\x00"


def _rebus_sections(puzzle: Puzzle) -> bytes:
    """GRBS (one byte per cell: 0 = not a rebus, else 1-based index into
    RTBL) + RTBL (" idx:ANSWER;" entries, one per distinct rebus string)
    -- both omitted entirely if the puzzle has no rebus square at all, so
    a plain puzzle's file bytes are completely unchanged from before this
    feature existed. Two cells sharing the identical rebus string share
    one RTBL entry, matching how real .puz files do it (and keeping the
    table from growing with duplicate answers)."""
    w, h = puzzle.width, puzzle.height
    index_by_string: dict[str, int] = {}
    grbs = bytearray(w * h)
    for r in range(h):
        for c in range(w):
            if puzzle.blocks[r][c] or not puzzle.is_rebus(r, c):
                continue
            letter = puzzle.letters[r][c]
            if letter not in index_by_string:
                index_by_string[letter] = len(index_by_string)
            grbs[r * w + c] = index_by_string[letter] + 1
    if not index_by_string:
        return b""
    rebus_strings = sorted(index_by_string, key=index_by_string.get)
    rtbl = "".join(f" {i:2d}:{s};" for i, s in enumerate(rebus_strings)).encode("latin-1", errors="replace")
    return _extra_section(b"GRBS", bytes(grbs)) + _extra_section(b"RTBL", rtbl)


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
    out += _rebus_sections(puzzle)
    return bytes(out)


def _read_extra_sections(data: bytes, pos: int) -> dict[bytes, bytes]:
    """Every GRBS/RTBL/GEXT/LTIM/... section from `pos` (right after
    notes) to the end of the file, keyed by their 4-byte title. Malformed
    trailing bytes (shorter than one more full 8-byte section header) just
    stop the scan rather than raising -- the sections this reader actually
    uses (GRBS/RTBL) are simply absent from the result then, same as a
    puzzle that never had them."""
    sections: dict[bytes, bytes] = {}
    while pos + 8 <= len(data):
        title = data[pos:pos + 4]
        length, _cksum = struct.unpack_from("<HH", data, pos + 4)
        pos += 8
        if pos + length > len(data):
            break
        sections[title] = data[pos:pos + length]
        pos += length + 1  # +1 skips the section's own NUL terminator
    return sections


def _parse_rtbl(data: bytes) -> dict[int, str]:
    """RTBL's " idx:ANSWER;"-per-entry text back into {idx: answer} --
    the inverse of _rebus_sections' own formatting above."""
    table: dict[int, str] = {}
    for entry in data.decode("latin-1", errors="replace").split(";"):
        idx_s, sep, word = entry.strip().partition(":")
        if not sep:
            continue
        try:
            table[int(idx_s)] = word
        except ValueError:
            continue
    return table


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

    # Rebus squares: GRBS/RTBL, if present, override whatever single
    # placeholder letter the solution grid held for that cell (see
    # _grid_bytes/_rebus_sections) with the real, full answer.
    extra_sections = _read_extra_sections(data, pos)
    grbs, rtbl = extra_sections.get(b"GRBS"), extra_sections.get(b"RTBL")
    if grbs and rtbl:
        rebus_table = _parse_rtbl(rtbl)
        for r in range(h):
            for c in range(w):
                idx = r * w + c
                if idx >= len(grbs) or grbs[idx] == 0 or puzzle.blocks[r][c]:
                    continue
                word = rebus_table.get(grbs[idx] - 1)
                if word:
                    puzzle.letters[r][c] = word.upper()

    clue_pairs = _clue_order(puzzle)
    for (slot_id, _), text in zip(clue_pairs, clue_strings):
        if text:
            puzzle.clues[slot_id] = text
    return puzzle
