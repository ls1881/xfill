#!/usr/bin/env python3
"""Scrapes 15x15 grid layouts (block patterns only) from crosswordgrids.com
into this project's grid text format, for use as benchmark/test grids.

Each grid page embeds its layout as an inline SVG: a full-size background
rect plus one small rect per blocked cell. This script pulls the block
cells' pixel coordinates back out and converts them to a `.`/`#` grid.

Usage:
    python3 benchmarks/scrape_crosswordgrids.py
    python3 benchmarks/scrape_crosswordgrids.py --start 1 --end 500 --delay 1.0

Safe to interrupt and re-run: already-downloaded grids are skipped.
"""

import argparse
import csv
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

BASE_URL = "https://crosswordgrids.com/15x15-crossword-grids/grid-{}"
GRID_SIZE = 15
USER_AGENT = (
    "crossword-filler-benchmark-scraper/1.0 "
    "(one-time collection of grid layouts for local CSP-solver testing; "
    "low request rate, see benchmarks/scrape_crosswordgrids.py)"
)

SVG_RE = re.compile(r'<svg id="cg-svg".*?</svg>', re.S)
VIEWBOX_RE = re.compile(r'viewBox="0 0 (\d+) (\d+)"')
RECT_RE = re.compile(r'<rect x="(\d+)" y="(\d+)" width="(\d+)" height="(\d+)"')
BLACK_SQUARES_RE = re.compile(r"(\d+)\s*black squares")
OPEN_SQUARES_RE = re.compile(r"(\d+)\s*open squares")


def fetch(url, retries=3, backoff=1.5):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    last_err = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                return resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            last_err = e
        except urllib.error.URLError as e:
            last_err = e
        time.sleep(backoff * (attempt + 1))
    raise RuntimeError(f"failed to fetch {url}: {last_err}")


def parse_grid(html):
    """Returns (lines, black_count, open_count) or None if unparseable."""
    svg_match = SVG_RE.search(html)
    if not svg_match:
        return None
    svg = svg_match.group(0)

    viewbox_match = VIEWBOX_RE.search(svg)
    if not viewbox_match:
        return None
    width = int(viewbox_match.group(1))
    cell_size = width // GRID_SIZE
    if cell_size * GRID_SIZE != width:
        return None

    blocked = set()
    for x, y, w, h in RECT_RE.findall(svg):
        w = int(w)
        if w >= width:
            continue  # the full-size background rect, not a block cell
        row, col = int(y) // cell_size, int(x) // cell_size
        if not (0 <= row < GRID_SIZE and 0 <= col < GRID_SIZE):
            return None  # coordinates outside the expected grid -- bail
        blocked.add((row, col))

    lines = [
        "".join("#" if (r, c) in blocked else "." for c in range(GRID_SIZE))
        for r in range(GRID_SIZE)
    ]

    black_match = BLACK_SQUARES_RE.search(html)
    open_match = OPEN_SQUARES_RE.search(html)
    black_count = int(black_match.group(1)) if black_match else len(blocked)
    open_count = int(open_match.group(1)) if open_match else GRID_SIZE * GRID_SIZE - len(blocked)
    return lines, black_count, open_count


def load_manifest(path):
    rows = {}
    if path.exists():
        with path.open(newline="") as f:
            for row in csv.DictReader(f):
                rows[int(row["grid"])] = row
    return rows


def write_manifest(path, rows):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["grid", "black_squares", "open_squares", "source_url"])
        writer.writeheader()
        for n in sorted(rows):
            writer.writerow(rows[n])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--start", type=int, default=1)
    parser.add_argument("--end", type=int, default=500)
    parser.add_argument("--out", default="benchmarks/grids/scraped_15x15")
    parser.add_argument("--delay", type=float, default=0.75, help="seconds between requests")
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = out_dir / "manifest.csv"
    manifest = load_manifest(manifest_path)

    fetched = 0
    for n in range(args.start, args.end + 1):
        out_file = out_dir / f"grid_{n:03d}.txt"
        if out_file.exists() and n in manifest:
            continue

        url = BASE_URL.format(n)
        try:
            html = fetch(url)
        except RuntimeError as e:
            print(f"[{n}] ERROR: {e}", file=sys.stderr)
            continue

        if html is None:
            print(f"[{n}] 404 -- no more grids, stopping", file=sys.stderr)
            break

        parsed = parse_grid(html)
        if parsed is None:
            print(f"[{n}] WARNING: could not parse grid, skipping", file=sys.stderr)
            time.sleep(args.delay)
            continue

        lines, black_count, open_count = parsed
        out_file.write_text("\n".join(lines) + "\n")
        manifest[n] = {
            "grid": n,
            "black_squares": black_count,
            "open_squares": open_count,
            "source_url": url,
        }
        fetched += 1
        print(f"[{n}] saved {out_file.name} (black={black_count} open={open_count})")

        time.sleep(args.delay)

    write_manifest(manifest_path, manifest)
    print(f"done: {fetched} grids fetched this run, {len(manifest)} total in {manifest_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
