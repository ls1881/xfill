# crossword-filler

A high-performance crossword grid autofill engine, written in C++20.

Crossword filling is modeled as a constraint satisfaction problem
(across/down slots as variables, dictionary words as domains, crossing
letters as constraints) and solved with constraint propagation and a
heuristic-guided backtracking search. See [`docs/design.md`](docs/design.md)
for architecture and future work, and [`docs/bibliography.md`](docs/bibliography.md)
for the papers/codebases each technique below is drawn from.

## Status

✅ Slot detection, crossing computation, no-duplicate-words enforcement,
queue-based AC-3 propagation, `dom/wdeg` branching, randomized restarts,
and a parallel-restart portfolio search (`Solver::SolveParallel`, `xfill_cli`'s
default) are all implemented and tested (20/20 tests passing). Small and
medium grids (tested up to 15x15 with real block patterns, against a real
~280k-entry dictionary at `min_score=40`) solve in well under a second;
some genuinely dense grids remain intractable, which is a documented,
expected limit (see "Known limits" below and
[`docs/design.md`](docs/design.md)'s "Known hard cases" section for more),
not a bug.

## How the algorithm works

This section is kept up to date as `Solver` changes — for the full
reasoning and citations behind each piece, see
[`include/xfill/solver.hpp`](include/xfill/solver.hpp)'s class comment
(most detailed and most current) and
[`docs/bibliography.md`](docs/bibliography.md) (sources).

1. **Model.** Every across/down run of open cells is a *slot* (a
   variable); its *domain* is every same-length dictionary word; a
   *crossing* between two slots is a constraint that they agree on their
   shared letter. Domains are `WordBitset`s — one bit per word of that
   length — so intersecting/narrowing a domain is a handful of `uint64_t`
   operations rather than a loop over strings.

2. **Propagation.** After every assignment, `Propagate` runs queue-based
   AC-3: only slots whose domain actually shrank get re-examined (not a
   fixed rescan of every crossing), and it skips a narrowing step
   entirely when either every letter is still viable at that position, or
   the neighbor's domain is already a subset of the incoming filter.
   (Source: `rainjacket/orca-solver`.) Checking "which letters are viable
   at this crossing" takes one of two paths depending on how narrow the
   slot's domain already is: a domain below `kDirectLookupThreshold`
   candidates reads their actual letters directly (cheap once
   `WordBitset::SetBits()` skips zero chunks via `ctz` instead of testing
   every index one at a time); a wider domain tests all 26 `LetterMask`s
   against it instead, since materializing every surviving candidate
   isn't worth it when there are thousands of them. This split -- and the
   `SetBits()` fix, reusing one scratch bitset per length instead of
   heap-allocating a fresh one per crossing, indexing all per-length state
   (`LetterMask`, that scratch bitset) directly by length instead of
   through a hash map, and reusing the propagation queue's membership
   buffer across calls instead of reallocating it per node -- came
   directly out of `sample`-profiling real (scraped, not synthetic) 15x15s
   that were timing out.

3. **Branching.** `SelectBranchSlot` picks which slot to guess next using
   `dom/wdeg`: (masked domain size) ÷ (summed weight of this slot's
   crossings to still-unassigned neighbors), lowest first. Crossing
   weights start at 1, get bumped by 1 whenever propagation through that
   crossing wipes out a domain, and decay 1% back toward 1 on every other
   wipeout — so the heuristic tracks *currently* troublesome crossings
   instead of a fixed notion of constrainedness. (`CrossingWeights`
   computes this lazily — one crossing updated per wipeout rather than
   decaying all of them every time — a VSIDS-style trick borrowed from
   MiniSat; see `docs/bibliography.md`'s Eén & Sörensson entry.) Word
   choice within a
   slot always tries higher dictionary-score words first (`ScoreOrder`),
   so a fill reads like a real crossword instead of the first
   alphabetically-valid guess. (Source: `rf-/ingrid_core`, crediting
   Balafoutis's "Adaptive Strategies for Solving CSPs".) Below a
   candidate-count threshold, the live candidates are extracted directly
   and sorted by a precomputed score rank instead of walking every word of
   that length checking membership — the same "direct extraction below a
   threshold" split step 2 uses for letter viability, applied here to word
   selection; see `docs/design.md` for the measured effect and a
   correctness bug this surfaced and fixed along the way.

4. **Backtracking.** Trail-based: assigning a slot snapshots only the
   domains that assignment actually touches (once per decision level),
   so undoing a decision restores exactly what changed rather than
   copying every slot's domain at every search node. (Source:
   `rainjacket/orca-solver`.)

5. **Restarts.** The whole search in steps 2-4 runs inside a retry loop.
   The first attempt is fully deterministic (greedy `dom/wdeg`). If an
   attempt racks up more dead ends than its budget (starts at 500, grows
   ×1.1 per retry), it aborts and restarts from the root with a new RNG
   seed — restarts after the first pick their branch slot via a
   weighted-random choice among the best few `dom/wdeg`-ranked slots
   (weights `{4, 2, 1}`) instead of always the single best, so different
   attempts actually explore different branch orders. Crossing weights
   learned by `dom/wdeg` carry over across restarts; only the search tree
   itself starts over. Because the budget only ever grows, this stays a
   *complete* search — an unsatisfiable grid is still eventually proven
   so. (Sources: `rf-/ingrid_core`'s restart loop for the mechanism;
   Gomes, Selman & Kautz, "Boosting Combinatorial Search Through
   Randomization," for *why* it helps — backtracking search runtimes are
   often heavy-tailed, so an attempt that's had a demonstrably unlucky
   run is better abandoned than waited out.)

6. **Duplicate words.** A slot's effective domain is always masked
   against a global "words of this length already used elsewhere" bitset,
   rather than writing exclusions into every sibling domain on each
   assignment — so no word is ever placed twice in one fill.

7. **Component-restricted branching.** `Solver` computes the connected
   components of the slot-crossing graph once at construction time (one
   BFS pass), and branching only ever considers the lowest-indexed
   component that still has an unassigned slot — fully settling one
   before starting the next, since components sharing no crossing can
   never help or hurt each other's search. This is the crossword analogue
   of the "critical junction" structure road-routing algorithms exploit
   (see `docs/bibliography.md`'s Dechter entry): it's a free no-op for
   any single-component grid — which is every curated grid here *and*
   every one of the 500 real scraped grids, since well-built crosswords
   are essentially always fully interlocked — but a real ~2.6x win on a
   grid that does have independent regions (see
   `benchmarks/grids/synthetic/disconnected_15x15.txt`, one of two
   purpose-built edge-case grids alongside `rectangular_9x5.txt`, which
   tests a non-square grid).

8. **Parallel restarts.** `Solver::SolveParallel` runs several independent
   restart sequences at once (`hardware_concurrency()` threads by
   default, `xfill_cli`'s default) instead of steps 2-6's single retry
   loop — a direct, if belated, application of the Gomes/Selman/Kautz
   heavy-tailed-runtime result above: if a *different* random run of the
   same search often finishes fast, running several simultaneously
   should find one that does sooner in wall-clock time. Each worker gets
   its own private `Solver` (own domains, trail, crossing weights,
   nogoods, RNG — nothing search-related is shared, so nothing needs
   synchronizing beyond one `std::atomic<bool>` cancellation flag,
   checked once per node); worker 0 reproduces today's exact
   single-threaded sequence, every other worker's own first attempt is
   already randomized so it doesn't just redo worker 0's deterministic
   pass for free. Still complete — unsatisfiable is only reported once
   every worker has independently exhausted its own search. Real, large
   net win on the real benchmark set (three 30-grid samples: -43.6%
   total time, two previously-timing-out grids newly solved, zero lost),
   but not uniform: a handful of grids that were already close to
   worker 0's best case get *slower* from added thread contention with
   no compensating benefit — see `docs/design.md` for the full numbers
   and `docs/bibliography.md`'s Gomes, Selman & Kautz entry. Pass an
   explicit `num_threads` of 1 for the old single-threaded behavior.

### Known limits

`benchmarks/grids/curated/sample_13x13.txt` still hasn't finished after
15+ minutes, even at `min_score=40` -- confirmed still true even with
`SolveParallel`'s 14-way portfolio search (also stopped after 20+
minutes with no solution). That's an expected result, not a bug: per
Anbulagan & Botea's phase-transition study of crossword CSPs, some
"hard region" instances stay expensive for *any* search order, because
the underlying instance itself is hard, not just this solver's choices
leading up to it -- restarts (and, by the same logic, more of them at
once) fix a search that got *unlucky*, but can't turn a genuinely hard
instance easy. `sample_15x15.txt` (the original,
fully-open-interior 15x15, as opposed to `sample_15x15_interlock.txt`
which solves quickly) is not in that category: it solves in about 4.7s at
`min_score=40` -- much faster than the ~158s this took earlier in the
project's history, thanks to the propagation and restart optimizations
in `docs/design.md`'s "Implementation summary" -- it just needs a
less-restricted dictionary, not a fundamentally harder search.
`sample_21x21.txt` is different again: it's proven unsatisfiable in
microseconds, because it has a fully-open 21-cell row and the dictionary
has no words that long even at `min_score=0` (max length 15) -- not a
search problem at all.

## Dictionary format

One entry per line, semicolon-delimited: `WORD;SCORE`. Score is used two
ways: entries below `min_score` (the solver's third CLI argument) are
dropped from the dictionary entirely at load time, and among the words
that remain, the solver tries higher-scored ones first within each slot
(`Dictionary::ScoreOrder`) so a fill reads like a real crossword rather
than the first alphabetically-valid guess. Words are uppercased on load,
so mixed-case input is fine.

`min_score=40` is the recommended default for `data/spreadthewordlist_caps.txt`
specifically (and is what `benchmarks/bench_subset.py` defaults to) --
see `docs/design.md`'s "Dictionary tuning" section for why lower
thresholds stop helping solvability and start hurting fill quality in
this particular wordlist.

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
./build/xfill_cli <grid_spec_file> <dictionary_file> [min_score] [num_threads]
```

`num_threads` defaults to 0, meaning `std::thread::hardware_concurrency()`
(see "Parallel restarts" above). Pass `1` for the old single-threaded
behavior — useful for reproducible timing, or comparing against a build
predating `SolveParallel`.

## Benchmarking

```bash
./benchmarks/run_benchmarks.sh ./build/xfill_cli data/spreadthewordlist_caps.txt
```

Grids in `benchmarks/grids/curated/` were generated with:

```bash
python3 benchmarks/generate_grid.py --size 15 --block-pairs 18 > benchmarks/grids/curated/sample_15x15.txt
```

`benchmarks/grids/synthetic/` holds a couple of hand-built grids that
target one specific solver behavior each rather than general sizing
(disconnected components, a non-square shape) — see step 7 above.

`benchmarks/grids/scraped_15x15/` holds 500 real 15x15 grid layouts
(block patterns only, via `benchmarks/scrape_crosswordgrids.py`) —
a much harder, more realistic benchmark set than the small curated one
above. `benchmarks/bench_subset.py` runs a reproducible random sample of
them against `xfill_cli`, with a per-grid timeout, and can diff two runs
against each other:

```bash
python3 benchmarks/bench_subset.py --n 20 --seed 42 --save before.csv
# ...make a change...
python3 benchmarks/bench_subset.py --n 20 --seed 42 --compare before.csv
```

## License

MIT — see [LICENSE](LICENSE).
