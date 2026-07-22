# crossword-filler

A high-performance crossword grid autofill engine, written in C++20.

Crossword filling is modeled as a constraint satisfaction problem
(across/down slots as variables, dictionary words as domains, crossing
letters as constraints) and solved with constraint propagation and a
heuristic-guided backtracking search. See [`docs/design.md`](docs/design.md)
for the architecture and roadmap.

## Status

🚧 Early scaffold — core data structures and interfaces are in place;
the actual propagation/search logic is being built out incrementally,
with each optimization benchmarked before/after rather than assumed.

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
./benchmarks/run_benchmarks.sh ./build/xfill_cli data/wordlist_sample.txt
```

## License

MIT — see [LICENSE](LICENSE).
