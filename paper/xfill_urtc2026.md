---
title: "Sharing Conflict Weights Without Partitioning: A Concurrent dom/wdeg Portfolio for Word-Level Crossword Filling"
author: "[Author Name(s)], [Department], [University] — *submitted to IEEE MIT URTC 2026*"
---

**Abstract** — Parallel constraint solvers generally take one of two paths: partition the search space across workers, or run an undivided portfolio of independent restarts. Prior work that shares information between constraint-satisfaction-problem (CSP) workers — SPREAD/ELF's manager-averaged variable weights, embarrassingly parallel search's disjoint decomposition — treats that sharing as a prelude to partitioning: gathered once, then spent. This paper describes a different point in that design space: a restart portfolio whose workers never partition the problem, but continuously and concurrently update a shared pool of dom/wdeg-style conflict weights throughout the entire search, with no manager and no synchronization phase. Each worker's failures immediately bias every other worker's branch ordering, while every worker keeps searching the full, overlapping problem. We implement this mechanism in xfill, a word-level CSP solver for American-style crossword filling, and evaluate it against orca-solver, an independently developed partition-based parallel solver, on a corpus of real scraped crossword grids and on an adversarially constructed grid engineered to be exhaustively hard. Across an 11-grid hard-instance set, the two solvers succeed on different, only partially overlapping subsets; across 50 randomly sampled real grids under a 5-minute cap, the restart-portfolio solver wins by a substantial margin 17 times to the partition-based solver's 1. Shared conflict weights give a further, consistent improvement within the restart-portfolio regime, but only once scoped to avoid two measured regressions — one at a single thread, one for a worker whose value depends on an uninterrupted search trajectory — that a naive implementation introduces. We also report a negative result from the same codebase: five independently plausible letter-level branching heuristics produced numerically identical output on our hardest benchmark grid, because a domain-size precondition meant none of them ever executed — a fact surfaced only by direct execution-path instrumentation, not by comparing benchmark scores.

# I. Introduction

American-style crossword filling — assigning a dictionary word to every slot in a grid so that every crossing letter agrees — is a natural constraint satisfaction problem (CSP): slots are variables, candidate words are domains, and shared cells are binary constraints. It is also a convenient testbed for search research: instances are human-legible, difficulty can be controlled directly through grid geometry (block density, entry length, crossing density) independently of dictionary size, and strong, independently developed solvers already exist to compare against. Ginsberg's Dr.Fill [1] established crossword filling as a serious weighted-CSP application over a decade ago; several open solvers, including orca-solver used in this paper, have been built since.

This paper is not primarily about crossword filling, however. It uses crossword filling as the setting in which to examine a specific, narrower question in parallel constraint search: when multiple search workers run concurrently, is there value in sharing *soft* heuristic information — statistics that bias search order without asserting anything logically — continuously, without ever partitioning the problem those workers are solving? We show this combination is distinct from the three closest points in the literature we could find, implement it, and measure its effect.

The paper makes three contributions. First, a description of a concurrent, decentralized conflict-weight-sharing mechanism for a restart-portfolio CSP solver, positioned precisely against the closest prior work (Section III). Second, an empirical comparison between a solver using this mechanism (xfill) and an independently developed partition-based solver (orca-solver), across real and adversarially constructed crossword grids, showing the two architectures are not uniformly ordered but suited to different regimes (Section V). Third, a methodological negative result: a family of heuristics that appeared to have no effect on search performance because a precondition meant they never ran at all, and the direct-instrumentation technique that caught this rather than a benchmark comparison (Section IV).

# II. Background

**CSP formulation and dom/wdeg.** A CSP is a tuple of variables, domains, and constraints; systematic backtracking search assigns values one variable at a time, propagating consistency after each assignment and backtracking on a domain wipeout. The dom/wdeg heuristic [4] selects the variable with the smallest ratio of current domain size to *weighted degree*, where each constraint's weight increases by one every time it causes a wipeout — a simple, effective way to let search learn which parts of a problem are actually hard.

**Heavy-tailed restarts.** Randomized backtracking search exhibits heavy-tailed runtime distributions: a small fraction of runs take vastly longer than the median [2], [3]. Restarting a randomized search periodically — discarding progress and trying again with new random choices — eliminates most of this tail and is now standard in both SAT and CSP solvers.

**Portfolio vs. partition parallelism.** Two dominant strategies exist for parallelizing search. *Portfolio* approaches run several independent searches (over the whole problem) concurrently and take the first result; SAT portfolio solvers such as ManySAT [6] additionally share *learned clauses* between the concurrently running solvers, without partitioning. *Partitioning* approaches instead divide the search space among workers so that each explores a disjoint region; this yields non-overlapping coverage useful for proving unsatisfiability, at the cost of load-balancing difficulty when subproblems have uneven difficulty.

**Where information-sharing sits in CSP parallelism specifically.** The most relevant CSP work we are aware of falls into three groups, none of which occupies the same point in the design space as the mechanism in this paper.

*Sequential, single-worker.* Grimes and Wallace [5] use short restarted "probes" of a single sequential solver to accumulate dom/wdeg-style weights before the real search begins — a *prelude*, and never concurrent across workers.

*Concurrent, but with formal learning.* Ehlers and Stuckey [7] parallelize a lazy-clause-generation CP solver by sharing nogoods and partial trail state between workers, and by reassigning subproblems between workers based on which clause database looks well-suited to which region. This requires a solver with actual clause-learning machinery, and, like ManySAT, shares *hard*, logically derived facts rather than soft statistics.

*Concurrent soft statistics, but gathered for partitioning.* Yun and Epstein's ELF and SPREAD [8], [9] are, to our knowledge, the closest prior work: a manager races several workers, each of which searches under a plain dom/wdeg-style solver and reports its learned variable weights back once it exhausts a time or backtrack budget. The manager *averages* the reported weights and uses them to choose which variables to partition the problem on for a subsequent splitting phase. The authors state this explicitly: "the primary purpose of SPREAD's portfolio phase is to glean information to support search space splitting, not to solve P" [9], and separately note that "most portfolio-based methods for CSPs do not share information" [9] at all outside their own framework. A comprehensive 2018 survey of parallel constraint solving [11] documents no CSP work that shares heuristic statistics continuously between concurrently-running, non-partitioning workers; its own taxonomy has no category for it. Embarrassingly parallel search [10], a widely used and effective later technique, decomposes the problem into many disjoint subproblems with "almost no communication ... between workers" — a different point again, sharing nothing at all.

The mechanism described in Section III occupies the gap these three groups leave: concurrent (unlike Grimes and Wallace), soft rather than logically hard (unlike Ehlers and Stuckey), and never used to partition the search space (unlike Yun and Epstein).

# III. System Architecture

**Grid model.** A grid is parsed into slots (maximal open runs of length ≥ 2 in each direction) and crossings (cell-level intersections between an across and a down slot). Each slot's domain is the set of dictionary words of matching length, narrowed by any pre-filled letters.

**Sequential search.** xfill's core solver performs dom/wdeg-style branching: it selects the slot minimizing domain size divided by a *crossing weight* accumulated per crossing (rather than per constraint directly, since a crossing here is the natural unit of "how troublesome has this junction been"), then propagates letter constraints across all crossings using an AC-3-style queue. On a domain wipeout, the responsible crossing's weight increases. Small domains (below a fixed threshold) enumerate candidate words directly; larger domains use a separate branch, present in the implementation but, as Section IV shows, not always reached in practice.

**Restart portfolio.** `SolveParallel` runs N independent workers, each performing the sequential search above with its own random seed and its own private, decaying crossing-weight table. One worker is dedicated to a single uninterrupted depth-first search with no restarts at all (`unlimited_budget`), guaranteeing eventual exhaustive completion independent of restart-based search's heavy-tailed variance; the remainder restart on a geometric backoff schedule. Any worker's sound "no solution" result — restart-based or the dedicated exhaustive one — cancels every other worker, since either constitutes a complete proof.

**Concurrent conflict-weight sharing.** In addition to each worker's private weight table, `SolveParallel` maintains one array of plain atomic counters, one per crossing, shared by every restart-based worker (the dedicated exhaustive worker is excluded — see below). Every worker increments a crossing's counter on every wipeout it causes and reads every crossing's counter as an additive term when scoring branch candidates. Two design choices distinguish this from a naive port of dom/wdeg to a shared setting. First, the shared counters are plain, order-independent atomic increments rather than the same decaying scheme each worker's private weights use: a decay scheme's normalization term depends on the exact sequence of prior updates, which is only race-free with a single writer, whereas concurrent writers need an update rule independent of order. Second, the dedicated exhaustive worker neither reads nor writes the shared array: its entire value lies in one undisturbed trajectory, and wiring it into a signal driven by other workers' restarts measurably regressed the one grid in our corpus it uniquely solved.

Two correctness properties were necessary before the mechanism's performance numbers were meaningful. A single-threaded call must remain byte-for-byte reproducible with the shared array absent, since a lone worker gains nothing from a signal only it writes; this required gating the wiring on thread count rather than enabling it unconditionally. And thread-sanitizer verification, including real multi-threaded solves rather than only the unit test suite, reported no data races, consistent with every shared update being an independent, order-free atomic operation.

# IV. A Negative Result: Branching on the Wrong Thing

Prompted by a benchmark loss against orca-solver on a difficult 7×7 grid (Section V-A), we implemented five variants of letter-level branching — replacing whole-word enumeration with branching on a single grid cell at a time — reasoning that a smaller per-node branching factor (at most 26 letters rather than thousands of candidate words) should help on wide-open grids with very large domains. The variants included two different minimum-remaining-value scoring rules, a faithful reimplementation of orca-solver's own candidate-scoring heuristic (read directly from its published source for reference), and a reversed scan order.

All five produced numerically identical search node and backtrack counts on the target grid. This is a stronger and more useful signal than "the heuristics didn't help" — five independently reasonable heuristics agreeing to the exact node is not what genuine heuristic equivalence looks like, and warranted checking rather than reporting a null result. A plain integer counter placed directly inside the branch-selection code, tracking how often the large-domain branch (where these heuristics apply) actually executed, showed it firing zero times across more than 17,000 real search nodes on this grid: propagation from the grid's few pre-filled letters narrowed every slot's domain below the branch's activation threshold within the first few assignments, even on an otherwise wide-open grid. All five heuristics were unreachable code for this specific grid the entire time they were being compared — which is exactly why comparing their benchmark scores could not have distinguished them.

The actual bottleneck, found only after ruling out the large-domain branch, was the small-domain branch's word ordering, which had used a fixed deterministic order unaffected by the solver's existing restart-randomization flag. Extending that flag's effect to the small-domain branch — a small, targeted change — is what produced the improvements reported in Section V-A. We report this as a methodological point independent of the specific fix: an A/B comparison of branching heuristics is only informative if the branch under test is confirmed, by direct instrumentation of the control flow rather than by inference from outcomes, to actually execute.

# V. Experiments

**Solvers compared.** xfill (this work) and orca-solver, an independently developed, publicly available solver that parallelizes search by partitioning the grid's search space across worker threads, with dynamic re-splitting of long-running partitions. We do not modify orca-solver; we treat it as a black box representative of the partition-based architecture.

**Dictionary.** Both solvers draw candidate words from the same dictionary file in every comparison, and orca-solver's default 6-letter shared-substring constraint is disabled to match xfill's exact-word-only duplicate rule, so that neither solver is advantaged by a dictionary or duplicate-checking difference unrelated to search strategy. Reported xfill times exclude dictionary-load time (measured separately by the solver itself); orca-solver has no equivalent internal split, so a fixed per-invocation load-time baseline, measured once against a trivial always-blocked grid, is subtracted from its wall-clock time.

## A. A Known Hard Grid

We first compare both solvers on a 7×7 grid with two pre-filled letters and no blocked cells at all, a difficult instance published by orca-solver's own author as a benchmark. orca-solver solves it single-threaded in 201.7s. xfill's unmodified restart-portfolio, before the fix described in Section IV, did not finish this grid in any tested budget; after that fix, xfill solved it in 166.7s single-threaded and 63.0s with 14 threads — real wins, though not reproducible on every run, consistent with restart-based search's known heavy-tailed variance [2]. Table I shows the effect of oversubscribing the thread count beyond this machine's 14 physical cores, which every-worker-restarts-independently theory predicts should help by racing more independent draws from a heavy-tailed distribution.

*Table I. Thread count vs. wall time on the 7×7 benchmark grid (single sample per configuration; orca-solver's single-threaded baseline is 201.7s).*

| Threads | Wall time (s) |
|---|---|
| 1 | 166.7 |
| 14 | 63.0 |
| 28 | 104.6 |
| 42 | 85.6 |
| 56 | 112.4 |

Every oversubscribed configuration beats orca-solver's single-threaded baseline; 42 threads (3× the physical core count) is the fastest configuration tested, with contention overhead visibly eroding the gain by 56 threads.

## B. An 11-Grid Hard Set

We assembled 11 grids known from prior informal testing to resist plain restart-based search, including the 7×7 grid above, and ran both solvers to a 300-second cap. Table II summarizes outcomes; six grids timed out for both solvers under every configuration tested and are omitted from the table for space. Neither solver dominates: orca-solver's partitioned search uniquely solves one grid (grid_303) that xfill cannot within the cap at any thread count tested, while xfill's `unlimited_budget` worker uniquely solves another (grid_115) that orca-solver cannot.

*Table II. Outcomes on the 5 (of 11) hard-set grids where at least one solver succeeded. "auto" is xfill's default thread count (14, matching this machine's physical cores).*

| Grid | xfill (auto) | orca (14t) |
|---|---|---|
| grid_045 | 1.0s | 16.7s |
| grid_115 | 73.6s | timeout |
| grid_120 | 4.1s | 6.8s |
| grid_303 | 67.9s | 3.4s |
| 7×7 (above) | 210.1s | 8.9s |

## C. Fifty Randomly Sampled Real Grids

To measure typical-case behavior rather than only adversarial or hand-picked cases, we sampled 50 grids uniformly at random from a corpus of real, previously published crossword grids and ran both solvers to a 5-minute cap. Raw win counts were close — xfill faster on 23, orca-solver faster on 21, neither finishing on 6 — but raw counts are misleading when many "wins" are separated by milliseconds. Filtering to margins exceeding 0.5 seconds, xfill's advantage becomes clear: 17 substantial wins to orca-solver's 1.

## D. Case Study: An Exhaustively Hard Constructed Grid

The comparisons above use real or previously known grids. To test both solvers' ability to *prove* a negative result rather than only find positive ones, we constructed a grid deliberately, by hand, to be maximally constrained: a stack of five mutually crossing 15-letter entries, with the surrounding grid structured so that most crossing entries are short (3–9 letters) and the design was independently verified, cell by cell, against both solvers' own grid parsers before use. Both solvers, run to completion with no time limit, independently concluded the grid has no solution under our dictionary: xfill's restart portfolio exhausted 2.16 billion search nodes across 791 restarts in 76 minutes without finding one; orca-solver's partitioned search reported "search exhausted" after enumerating and eliminating 224 partitions in 25 minutes. That the faster, structurally exhaustive partitioned search reached its conclusion roughly three times faster than the restart portfolio's inherently redundant coverage of the same space is consistent with the discussion in Section VI.

# VI. Discussion

The results above are consistent with a simple explanation grounded in the restart-vs-partition distinction itself, not specific to either implementation. Restart-based portfolios benefit from heavy-tailed runtime distributions: more independent draws increase the chance of an early lucky one, which is exactly why oversubscribing thread count past physical core count still helped in Table I, and why xfill's margin over orca-solver on real grids (Section V-C) is a *finding*-oriented result. Partition-based search instead gives genuine, non-overlapping coverage of the search space, which is what a *proof* of unsatisfiability requires; restart-based search can approximate this only through its `unlimited_budget` fallback, at the cost of forgoing restarts' benefit entirely for that one worker. This also explains why shared conflict weights (Section III) helped broadly across our grid corpus but did not resolve the two grids in Table II that only one architecture could solve: the mechanism improves the restart portfolio's branch ordering, but does not change what the restart portfolio's coverage of the search space fundamentally is.

# VII. Limitations

All timing results are from a single machine (14 physical cores) and are not intended to establish absolute performance; thread-scaling numbers in particular should not be assumed to transfer to different hardware. The dictionary used is a specific blend of licensed and freely available word lists; the unsatisfiability result in Section V-D is a claim about that specific dictionary, not an intrinsic property of the grid. Both solvers are actively developed software, not fixed artifacts; exact version/commit identifiers are recorded alongside this paper's supplementary material for reproducibility.

# VIII. Conclusion

We described a concurrent conflict-weight-sharing mechanism for restart-portfolio CSP search that occupies a specific gap in prior work — soft rather than hard information, shared continuously between workers rather than gathered once, and never used to partition the search space — and showed it is distinct from the closest three points in the literature (Ehlers and Stuckey's nogood sharing, Grimes and Wallace's sequential probing, and Yun and Epstein's gather-then-partition SPREAD/ELF). Implemented in xfill and evaluated against an independently developed partition-based solver, the mechanism contributes to a broader empirical pattern: restart portfolios and partitioned search are not uniformly ordered, but suited respectively to finding solutions quickly and to proving none exist. We also reported a negative result — five heuristics rendered indistinguishable by a precondition that kept them from ever executing — as a reusable methodological caution for anyone comparing branching heuristics by benchmark score alone.

# Acknowledgments

[Add any advisor, funding, or collaborator acknowledgments here.]

# References

[1] M. L. Ginsberg, "Dr.Fill: Crosswords and an Implemented Solver for Singly Weighted CSPs," *Journal of Artificial Intelligence Research*, vol. 42, pp. 851–886, 2011.

[2] C. P. Gomes, B. Selman, and H. Kautz, "Boosting Combinatorial Search Through Randomization," in *Proc. 15th National Conference on Artificial Intelligence (AAAI)*, 1998, pp. 431–437.

[3] C. P. Gomes, B. Selman, N. Crato, and H. Kautz, "Heavy-tailed phenomena in satisfiability and constraint satisfaction problems," *Journal of Automated Reasoning*, vol. 24, pp. 67–100, 2000.

[4] F. Boussemart, F. Hemery, C. Lecoutre, and L. Sais, "Boosting systematic search by weighting constraints," in *Proc. 16th European Conference on Artificial Intelligence (ECAI)*, 2004, pp. 146–150.

[5] D. Grimes and R. J. Wallace, "Sampling Strategies and Variable Selection in Weighted Degree Heuristics," in *Proc. 13th International Conference on Principles and Practice of Constraint Programming (CP)*, Lecture Notes in Computer Science, vol. 4741, 2007, pp. 831–838.

[6] Y. Hamadi, S. Jabbour, and L. Sais, "ManySAT: a Parallel SAT Solver," *Journal on Satisfiability, Boolean Modeling and Computation*, vol. 6, pp. 245–262, 2009.

[7] T. Ehlers and P. J. Stuckey, "Parallelizing Constraint Programming with Learning," in *Proc. 13th International Conference on Integration of AI and OR Techniques in Constraint Programming (CPAIOR)*, Lecture Notes in Computer Science, vol. 9676, Banff, Canada, 2016.

[8] X. Yun and S. L. Epstein, "Adaptive Parallelization for Constraint Satisfaction Search," in *Proc. 5th Annual Symposium on Combinatorial Search (SoCS)*, 2012, pp. 145–152.

[9] X. Yun and S. L. Epstein, "A Hybrid Paradigm for Adaptive Parallel Search," in *Proc. 18th International Conference on Principles and Practice of Constraint Programming (CP)*, Lecture Notes in Computer Science, vol. 7514, 2012, pp. 720–736.

[10] J.-C. Régin, M. Rezgui, and A. Malapert, "Embarrassingly Parallel Search in Constraint Programming," *Journal of Artificial Intelligence Research*, vol. 57, pp. 421–464, 2016.

[11] I. P. Gent, I. J. Miguel, P. W. Nightingale, C. McCreesh, P. Prosser, N. Moore, and C. Unsworth, "A Review of Literature on Parallel Constraint Solving," *Theory and Practice of Logic Programming*, vol. 18, no. 5–6, pp. 725–758, 2018.
