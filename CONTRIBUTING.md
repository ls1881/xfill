# Contributing to xfill

Thanks for taking the time to contribute. This project's whole ethos is
"every design decision is backed by a measurement, not an assumption" (see
[`docs/design.md`](docs/design.md)) — that applies to contributions too:
a change that's reasoned through and, where it matters, benchmarked, is
far more useful than one that just "seems like it should help."

## Reporting a bug

Open an [issue](../../issues) with:

- **What you did, what you expected, what actually happened.**
- **How to reproduce it**, as concretely as possible:
  - For the C++ solver/CLI: the exact command, and the grid-spec +
    dictionary files (or a minimal version that still triggers it) —
    attach them or paste them inline.
  - For the GUI: the puzzle (Export it, or note the grid size/shape) and
    the browser you're using. Screenshots help for layout/print issues.
  - For anything nondeterministic (a race, a rare wrong solve): as much
    detail as you can — this project has hunted down and fixed real races
    before (see `docs/design.md`) by reproducing them under a stress
    test, so "it happens sometimes" is a valid start, but a repro that
    fails, say, 1 in 30 runs and a script that demonstrates it is much
    more valuable than a one-off report.
- Whether it's a regression (worked before, broke recently) — if so,
  which commit, if you know it.

If you're not sure whether something is a bug or intended behavior,
open an issue anyway and ask — that's what issues are for.

## Requesting a feature / proposing a change

Open an issue describing the problem you're trying to solve, not just the
solution you have in mind — the "why" is what lets a maintainer (or
another contributor) suggest a better approach, or point out it's already
possible another way. For anything touching the solver's algorithm
specifically, skim [`docs/design.md`](docs/design.md) first: several
approaches that look promising on paper are already documented there as
tried-and-measured-not-worth-it, with the numbers to back it up.

## Pull requests

1. Fork the repo, branch off `main`.
2. Make focused commits — one logical change per commit, with a message
   that explains *why*, not just *what* (the existing `git log` is the
   style to match).
3. Build and test before opening the PR:
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --parallel
   ctest --test-dir build --output-on-failure
   ```
   CI runs exactly this on every PR — it must be green. If you touched
   solver behavior (`src/`, `include/xfill/`), add a Catch2 test case in
   `tests/` that would have caught the bug/regression you're fixing, or
   that exercises the new behavior — see `tests/test_solver.cpp` for the
   established style (a short, focused grid + dictionary fixture per
   test, `WriteAndLoadDict` for inline dictionaries). A fix with no
   regression test is much more likely to silently break again later.
4. For a solver **performance** claim specifically, back it with
   `benchmarks/bench_subset.py` (ideally `--compare` against `main`), not
   just "this should be faster" — see the Results section of the README
   for the bar this project already holds itself to.
5. `gui/backend/` (Python) and `gui/frontend/` (JS) currently have no
   committed automated test suite — CI doesn't touch them. If you change
   either, describe in the PR how you verified it (a manual repro against
   the running app is fine; a scratch test script you ran is even
   better, even if it's not something you're adding to the repo). Don't
   let the absence of CI coverage here be a reason to test less carefully
   than the C++ side — it's a reason to be more explicit in the PR
   description about what you checked.
6. Keep the PR scoped to one thing. A bug fix doesn't need an
   accompanying refactor; if you spot something else worth fixing along
   the way, mention it in the PR or open a separate issue instead of
   folding it in.

## Code style

There isn't a separate style guide beyond "match what's already there":

- **Comments explain *why*, not *what*.** Identifiers should already make
  the *what* obvious; a comment earns its place by capturing a non-obvious
  constraint, a rejected alternative, or the reasoning behind a specific
  constant — see almost any function in `src/solver.cpp` or
  `gui/frontend/app.js` for the level of detail this project expects for
  anything non-obvious.
- **No speculative abstraction.** Three similar lines beat a premature
  helper; don't design for a hypothetical future requirement.
- **Every solver-facing change should be able to answer "why is this
  correct?" and, if it's a performance change, "how much, measured how?"**
  — this is the one place this project is genuinely strict.

## Questions

If something's unclear — the codebase, the algorithm, whether an idea is
worth pursuing — open an issue. A question that surfaces a documentation
gap is a useful contribution on its own.
