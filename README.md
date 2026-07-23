# crossword-filler

A high-performance crossword grid autofill engine, written in C++20.

Crossword filling is modeled as a constraint satisfaction problem
(across/down slots as variables, dictionary words as domains, crossing
letters as constraints) and solved with constraint propagation and a
heuristic-guided backtracking search. See [`docs/design.md`](docs/design.md)
for the architecture and roadmap.

## Status

✅ Working baseline solver — correctness-first, unoptimized. Slot
detection, crossing computation, AC-3 style propagation, and MRV
backtracking are all implemented and tested. It reliably solves small
and medium grids (tested up to 7x7 with a real ~280k-entry dictionary
in well under a second); a fully-loaded 15x15 is not yet fast, since no
performance work has been done on top of the baseline (see
[`docs/design.md`](docs/design.md) roadmap — that's the next phase).

## Dictionary format

One entry per line, semicolon-delimited: `WORD;SCORE`. Score is parsed
and retained but not yet used by the solver (a hook for later
quality-guided search). Words are uppercased on load, so mixed-case
input is fine.

```
CAT;50
DOG;40
```

Drop your wordlist in `data/` (e.g. `data/spreadthewordlist_caps.txt`)
and pass its path as the second CLI argument.

## Grid format

One row per line, `.` for an open cell and `#` for a block:

```
...#...
...#...
.......
##...##
.......
...#...
...#...
```

Generate a random symmetric one with `benchmarks/generate_grid.py`.

## Building

Requires CMake 3.20+ and a C++20 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Running

```bash
./build/xfill_cli <grid_spec_file> <dictionary_file>
```

## Benchmarking

```bash
./benchmarks/run_benchmarks.sh ./build/xfill_cli data/spreadthewordlist_caps.txt
```

Grids in `benchmarks/grids/` were generated with:

```bash
python3 benchmarks/generate_grid.py --size 15 --block-pairs 18 > benchmarks/grids/sample_15x15.txt
```

## License

MIT — see [LICENSE](LICENSE).
