#!/usr/bin/env python3
"""Standalone command-line interface to the xfill solver.

For anyone who wants to fill a crossword grid without running the GUI at
all. Reads a real puzzle file -- .puz, .ipuz, .cfp (best-effort, see
cfp_format.py), or xfill's own plain-text grid-spec format (.txt or any
other extension: '.'=open, '#'=block, a letter=prefilled, one row per
line -- see grid_model.Puzzle.from_grid_spec) -- fills it using the exact
same C++ solver and format readers/writers the GUI itself uses, and
writes the result back out.

No extra dependencies beyond the C++ solver binary itself: this script
and everything it imports (grid_model, puz_format, ipuz_format,
cfp_format, solver_bridge) use only the Python standard library, so
`python3 cli.py ...` works with no `pip install` and no virtualenv --
unlike app.py (the GUI's own backend), which needs FastAPI/uvicorn.

Examples:
    # Fill a .puz file, same dictionary for both directions, write to a
    # new file (defaults to INPUT_filled.EXT next to the input if -o is
    # omitted).
    python3 cli.py mypuzzle.puz --dict words.txt --min-score 40

    # Different dictionaries/thresholds per direction, explicit output.
    python3 cli.py grid.txt \\
        --across-dict across.txt --across-min 30 \\
        --down-dict down.txt --down-min 50 \\
        -o filled.txt

    # Settings from a config file (see --config below), overriding just
    # one setting from the command line.
    python3 cli.py mypuzzle.ipuz --config myconfig.json --maximize

    # Print the exact config-file shape this script understands.
    python3 cli.py --show-config-example

Config file (--config path/to/config.json): a JSON object with any of
the same settings as the flags below, using these keys -- across_dict,
across_min_score, down_dict, down_min_score, across_min_overrides (an
object like {"3": 25, "5": 60}), down_min_overrides, threads, maximize.
A flag on the command line always overrides the same setting from a
config file, so a config can hold your everyday defaults (dictionary
paths, a usual min score) while a flag tweaks just one run.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import signal
import sys
import time

from grid_model import Puzzle
import cfp_format
import ipuz_format
import puz_format
import solver_bridge

_READERS = {
    ".puz": puz_format.from_puz_bytes,
    ".ipuz": ipuz_format.from_ipuz_bytes,
    ".cfp": cfp_format.from_cfp_bytes,
}
_WRITERS = {
    ".puz": puz_format.to_puz_bytes,
    ".ipuz": ipuz_format.to_ipuz_bytes,
    ".cfp": cfp_format.to_cfp_bytes,
}

_CONFIG_EXAMPLE = {
    "across_dict": "/path/to/across_words.txt",
    "across_min_score": 40,
    "down_dict": "/path/to/down_words.txt",
    "down_min_score": 40,
    "across_min_overrides": {"3": 10, "15": 60},
    "down_min_overrides": {},
    "threads": 0,
    "maximize": False,
}


def _read_puzzle(path: pathlib.Path) -> Puzzle:
    ext = path.suffix.lower()
    reader = _READERS.get(ext)
    if reader is not None:
        return reader(path.read_bytes())
    # Anything else (.txt, .grid, no extension, ...) is treated as xfill's
    # own plain-text grid-spec format -- the same one the C++ solver's CLI
    # takes directly, and what to_grid_spec()/from_grid_spec() round-trip.
    return Puzzle.from_grid_spec(path.read_text(encoding="utf-8"))


def _write_puzzle(puzzle: Puzzle, path: pathlib.Path, format_override: str | None) -> None:
    ext = (f".{format_override}" if format_override else path.suffix).lower()
    writer = _WRITERS.get(ext)
    if writer is not None:
        path.write_bytes(writer(puzzle))
    else:
        path.write_text(puzzle.to_grid_spec(), encoding="utf-8")


def _parse_overrides(spec: str | None) -> dict[int, int]:
    """"<length>:<score>,<length>:<score>,..." -- same wire format the C++
    CLI's own --across-min-overrides/--down-min-overrides take."""
    if not spec:
        return {}
    overrides: dict[int, int] = {}
    for pair in spec.split(","):
        pair = pair.strip()
        if not pair:
            continue
        length_s, _, score_s = pair.partition(":")
        overrides[int(length_s)] = int(score_s)
    return overrides


def _load_config(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        config = json.load(f)
    if not isinstance(config, dict):
        raise ValueError(f"{path}: config file must contain a JSON object")
    return config


def _resolve_settings(args: argparse.Namespace, config: dict) -> dict:
    """Merges config-file values with command-line flags, flags always
    winning over the same setting in the config -- see this module's own
    docstring for why. `--dict`/`--min-score` are a shorthand for "both
    directions the same"; an explicit --across-*/--down-* flag (or its
    config-file equivalent) still wins over that shorthand for its own
    direction specifically."""
    shared_dict = args.dict if args.dict is not None else config.get("dict")
    shared_min = args.min_score if args.min_score is not None else config.get("min_score")

    def resolve(flag_value, config_key, shared_value, default=None):
        if flag_value is not None:
            return flag_value
        if config_key in config:
            return config[config_key]
        return shared_value if shared_value is not None else default

    across_overrides = _parse_overrides(args.across_min_overrides)
    down_overrides = _parse_overrides(args.down_min_overrides)
    if not across_overrides:
        across_overrides = {int(k): v for k, v in config.get("across_min_overrides", {}).items()}
    if not down_overrides:
        down_overrides = {int(k): v for k, v in config.get("down_min_overrides", {}).items()}

    return {
        "across_dict": resolve(args.across_dict, "across_dict", shared_dict),
        "across_min_score": resolve(args.across_min, "across_min_score", shared_min, 0),
        "down_dict": resolve(args.down_dict, "down_dict", shared_dict),
        "down_min_score": resolve(args.down_min, "down_min_score", shared_min, 0),
        "across_min_overrides": across_overrides,
        "down_min_overrides": down_overrides,
        "threads": resolve(args.threads, "threads", None, 0),
        "maximize": args.maximize if args.maximize else config.get("maximize", False),
    }


def _build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="cli.py",
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("input", nargs="?", help="Puzzle file to fill: .puz, .ipuz, .cfp, or a plain-text grid spec (any other extension)")
    p.add_argument("-o", "--output", help="Where to write the filled puzzle (default: INPUT_filled.EXT next to the input)")
    p.add_argument("--format", choices=["puz", "ipuz", "cfp", "txt"], help="Output format, overriding whatever --output's/the input's extension implies")
    p.add_argument("--config", help="JSON config file with any of the settings below -- see this script's own --help epilog for the exact shape")
    p.add_argument("--dict", help="Dictionary path for BOTH across and down (shorthand for --across-dict and --down-dict together)")
    p.add_argument("--min-score", type=int, help="Min score for BOTH directions (shorthand for --across-min and --down-min together)")
    p.add_argument("--across-dict", help="Dictionary path for across entries")
    p.add_argument("--across-min", type=int, help="Minimum word score for across entries (0-100)")
    p.add_argument("--down-dict", help="Dictionary path for down entries")
    p.add_argument("--down-min", type=int, help="Minimum word score for down entries (0-100)")
    p.add_argument("--across-min-overrides", help='Per-length min-score overrides for across, e.g. "3:10,15:60"')
    p.add_argument("--down-min-overrides", help='Per-length min-score overrides for down, e.g. "3:10,15:60"')
    p.add_argument("--threads", type=int, help="Worker thread count (0 = use every available core)")
    p.add_argument("--maximize", action="store_true", help="Keep searching for a higher-scoring fill instead of stopping at the first valid one (branch-and-bound; slower, interruptible with Ctrl+C)")
    p.add_argument("--quiet", action="store_true", help="Suppress progress output on stderr; only the final summary is printed")
    p.add_argument("--show-config-example", action="store_true", help="Print an example --config JSON file and exit")
    return p


def main(argv: list[str] | None = None) -> int:
    parser = _build_arg_parser()
    args = parser.parse_args(argv)

    if args.show_config_example:
        print(json.dumps(_CONFIG_EXAMPLE, indent=2))
        return 0
    if not args.input:
        parser.error("the following arguments are required: input")

    input_path = pathlib.Path(args.input)
    if not input_path.exists():
        print(f"error: input file not found: {input_path}", file=sys.stderr)
        return 1

    try:
        puzzle = _read_puzzle(input_path)
    except Exception as e:
        print(f"error: could not read {input_path}: {e}", file=sys.stderr)
        return 1

    config = {}
    if args.config:
        try:
            config = _load_config(args.config)
        except Exception as e:
            print(f"error: could not read config {args.config}: {e}", file=sys.stderr)
            return 1

    settings = _resolve_settings(args, config)
    if not settings["across_dict"] or not settings["down_dict"]:
        print(
            "error: no dictionary specified for one or both directions -- pass --dict, "
            "or --across-dict/--down-dict, or set them in a --config file",
            file=sys.stderr,
        )
        return 1
    for direction, path in (("across", settings["across_dict"]), ("down", settings["down_dict"])):
        if not pathlib.Path(path).exists():
            print(f"error: {direction} dictionary not found: {path}", file=sys.stderr)
            return 1

    if args.output:
        output_path = pathlib.Path(args.output)
    else:
        output_path = input_path.with_name(f"{input_path.stem}_filled{input_path.suffix}")

    def log(message: str) -> None:
        if not args.quiet:
            print(message, file=sys.stderr)

    log(f"Solving {input_path} ({puzzle.width}x{puzzle.height})...")
    started = time.monotonic()
    best_score = None

    # Ctrl+C during a long (especially --maximize) solve should stop the
    # underlying xfill_cli subprocess, not just this Python process --
    # solve_stream's generator is still suspended mid-iteration when a
    # KeyboardInterrupt lands, so the subprocess would otherwise be
    # orphaned rather than terminated with it.
    def handle_sigint(_signum, _frame):
        solver_bridge.cancel_current_fill()

    old_handler = signal.signal(signal.SIGINT, handle_sigint)
    try:
        final_event = None
        for event in solver_bridge.solve_stream(
            puzzle,
            settings["across_dict"], settings["across_min_score"],
            settings["down_dict"], settings["down_min_score"],
            threads=settings["threads"],
            maximize=settings["maximize"],
            across_min_overrides=settings["across_min_overrides"],
            down_min_overrides=settings["down_min_overrides"],
        ):
            if event["type"] == "progress":
                log(f"  ...{event['nodes']:,} nodes explored ({time.monotonic() - started:.1f}s)")
            elif event["type"] == "improved":
                best_score = event["score"]
                log(f"  improved: score {best_score:,} ({time.monotonic() - started:.1f}s)")
                solver_bridge.apply_solution(puzzle, {"solved": True, "grid": event.get("grid")})
            else:
                final_event = event
    finally:
        signal.signal(signal.SIGINT, old_handler)

    elapsed = time.monotonic() - started
    if final_event is None or final_event["type"] == "error":
        message = final_event.get("message", "unknown error") if final_event else "no result produced"
        print(f"error: {message}", file=sys.stderr)
        return 1
    if final_event["type"] == "cancelled":
        if best_score is not None:
            log(f"Cancelled after {elapsed:.1f}s -- keeping the best fill found (score {best_score:,})")
        else:
            print(f"Cancelled after {elapsed:.1f}s -- no fill found yet, nothing written", file=sys.stderr)
            return 130
    elif not final_event.get("solved"):
        print(f"No solution found ({elapsed:.1f}s, {final_event.get('nodes', 0):,} nodes)", file=sys.stderr)
        return 1
    else:
        solver_bridge.apply_solution(puzzle, final_event)

    try:
        _write_puzzle(puzzle, output_path, args.format)
    except Exception as e:
        print(f"error: could not write {output_path}: {e}", file=sys.stderr)
        return 1

    stats = puzzle.stats()
    log(f"Solved in {elapsed:.1f}s -- {stats['word_count']} words, avg length {stats['avg_word_length']}")
    print(f"Wrote {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
