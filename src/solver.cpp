#include "xfill/solver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace xfill {

namespace {
constexpr uint32_t kAllLettersMask = (1u << 26) - 1;

// Restart tuning, all taken verbatim from rf-/ingrid_core's
// backtracking_search.rs (RANDOM_SLOT_WEIGHTS, RETRY_GROWTH_FACTOR, and its
// find_fill()'s starting max_backtracks of 500) -- see the Solver class
// comment in solver.hpp for the design this implements.
constexpr size_t kRandomTopN = 3;
constexpr std::array<int, kRandomTopN> kRandomSlotWeights = {4, 2, 1};
constexpr uint64_t kInitialBacktrackLimit = 500;
constexpr float kRetryGrowthFactor = 1.1f;

// How many restarts must already have happened before Backtrack's
// large-domain branch starts shuffling word candidates instead of using
// dict_.ScoreOrder's strict best-first order (see the class comment's
// "restart" section in solver.hpp). Every one of this dictionary's word
// lengths has well over 1000 candidates (see docs/design.md), so a fresh
// slot's *first* branch in any component -- on any grid, not just a
// wide-open one -- routinely starts in that large-domain branch: gating
// purely on domain size (an earlier version of this fix) still shuffled on
// essentially every restart of every grid, and regressed grid_328.txt from
// solved in 0.83s to a 20s timeout. Restart *count* is a much better signal
// for "genuinely stuck, diversify harder": grid_328.txt/grid_053.txt/
// grid_058.txt (all in the benchmark corpus) solve within 3-17 restarts on
// the unmodified solver, while the motivating wide-open 7x7 case needed
// dozens. 20 sits comfortably above the former and well before the latter.
//
// Only applies to attempt_offset_ == 0 (worker 0, or a plain single-
// threaded Solve() call) -- see attempt_offset_'s comment in solver.hpp for
// why every other SolveParallel worker skips this gate and shuffles from
// its own local attempt 0.
constexpr uint64_t kWordShuffleRestartThreshold = 20;
}  // namespace

Solver::Solver(const Grid& grid, const Dictionary& dict)
    : grid_(grid), dict_(dict), crossings_by_slot_(grid_.slots().size()) {
  slot_length_.assign(grid_.slots().size(), 0);
  for (const Slot& slot : grid_.slots()) {
    slot_length_[static_cast<size_t>(slot.id)] = slot.length;
    slots_by_length_[slot.length].push_back(slot.id);
    max_length_ = std::max(max_length_, slot.length);
  }
  filter_scratch_by_length_.resize(static_cast<size_t>(max_length_) + 1);
  nogood_forbidden_scratch_by_length_.resize(static_cast<size_t>(max_length_) + 1);
  candidate_scratch_by_length_.resize(static_cast<size_t>(max_length_) + 1);
  for (const auto& [length, ids] : slots_by_length_) {
    filter_scratch_by_length_[static_cast<size_t>(length)] =
        WordBitset(dict_.NumWordsOfLength(length), false);
    nogood_forbidden_scratch_by_length_[static_cast<size_t>(length)] =
        WordBitset(dict_.NumWordsOfLength(length), false);
    candidate_scratch_by_length_[static_cast<size_t>(length)] =
        WordBitset(dict_.NumWordsOfLength(length), false);
  }
  in_queue_scratch_.assign(grid_.slots().size(), false);
  active_queue_scratch_.reserve(grid_.slots().size());
  queued_count_scratch_.assign(grid_.slots().size(), 0);
  last_saved_epoch_.assign(grid_.slots().size(), 0);
  snapshot_pool_by_length_.resize(static_cast<size_t>(max_length_) + 1);
  const std::vector<Crossing>& crossings = grid_.crossings();
  for (size_t i = 0; i < crossings.size(); ++i) {
    const Crossing& cr = crossings[i];
    int crossing_id = static_cast<int>(i);
    int length_a = slot_length_[static_cast<size_t>(cr.slot_a)];
    int length_b = slot_length_[static_cast<size_t>(cr.slot_b)];
    crossings_by_slot_[static_cast<size_t>(cr.slot_a)].push_back(
        {cr.slot_b, cr.offset_a, cr.offset_b, crossing_id, length_b});
    crossings_by_slot_[static_cast<size_t>(cr.slot_b)].push_back(
        {cr.slot_a, cr.offset_b, cr.offset_a, crossing_id, length_a});
  }

  // Connected components of the slot-crossing graph, via one BFS pass --
  // O(slots + crossings). See the slots_by_component_ comment in
  // solver.hpp.
  component_of_slot_.assign(grid_.slots().size(), -1);
  for (const Slot& start : grid_.slots()) {
    if (component_of_slot_[static_cast<size_t>(start.id)] != -1) continue;
    int component = static_cast<int>(slots_by_component_.size());
    slots_by_component_.emplace_back();

    std::vector<int> queue{start.id};
    component_of_slot_[static_cast<size_t>(start.id)] = component;
    for (size_t head = 0; head < queue.size(); ++head) {
      int sid = queue[head];
      slots_by_component_[static_cast<size_t>(component)].push_back(sid);
      for (const SlotCrossing& sc : crossings_by_slot_[static_cast<size_t>(sid)]) {
        if (component_of_slot_[static_cast<size_t>(sc.neighbor)] == -1) {
          component_of_slot_[static_cast<size_t>(sc.neighbor)] = component;
          queue.push_back(sc.neighbor);
        }
      }
    }
  }
}

bool Solver::BuildInitialDomains(std::vector<WordBitset>& domains,
                                  CrossingWeights& crossing_weights) const {
  domains.assign(grid_.slots().size(), WordBitset());
  for (const Slot& slot : grid_.slots()) {
    WordBitset& domain = domains[static_cast<size_t>(slot.id)];
    // Restrict to this slot's direction up front, once: AC-3 propagation
    // and every later narrowing step only ever shrink a domain (see
    // Propagate below), never add bits back, so excluding the other
    // direction's disallowed words here keeps them out for the rest of the
    // search without any per-direction awareness anywhere else in this
    // file. See Dictionary::AllowedMask's doc comment.
    domain = dict_.AllowedMask(slot.length, slot.dir == Direction::Across);
    // Seeded/pre-filled cells (see Grid::FromSpec) narrow this slot's
    // domain before propagation or search ever runs, the same way a
    // crossing constraint would once a neighbor is assigned -- just
    // known up front instead of discovered mid-search.
    for (size_t k = 0; k < slot.cells.size(); ++k) {
      char letter = grid_.PrefilledLetter(slot.cells[k]);
      if (letter != '\0') {
        domain &= dict_.LetterMask(slot.length, static_cast<int>(k), letter);
      }
    }
  }

  // Catch domains that start empty (e.g. no dictionary word of that
  // length) even when the owning slot has no crossings to narrow it --
  // Propagate only ever visits slots reachable from a seed via crossings,
  // so an isolated slot's domain would otherwise go unchecked.
  for (const WordBitset& domain : domains) {
    if (!domain.Any()) return false;
  }

  std::vector<int> all_slots;
  all_slots.reserve(grid_.slots().size());
  for (const Slot& slot : grid_.slots()) all_slots.push_back(slot.id);

  Trail root_trail;
  bool changed = true;
  while (changed) {
    changed = false;
    if (!Propagate(domains, all_slots, root_trail, next_save_epoch_++, crossing_weights)) {
      return false;
    }
    root_trail.domains.clear();  // one-time pass -- nothing to undo to
    if (!EnforceUniqueWordsOnce(domains, changed)) return false;
  }
  return true;
}

std::optional<Solution> Solver::Solve(uint64_t attempt_offset,
                                       const std::atomic<bool>* cancel,
                                       bool unlimited_budget) {
  cancel_ = cancel;
  attempt_offset_ = attempt_offset;
  std::vector<WordBitset> domains;
  CrossingWeights crossing_weights(grid_.crossings().size());
  if (!BuildInitialDomains(domains, crossing_weights)) return std::nullopt;

  std::vector<WordBitset> used_by_length(static_cast<size_t>(max_length_) + 1);
  for (const auto& [length, ids] : slots_by_length_) {
    used_by_length[static_cast<size_t>(length)] =
        WordBitset(dict_.NumWordsOfLength(length), false);
  }

  // Randomized restarts (ingrid_core-derived, see the class comment in
  // solver.hpp): each attempt starts fresh from the post-root-propagation
  // domains above, with a newly-seeded RNG for slot-selection tie-breaks,
  // but keeps the *same* crossing_weights -- what dom/wdeg has learned
  // about which crossings are troublesome carries over even though the
  // search tree itself restarts. An attempt that racks up more than
  // attempt_backtrack_limit_ dead ends aborts (Backtrack sets aborted_)
  // rather than continuing to grind on what may be a heavy-tailed bad
  // draw; the limit then grows geometrically for the next attempt. An
  // attempt that exhausts the search space *without* hitting the limit is
  // definitive -- solved or genuinely unsatisfiable -- so only that case
  // returns from the loop.
  //
  // `unlimited_budget`: never restart at all -- attempt 0 just keeps
  // going, however long it takes, until it's genuinely exhaustive.
  // EXPERIMENTAL: restarts trade a guarantee of eventual completion for
  // expected-case speed (Gomes/Selman/Kautz's heavy-tailed-runtime
  // argument), which is the right trade for *finding* a solution fast but
  // the wrong one for *proving none exists* -- each restart discards
  // almost all of that attempt's progress toward a genuinely exhaustive
  // pass, and grid_072.txt/grid_217.txt (real, scraped, actually
  // unsatisfiable at min_score=40) showed this directly: neither ever
  // completed one in 90 minutes of nothing but restarting. A single
  // uninterrupted DFS is still complete either way, just with less
  // variance and no risk of restart-thrashing forever short of a genuine
  // exhaustive pass.
  attempt_backtrack_limit_ =
      unlimited_budget ? std::numeric_limits<uint64_t>::max() : kInitialBacktrackLimit;
  for (uint64_t attempt = 0;; ++attempt) {
    // Checked here too (not just in Backtrack, once per node) so a
    // worker that's cancelled between attempts doesn't even start
    // another one -- relevant for SolveParallel, where cancel_ is set
    // once some other worker has already found a solution.
    if (cancel_ != nullptr && cancel_->load(std::memory_order_relaxed)) {
      return std::nullopt;
    }
    aborted_ = false;
    attempt_backtracks_ = 0;
    // The *global* attempt number (offset + local attempt), not just the
    // local one: SolveParallel gives every worker but the first a
    // nonzero attempt_offset specifically so its own local attempt 0 is
    // already randomized, instead of every worker wastefully repeating
    // worker 0's identical deterministic first pass. Worker 0's
    // offset is 0, so this is exactly today's single-threaded sequence.
    uint64_t global_attempt = attempt_offset + attempt;
    randomize_slot_choice_ = global_attempt > 0;
    rng_.seed(global_attempt);

    std::vector<WordBitset> attempt_domains = domains;
    std::vector<WordBitset> attempt_used_by_length = used_by_length;
    std::vector<bool> assigned(grid_.slots().size(), false);
    Trail trail;

    component_remaining_.resize(slots_by_component_.size());
    for (size_t c = 0; c < slots_by_component_.size(); ++c) {
      component_remaining_[c] = static_cast<int>(slots_by_component_[c].size());
    }

    auto result = Backtrack(attempt_domains, attempt_used_by_length, assigned,
                             trail, crossing_weights);
    if (result) return result;
    if (!aborted_) return std::nullopt;

    stats_.restarts++;
    if (std::getenv("XFILL_DEBUG_RESTARTS")) {
      std::cerr << "restart " << stats_.restarts
                << " total_backtracks=" << stats_.backtracks
                << " next_limit=" << attempt_backtrack_limit_ << "\n";
    }
    // A float->uint64_t cast is undefined behavior once the float value
    // exceeds uint64_t's representable range (confirmed via UBSan: a test
    // with a trivially small search space can restart often enough,
    // rapidly enough, for kRetryGrowthFactor's geometric growth to reach
    // that range well within a test run). No real search benefits from a
    // budget anywhere near UINT64_MAX backtracks anyway -- clamp instead
    // of letting the cast overflow.
    float grown = static_cast<float>(attempt_backtrack_limit_) * kRetryGrowthFactor;
    uint64_t grown_limit = grown >= static_cast<float>(std::numeric_limits<uint64_t>::max())
                                ? std::numeric_limits<uint64_t>::max()
                                : static_cast<uint64_t>(grown);
    attempt_backtrack_limit_ = std::max<uint64_t>(attempt_backtrack_limit_ + 1, grown_limit);
  }
}

ParallelSolveResult Solver::SolveParallel(const Grid& grid, const Dictionary& dict,
                                           unsigned num_threads,
                                           std::function<void(uint64_t)> on_progress) {
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 1;
  }

  // One fully independent Solver per worker -- own domains, trail,
  // crossing weights, nogoods, RNG, all of it -- so there is nothing
  // search-related shared between threads and so nothing to
  // synchronize inside the hot path. Redundant construction-time work
  // (crossings_by_slot_, slots_by_component_, etc., each O(slots +
  // crossings)) is negligible next to the search itself.
  std::vector<std::unique_ptr<Solver>> solvers;
  solvers.reserve(num_threads);
  for (unsigned i = 0; i < num_threads; ++i) {
    solvers.push_back(std::make_unique<Solver>(grid, dict));
  }

  // See SharedCrossingWeights' comment in solver.hpp: one instance shared
  // by every *restart-based* worker, so a crossing several of them
  // independently struggle with gets deprioritized everywhere, not just
  // in whichever worker hit it first. Wired in before any thread starts
  // (never touched again by this function itself), so there's nothing to
  // synchronize about the pointer assignment below -- only the counts
  // inside it are ever written concurrently, and those are already
  // atomic. Gated on num_threads > 1, same as unlimited_budget below and
  // for the same reason: a lone worker has nobody to share with, and
  // wiring it up anyway would perturb SlotWeight's sum (an extra
  // +get(id)-1.0f term on every crossing) even with a single writer,
  // breaking the num_threads=1 reproducibility `bench_subset.py
  // --threads 1` and this project's other single-threaded comparisons
  // depend on.
  //
  // The dedicated unlimited_budget worker (the last one, see below) is
  // deliberately excluded: its whole value is a single, uninterrupted
  // trajectory that's guaranteed to eventually reach a genuine exhaustive
  // conclusion, with no restart to recover from a bad branch. Confirmed
  // directly this costs it real cases: wiring it in regressed
  // `grid_115.txt` from reliably solved (~6-7s via this exact worker) to
  // consistently timing out, across repeated runs -- the restart-based
  // workers' own experience, however genuinely troublesome for *their*
  // randomized paths, isn't necessarily relevant to this worker's
  // deterministic-ish one, and unlike them it has no restart to shake off
  // a bad nudge if it turns out not to be.
  // XFILL_DISABLE_SHARED_WEIGHTS is a benchmarking-only escape hatch (for
  // the URTC testbench's ablation, isolating this mechanism's effect from
  // everything else SolveParallel does) -- unset, the default, changes
  // nothing about normal behavior.
  bool shared_weights_disabled = std::getenv("XFILL_DISABLE_SHARED_WEIGHTS") != nullptr;
  SharedCrossingWeights shared_crossing_weights(grid.crossings().size());
  if (num_threads > 1 && !shared_weights_disabled) {
    for (unsigned i = 0; i + 1 < num_threads; ++i) {
      solvers[i]->shared_crossing_weights_ = &shared_crossing_weights;
    }
  }

  // Shared node counter + monitor thread for on_progress (see its doc
  // comment in solver.hpp): every worker adds to `total_nodes` as it
  // visits nodes (Backtrack's try_candidate, gated on
  // global_node_counter_ being non-null so this costs nothing when no
  // callback was requested), and this dedicated thread -- never a worker
  // itself -- polls it on a wall-clock interval and reports. Polling
  // instead of an exact-node-count trigger sidesteps needing any
  // synchronization among workers to agree on who reports which boundary.
  //
  // Waits on a condition variable rather than plain sleep_for: a solve
  // that finishes well inside one interval still has to let this thread's
  // *current* wait actually end before SolveParallel can join and return
  // it, and a sleep_for can't be woken early -- confirmed directly, a
  // trivial 2x2 grid that used to solve in ~0.0003s took 0.155s once this
  // was wired in with plain sleep_for, all of it this thread finishing
  // out a stale wait nobody needed anymore. notify_one() below wakes it
  // immediately once solving is done instead.
  std::atomic<uint64_t> total_nodes{0};
  bool stop_monitor = false;
  std::mutex monitor_mutex;
  std::condition_variable monitor_cv;
  std::thread monitor_thread;
  if (on_progress) {
    for (auto& solver : solvers) solver->global_node_counter_ = &total_nodes;
    monitor_thread = std::thread([&]() {
      std::unique_lock<std::mutex> lock(monitor_mutex);
      while (!monitor_cv.wait_for(lock, std::chrono::milliseconds(150),
                                   [&] { return stop_monitor; })) {
        on_progress(total_nodes.load(std::memory_order_relaxed));
      }
    });
  }

  std::atomic<bool> cancel{false};
  std::mutex result_mutex;
  std::optional<Solution> winning_solution;
  SolverStats winning_stats;

  // Comfortably larger than any realistic restart count, so no two
  // workers' attempt-number ranges can ever collide (see Solve()'s
  // global_attempt and the class comment above for why each worker
  // needs a distinct range at all).
  constexpr uint64_t kAttemptStride = uint64_t{1} << 40;

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (unsigned i = 0; i < num_threads; ++i) {
    // Dedicate exactly one worker (the last one, whenever there's more
    // than one to spare) to unlimited_budget: a single continuous DFS
    // that never restarts, trading the other workers' restart-based
    // expected-case speedup for a guarantee of eventually reaching a
    // genuine exhaustive conclusion (see Solve()'s unlimited_budget doc
    // comment). Real, scraped grids exist (grid_072.txt, grid_217.txt)
    // that no number of purely restart-based workers resolved in 90
    // minutes of `SolveParallel` -- confirmed directly: a standalone
    // unlimited_budget run on grid_072.txt proved it exhaustively
    // UNSAT in 84 minutes with zero restarts, something the restart
    // portfolio alone never achieved on either grid. `num_threads > 1`
    // keeps a plain 1-thread call reproducing today's exact
    // single-threaded sequence (see the "SolveParallel with 1 thread"
    // test), rather than silently changing its meaning.
    bool unlimited_budget = num_threads > 1 && i == num_threads - 1;
    threads.emplace_back([&, i, unlimited_budget]() {
      auto solution = solvers[i]->Solve(static_cast<uint64_t>(i) * kAttemptStride,
                                         &cancel, unlimited_budget);
      bool expected = false;
      if (solution) {
        // First solution to arrive here wins; cancel tells every other
        // worker to unwind (checked once per node in Backtrack). A loser
        // of this race still has a genuine solution, just not the one
        // reported -- any solution is as good as any other, so it's
        // simply discarded rather than compared.
        if (cancel.compare_exchange_strong(expected, true)) {
          std::lock_guard<std::mutex> lock(result_mutex);
          winning_solution = std::move(solution);
          winning_stats = solvers[i]->stats();
        }
        return;
      }
      // A nullopt *not* itself forced by another worker's cancellation
      // (the CAS below only succeeds the first time this happens) is a
      // genuine, sound proof this grid has no solution: every worker's
      // last attempt -- restart-based or unlimited_budget -- is a
      // complete, uninterrupted DFS from the same root-propagated
      // domains, so any one of them reaching that terminal state without
      // being cut short settles the question for the whole grid. No
      // need to wait for every other worker to separately rediscover the
      // same fact -- the real payoff on a grid like grid_072.txt, where
      // restart-based workers can run for 90+ minutes without ever
      // reaching it on their own.
      cancel.compare_exchange_strong(expected, true);
    });
  }
  for (std::thread& t : threads) t.join();

  if (on_progress) {
    {
      std::lock_guard<std::mutex> lock(monitor_mutex);
      stop_monitor = true;
    }
    monitor_cv.notify_one();
    monitor_thread.join();
    // One last report with the true final total -- the monitor thread's
    // own last iteration could have read total_nodes anywhere up to
    // 150ms before every worker actually finished.
    on_progress(total_nodes.load(std::memory_order_relaxed));
  }

  ParallelSolveResult result;
  result.num_threads = num_threads;
  if (winning_solution) {
    result.solution = std::move(winning_solution);
    result.stats = winning_stats;
  } else {
    // `cancel` only ever gets set inside the `if (solution)` branch
    // above, so if we get here it was never set at all -- every worker's
    // Solve() ran to its own natural completion (a fully exhausted,
    // non-aborted attempt) rather than being cut off early, so each one
    // independently proved the grid unsatisfiable. Sum their stats for
    // a representative "total work" figure, since there's no single
    // "the" search to report in this case.
    for (const auto& solver : solvers) {
      result.stats.nodes += solver->stats().nodes;
      result.stats.backtracks += solver->stats().backtracks;
      result.stats.restarts += solver->stats().restarts;
    }
  }
  return result;
}

void Solver::MaximizeBacktrack(std::vector<WordBitset>& domains,
                                std::vector<WordBitset>& used_by_length,
                                std::vector<bool>& assigned, Trail& trail,
                                CrossingWeights& crossing_weights, int64_t current_score,
                                std::atomic<int64_t>& shared_best_score,
                                const std::function<void(const Solution&, int64_t)>& on_improved,
                                std::mutex& callback_mutex, const std::atomic<bool>* cancel) {
  if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return;
  if (global_node_counter_ != nullptr) {
    global_node_counter_->fetch_add(1, std::memory_order_relaxed);
  }

  // Upper bound: score already committed, plus the best score any word
  // still in each unassigned slot's domain could contribute. This only
  // ever shrinks as the recursion goes deeper (domains only narrow, never
  // widen -- see Propagate), so a branch pruned here can never become
  // worth exploring later within the same trajectory.
  int64_t bound = current_score;
  for (const Slot& s : grid_.slots()) {
    int sid = s.id;
    if (assigned[static_cast<size_t>(sid)]) continue;
    bound += dict_.BestScoreInDomain(slot_length_[static_cast<size_t>(sid)],
                                      domains[static_cast<size_t>(sid)]);
  }
  if (bound <= shared_best_score.load(std::memory_order_relaxed)) return;

  int slot = SelectBranchSlot(domains, used_by_length, assigned, crossing_weights, nullptr);
  if (slot == -1) {
    // Complete, valid assignment -- current_score is its true total.
    // Compare-and-swap loop: only the worker that actually wins the race
    // to install a new best reports it, but every worker's improvement
    // attempt is still checked against whatever the winner just set, not
    // a stale value read before this loop started.
    int64_t expected = shared_best_score.load(std::memory_order_relaxed);
    while (current_score > expected) {
      if (shared_best_score.compare_exchange_weak(expected, current_score)) {
        Solution sol = ExtractSolution(domains);
        std::lock_guard<std::mutex> lock(callback_mutex);
        on_improved(sol, current_score);
        break;
      }
      // compare_exchange_weak already refreshed `expected` on failure.
    }
    return;
  }

  int length = slot_length_[static_cast<size_t>(slot)];
  const WordBitset& slot_domain = domains[static_cast<size_t>(slot)];
  WordBitset candidates = slot_domain;
  candidates.AndNotAssign(used_by_length[static_cast<size_t>(length)]);

  // Descending score order (same ScoreOrder the plain search already
  // uses): tries the highest-value completions first, so a worker tends
  // to report good totals early rather than needing to reach a
  // high-scoring leaf by chance -- the anytime-quality property this
  // whole feature depends on, not just eventual correctness.
  for (size_t idx : dict_.ScoreOrder(length)) {
    if (!candidates.Test(idx)) continue;
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return;
    size_t domain_mark = trail.domains.size();
    size_t used_mark = trail.used.size();
    int word_score = dict_.WordScore(length, idx);
    if (Assign(slot, idx, domains, used_by_length, assigned, trail, crossing_weights)) {
      MaximizeBacktrack(domains, used_by_length, assigned, trail, crossing_weights,
                         current_score + word_score, shared_best_score, on_improved,
                         callback_mutex, cancel);
    }
    Undo(slot, domains, used_by_length, assigned, trail, domain_mark, used_mark);
  }
}

void Solver::MaximizeSearchOneWorker(
    std::atomic<int64_t>& shared_best_score,
    const std::function<void(const Solution&, int64_t)>& on_improved, std::mutex& callback_mutex,
    const std::atomic<bool>* cancel, bool randomize, uint64_t seed,
    std::atomic<uint64_t>* global_node_counter) {
  cancel_ = cancel;
  global_node_counter_ = global_node_counter;
  randomize_slot_choice_ = randomize;
  rng_.seed(seed);

  std::vector<WordBitset> domains;
  CrossingWeights crossing_weights(grid_.crossings().size());
  if (!BuildInitialDomains(domains, crossing_weights)) return;  // proves unsatisfiable at the root

  std::vector<WordBitset> used_by_length(static_cast<size_t>(max_length_) + 1);
  for (const auto& [length, ids] : slots_by_length_) {
    used_by_length[static_cast<size_t>(length)] = WordBitset(dict_.NumWordsOfLength(length), false);
  }
  std::vector<bool> assigned(grid_.slots().size(), false);
  Trail trail;

  component_remaining_.resize(slots_by_component_.size());
  for (size_t c = 0; c < slots_by_component_.size(); ++c) {
    component_remaining_[c] = static_cast<int>(slots_by_component_[c].size());
  }

  MaximizeBacktrack(domains, used_by_length, assigned, trail, crossing_weights, 0,
                     shared_best_score, on_improved, callback_mutex, cancel);
}

std::optional<Solution> Solver::MaximizeScoreParallel(
    const Grid& grid, const Dictionary& dict, unsigned num_threads,
    std::function<void(const Solution&, int64_t)> on_improved, const std::atomic<bool>* cancel,
    std::function<void(uint64_t)> on_progress) {
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 1;
  }

  std::vector<std::unique_ptr<Solver>> solvers;
  solvers.reserve(num_threads);
  for (unsigned i = 0; i < num_threads; ++i) {
    solvers.push_back(std::make_unique<Solver>(grid, dict));
  }

  // -1 sentinel ("nothing found yet") rather than 0: an all-zero-score
  // dictionary is a legitimate (if unusual) input, and a real total of 0
  // must still be able to register as the first improvement.
  std::atomic<int64_t> shared_best_score{-1};
  std::mutex callback_mutex;
  std::optional<Solution> best_solution;  // guarded by callback_mutex

  auto wrapped_on_improved = [&](const Solution& sol, int64_t score) {
    best_solution = sol;  // called with callback_mutex already held, see MaximizeBacktrack
    on_improved(sol, score);
  };

  // Same shared-node-counter + polling-monitor-thread pattern as
  // SolveParallel's on_progress (see its comment there for the full
  // rationale) -- duplicated rather than factored out, since the two
  // callers' surrounding setup (worker construction, what each worker's
  // thread lambda actually calls) differs enough that sharing just this
  // middle section would need its own parameter list nearly as long as
  // either caller's, without meaningfully reducing the code on either
  // side.
  std::atomic<uint64_t> total_nodes{0};
  bool stop_monitor = false;
  std::mutex monitor_mutex;
  std::condition_variable monitor_cv;
  std::thread monitor_thread;
  if (on_progress) {
    monitor_thread = std::thread([&]() {
      std::unique_lock<std::mutex> lock(monitor_mutex);
      while (!monitor_cv.wait_for(lock, std::chrono::milliseconds(150),
                                   [&] { return stop_monitor; })) {
        on_progress(total_nodes.load(std::memory_order_relaxed));
      }
    });
  }

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (unsigned i = 0; i < num_threads; ++i) {
    threads.emplace_back([&, i]() {
      solvers[i]->MaximizeSearchOneWorker(shared_best_score, wrapped_on_improved, callback_mutex,
                                           cancel, /*randomize=*/i != 0, /*seed=*/i,
                                           on_progress ? &total_nodes : nullptr);
    });
  }
  for (std::thread& t : threads) t.join();

  if (on_progress) {
    {
      std::lock_guard<std::mutex> lock(monitor_mutex);
      stop_monitor = true;
    }
    monitor_cv.notify_one();
    monitor_thread.join();
    on_progress(total_nodes.load(std::memory_order_relaxed));
  }

  return best_solution;
}

void Solver::SaveDomainOnce(int slot, const std::vector<WordBitset>& domains,
                             Trail& trail, uint64_t epoch) const {
  if (last_saved_epoch_[static_cast<size_t>(slot)] == epoch) return;
  last_saved_epoch_[static_cast<size_t>(slot)] = epoch;

  int length = slot_length_[static_cast<size_t>(slot)];
  std::vector<WordBitset>& pool = snapshot_pool_by_length_[static_cast<size_t>(length)];
  if (pool.empty()) {
    trail.domains.push_back({slot, domains[static_cast<size_t>(slot)]});
    return;
  }
  // Reuse a previously-freed buffer of the same length (same word count,
  // so same size() -- assigning into it is an in-place copy, no
  // reallocation) instead of heap-allocating a fresh WordBitset.
  WordBitset recycled = std::move(pool.back());
  pool.pop_back();
  recycled = domains[static_cast<size_t>(slot)];
  trail.domains.push_back({slot, std::move(recycled)});
}

bool Solver::Propagate(std::vector<WordBitset>& domains,
                        const std::vector<int>& seed_slots, Trail& trail,
                        uint64_t epoch,
                        CrossingWeights& crossing_weights) const {
  std::vector<bool>& in_queue = in_queue_scratch_;
  std::vector<int>& touched = queue_touched_scratch_;
  // Caches domains[s].Count() at the moment `s` is (re-)enqueued, valid
  // for as long as `s` stays queued: a queued slot's domain only ever
  // changes here in Propagate, and every such change is immediately
  // followed by a call to enqueue() for that slot (or a contradiction
  // return) -- so the cached count is never stale while in_queue[s] is
  // true. Without this, the min-domain scan below recomputed Count() (an
  // O(chunks) popcount) for every still-queued slot on every single pop,
  // even slots whose domain hadn't changed since the last pass -- O(Q)
  // redundant recomputation per pop, O(Q^2) over a queue that takes Q pops
  // to drain.
  std::vector<size_t>& queued_count = queued_count_scratch_;
  // Flat, unordered list of currently-queued slot ids -- lets the min-scan
  // below touch only slots actually in the queue (typically far fewer
  // than the grid's total slot count) instead of scanning `in_queue` for
  // all of them. A *sorted* version of this was tried before and found a
  // wash (the O(S) bool-array scan it replaced was already cheap, and
  // keeping the vector sorted on every insert cost as much as it saved);
  // this one stays unsorted -- O(1) push on enqueue, O(1) swap-remove on
  // pop, since the min-scan already finds the popped slot's position in
  // the same pass that finds its value -- so there's no equivalent
  // maintenance cost to give the earlier attempt's wash its regression.
  std::vector<int>& active = active_queue_scratch_;
  active.clear();
  auto enqueue = [&](int s) {
    if (!in_queue[static_cast<size_t>(s)]) {
      touched.push_back(s);
      active.push_back(s);
    }
    in_queue[static_cast<size_t>(s)] = true;
    queued_count[static_cast<size_t>(s)] = domains[static_cast<size_t>(s)].Count();
  };
  touched.clear();
  // Every entry left `true` here gets reset before this function returns,
  // by whichever path it returns through -- including the early-return-on-
  // contradiction case below, where the queue may still hold un-popped
  // slots -- so the scratch buffer is always all-false again on the next
  // call.
  for (int s : seed_slots) enqueue(s);

  while (true) {
    // Pop the queued slot with the smallest domain -- checking wipeouts
    // (via SelectBranchSlot/the candidate loop, not here) happens sooner
    // when the most-constrained slots propagate first. Ties broken by
    // lowest slot id explicitly (matching the original full-array scan's
    // tie-break, which always found the lowest-indexed slot first) since
    // `active`'s order is insertion order, not slot-id order.
    int slot = -1;
    size_t best_count = 0;
    size_t best_pos = 0;
    for (size_t pos = 0; pos < active.size(); ++pos) {
      int s = active[pos];
      size_t count = queued_count[static_cast<size_t>(s)];
      if (slot == -1 || count < best_count || (count == best_count && s < slot)) {
        slot = s;
        best_count = count;
        best_pos = pos;
      }
    }
    if (slot == -1) break;
    in_queue[static_cast<size_t>(slot)] = false;
    active[best_pos] = active.back();
    active.pop_back();

    int length = slot_length_[static_cast<size_t>(slot)];
    const WordBitset& slot_domain = domains[static_cast<size_t>(slot)];

    // For a domain narrowed below this many candidates, it's cheaper to
    // read their actual letters directly than to ask "is any word with
    // letter C at this position still in the domain?" 26 times -- each
    // such Intersects() call walks the underlying bitset's full chunk
    // array regardless of how few bits are set (it can only early-exit on
    // finding an overlap, not on running out of set bits), so 26 of them
    // cost roughly 26x one chunk-array pass, while direct lookup costs
    // one chunk-array pass (via SetBits()) plus a handful of O(1) string
    // reads. `best_count` is already known from the queue-selection step
    // above, so checking it here costs nothing extra. 1000 was picked by
    // re-running benchmarks/bench_subset.py against several values (16,
    // 200, 1000) on the same 20-grid sample and taking the plateau --
    // not derived analytically, per this project's benchmarking
    // philosophy (see docs/design.md).
    constexpr size_t kDirectLookupThreshold = 1000;
    const std::vector<std::string>* slot_words = nullptr;
    if (best_count <= kDirectLookupThreshold) {
      slot_candidates_scratch_.clear();
      slot_domain.AppendSetBits(slot_candidates_scratch_, best_count);
      slot_words = &dict_.WordsOfLength(length);
    }

    for (const SlotCrossing& sc : crossings_by_slot_[static_cast<size_t>(slot)]) {
      // Which letters are still viable at this crossing position, given
      // the slot's current domain?
      uint32_t possible = 0;
      if (slot_words != nullptr) {
        for (size_t idx : slot_candidates_scratch_) {
          char ch = (*slot_words)[idx][static_cast<size_t>(sc.my_offset)];
          possible |= (1u << (ch - 'A'));
        }
      } else {
        for (int c = 0; c < 26 && possible != kAllLettersMask; ++c) {
          if (slot_domain.Intersects(dict_.LetterMask(length, sc.my_offset,
                                                        static_cast<char>('A' + c)))) {
            possible |= (1u << c);
          }
        }
      }
      if (possible == kAllLettersMask) continue;  // no constraint to apply

      int neighbor_length = sc.neighbor_length;
      WordBitset& neighbor_domain = domains[static_cast<size_t>(sc.neighbor)];

      // A single viable letter is common (especially once domains have
      // narrowed deep in the search), and needs no union at all: the
      // "filter" is just that one letter's mask, so this intersects the
      // neighbor's domain against it directly, skipping the copy into
      // filter_scratch_by_length_ entirely (there's nothing to union).
      const WordBitset* filter_ptr;
      if ((possible & (possible - 1)) == 0) {
        int c = __builtin_ctz(possible);
        filter_ptr = &dict_.LetterMask(neighbor_length, sc.neighbor_offset,
                                        static_cast<char>('A' + c));
      } else {
        WordBitset& filter = filter_scratch_by_length_[static_cast<size_t>(neighbor_length)];
        bool first_mask = true;
        for (int c = 0; c < 26; ++c) {
          if (!(possible & (1u << c))) continue;
          const WordBitset& mask =
              dict_.LetterMask(neighbor_length, sc.neighbor_offset, static_cast<char>('A' + c));
          if (first_mask) {
            filter = mask;
            first_mask = false;
          } else {
            filter |= mask;
          }
        }
        filter_ptr = &filter;
      }
      const WordBitset& filter = *filter_ptr;

      // The neighbor's domain only shrinks over the life of the search, so
      // if it's already a subset of the filter, intersecting would be a
      // no-op -- skip the snapshot and the write.
      if (neighbor_domain.IsSubsetOf(filter)) continue;

      SaveDomainOnce(sc.neighbor, domains, trail, epoch);
      size_t new_count = neighbor_domain.AndAssignCount(filter);
      if (new_count == 0) {
        crossing_weights.Bump(sc.crossing_id);
        if (shared_crossing_weights_ != nullptr) {
          shared_crossing_weights_->Bump(sc.crossing_id);
        }
        for (int t : touched) in_queue[static_cast<size_t>(t)] = false;
        return false;
      }

      // Same bookkeeping as enqueue(), but the count is already known
      // from the fused intersect-and-count above, so there's no need to
      // ask the just-narrowed domain to recompute it.
      if (!in_queue[static_cast<size_t>(sc.neighbor)]) {
        touched.push_back(sc.neighbor);
        active.push_back(sc.neighbor);
      }
      in_queue[static_cast<size_t>(sc.neighbor)] = true;
      queued_count[static_cast<size_t>(sc.neighbor)] = new_count;
    }
  }
  return true;
}

bool Solver::EnforceUniqueWordsOnce(std::vector<WordBitset>& domains,
                                     bool& changed) const {
  for (const auto& [length, slot_ids] : slots_by_length_) {
    for (int owner : slot_ids) {
      WordBitset& owner_domain = domains[static_cast<size_t>(owner)];
      if (owner_domain.Count() != 1) continue;
      size_t idx = owner_domain.First();

      for (int other : slot_ids) {
        if (other == owner) continue;
        WordBitset& other_domain = domains[static_cast<size_t>(other)];
        if (!other_domain.Test(idx)) continue;
        other_domain.Clear(idx);
        if (!other_domain.Any()) return false;
        changed = true;
      }
    }
  }
  return true;
}

float Solver::SlotWeight(int slot, const CrossingWeights& crossing_weights,
                          const std::vector<bool>& assigned) const {
  float total = 0.0f;
  for (const SlotCrossing& sc : crossings_by_slot_[static_cast<size_t>(slot)]) {
    if (!assigned[static_cast<size_t>(sc.neighbor)]) {
      float w = crossing_weights.Get(sc.crossing_id);
      // Both Get()s share the same baseline (1.0f means "never bumped"),
      // so adding them raw would double-count it -- subtract one copy.
      // Null for a plain single-threaded call (see shared_crossing_weights_'s
      // comment in solver.hpp), so this is a no-op then.
      if (shared_crossing_weights_ != nullptr) {
        w += shared_crossing_weights_->Get(sc.crossing_id) - 1.0f;
      }
      total += w;
    }
  }
  return total > 0.0f ? total : 1.0f;
}

int Solver::ActiveComponent() const {
  for (size_t c = 0; c < component_remaining_.size(); ++c) {
    if (component_remaining_[c] > 0) return static_cast<int>(c);
  }
  return -1;
}

int Solver::SelectBranchSlot(const std::vector<WordBitset>& domains,
                              const std::vector<WordBitset>& used_by_length,
                              const std::vector<bool>& assigned,
                              const CrossingWeights& crossing_weights,
                              size_t* out_domain_count) const {
  int active = ActiveComponent();
  if (active == -1) return -1;

  std::vector<std::tuple<float, int, size_t>>& candidates = branch_candidates_scratch_;
  candidates.clear();

  for (int sid : slots_by_component_[static_cast<size_t>(active)]) {
    if (assigned[static_cast<size_t>(sid)]) continue;

    // Popcount of (domain & ~used) directly, without copying the domain
    // just to AndNot() it and Count() the result -- this runs once per
    // unassigned slot in the component on every single branching decision,
    // so the avoided allocation/copy adds up.
    size_t count = domains[static_cast<size_t>(sid)].CountAndNot(
        used_by_length[static_cast<size_t>(slot_length_[static_cast<size_t>(sid)])]);

    float weight = SlotWeight(sid, crossing_weights, assigned);
    candidates.push_back({static_cast<float>(count) / weight, sid, count});
  }
  if (candidates.empty()) return -1;

  // Weighted-random pick among the best few rather than always the single
  // best -- see the class comment in solver.hpp for why (restart
  // diversity).
  size_t top_n = randomize_slot_choice_ ? std::min(candidates.size(), kRandomTopN) : 1;
  std::partial_sort(candidates.begin(),
                     candidates.begin() + static_cast<long>(top_n),
                     candidates.end());

  std::discrete_distribution<size_t> dist(
      kRandomSlotWeights.begin(),
      kRandomSlotWeights.begin() + static_cast<long>(top_n));
  const auto& [priority, chosen_slot, chosen_count] = candidates[dist(rng_)];
  (void)priority;
  if (out_domain_count != nullptr) *out_domain_count = chosen_count;
  return chosen_slot;
}

bool Solver::Assign(int slot, size_t word_index,
                     std::vector<WordBitset>& domains,
                     std::vector<WordBitset>& used_by_length,
                     std::vector<bool>& assigned, Trail& trail,
                     CrossingWeights& crossing_weights) const {
  int length = slot_length_[static_cast<size_t>(slot)];
  uint64_t epoch = next_save_epoch_++;

  assigned[static_cast<size_t>(slot)] = true;
  --component_remaining_[static_cast<size_t>(component_of_slot_[static_cast<size_t>(slot)])];

  SaveDomainOnce(slot, domains, trail, epoch);
  WordBitset chosen(domains[static_cast<size_t>(slot)].size(), false);
  chosen.Set(word_index);
  domains[static_cast<size_t>(slot)] = chosen;

  trail.used.push_back({length, word_index});
  used_by_length[static_cast<size_t>(length)].Set(word_index);

  return Propagate(domains, {slot}, trail, epoch, crossing_weights);
}

void Solver::Undo(int slot, std::vector<WordBitset>& domains,
                   std::vector<WordBitset>& used_by_length,
                   std::vector<bool>& assigned, Trail& trail,
                   size_t domain_mark, size_t used_mark) const {
  assigned[static_cast<size_t>(slot)] = false;
  ++component_remaining_[static_cast<size_t>(component_of_slot_[static_cast<size_t>(slot)])];

  while (trail.used.size() > used_mark) {
    const UsedSnapshot& u = trail.used.back();
    used_by_length[static_cast<size_t>(u.length)].Clear(u.word_index);
    trail.used.pop_back();
  }
  while (trail.domains.size() > domain_mark) {
    DomainSnapshot& d = trail.domains.back();
    // The domain state about to be overwritten (the narrower one this
    // decision produced) is no longer needed -- hand its buffer to the
    // recycle pool instead of letting the move-assignment below free it,
    // so a future SaveDomainOnce for this length can reuse it.
    int length = slot_length_[static_cast<size_t>(d.slot)];
    snapshot_pool_by_length_[static_cast<size_t>(length)].push_back(
        std::move(domains[static_cast<size_t>(d.slot)]));
    domains[static_cast<size_t>(d.slot)] = std::move(d.domain);
    trail.domains.pop_back();
  }
}

void Solver::RecordNogoodFromDeadEnd(const std::vector<WordBitset>& domains,
                                      const std::vector<bool>& assigned) {
  Nogood nogood;
  for (const Slot& s : grid_.slots()) {
    if (!assigned[static_cast<size_t>(s.id)]) continue;
    nogood.pairs.push_back({s.id, domains[static_cast<size_t>(s.id)].First()});
  }
  if (std::getenv("XFILL_DEBUG_NOGOODS")) {
    std::cerr << "nogood depth=" << nogood.pairs.size() << "\n";
  }
  int nogood_idx = static_cast<int>(nogoods_.size());
  for (const auto& [s, w] : nogood.pairs) {
    nogoods_by_slot_[s].push_back(nogood_idx);
  }
  nogoods_.push_back(std::move(nogood));
}

const WordBitset* Solver::NogoodForbiddenWords(int slot,
                                                const std::vector<WordBitset>& domains,
                                                const std::vector<bool>& assigned) const {
  auto it = nogoods_by_slot_.find(slot);
  if (it == nogoods_by_slot_.end()) return nullptr;

  int length = slot_length_[static_cast<size_t>(slot)];
  WordBitset& forbidden = nogood_forbidden_scratch_by_length_[static_cast<size_t>(length)];
  bool any = false;
  for (int nogood_idx : it->second) {
    const Nogood& nogood = nogoods_[static_cast<size_t>(nogood_idx)];
    size_t forbidden_word = std::numeric_limits<size_t>::max();
    bool all_others_match = true;
    for (const auto& [s2, w2] : nogood.pairs) {
      if (s2 == slot) {
        forbidden_word = w2;
        continue;
      }
      if (!assigned[static_cast<size_t>(s2)] || !domains[static_cast<size_t>(s2)].Test(w2)) {
        all_others_match = false;
        break;
      }
    }
    if (all_others_match && forbidden_word != std::numeric_limits<size_t>::max()) {
      if (!any) {
        forbidden.ClearAll();
        any = true;
      }
      forbidden.Set(forbidden_word);
    }
  }
  return any ? &forbidden : nullptr;
}

std::optional<Solution> Solver::Backtrack(std::vector<WordBitset>& domains,
                                           std::vector<WordBitset>& used_by_length,
                                           std::vector<bool>& assigned,
                                           Trail& trail,
                                           CrossingWeights& crossing_weights) {
  if (aborted_) return std::nullopt;
  // Checked once per node, same cadence as aborted_ above (which this
  // deliberately mimics -- setting aborted_ here, rather than a separate
  // flag, reuses the existing unwind-without-recording-a-nogood path:
  // RecordNogoodFromDeadEnd only fires when this node's candidate loop
  // runs to genuine completion, which this early return skips entirely,
  // same as a budget-triggered abort). Only ever non-null for a worker
  // started by SolveParallel; a plain single-threaded Solve() call never
  // pays even this one relaxed atomic load, since cancel_ stays null.
  if (cancel_ != nullptr && cancel_->load(std::memory_order_relaxed)) {
    aborted_ = true;
    return std::nullopt;
  }

  size_t domain_count = 0;
  int slot = SelectBranchSlot(domains, used_by_length, assigned, crossing_weights,
                               &domain_count);
  if (slot == -1) {
    return ExtractSolution(domains);
  }

  // See kWordShuffleRestartThreshold: word-choice shuffling (the large-
  // domain branch below) only kicks in once plain slot-choice
  // randomization has already had a real chance to escape a stuck attempt
  // and hasn't -- not on every restart, which would touch essentially
  // every grid's very first branch decision.
  bool shuffle_words = randomize_slot_choice_ &&
                        (attempt_offset_ > 0 || stats_.restarts >= kWordShuffleRestartThreshold);

  int length = slot_length_[static_cast<size_t>(slot)];
  const WordBitset& slot_domain = domains[static_cast<size_t>(slot)];

  // Words already proven, by an earlier restart's fully-exhausted search,
  // to be a dead end given exactly the current ancestor assignment -- skip
  // them without re-deriving the same failure again. nullptr (the common
  // case: no recorded nogood even mentions this slot) costs one hash
  // lookup and nothing else. NogoodForbiddenWords returns a pointer into
  // per-length *scratch* state (nogood_forbidden_scratch_by_length_),
  // reused across calls -- safe to read here, but NOT safe to hold onto
  // across the loop below, which recurses back into Backtrack: a
  // descendant call for a different slot of the same length would call
  // NogoodForbiddenWords again and silently overwrite the very buffer this
  // pointer refers to, corrupting an ancestor frame's still-in-progress
  // iteration. So it's merged into a local copy immediately instead of
  // kept as a pointer -- one WordBitset copy+OR, paid only on the (past
  // the first attempt) uncommon path where a nogood actually applies here.
  const WordBitset* nogood_forbidden = NogoodForbiddenWords(slot, domains, assigned);
  WordBitset combined_used_storage;
  const WordBitset* used = &used_by_length[static_cast<size_t>(length)];
  if (nogood_forbidden != nullptr) {
    combined_used_storage = *used;
    combined_used_storage |= *nogood_forbidden;
    used = &combined_used_storage;
  }

  // Try higher-quality words first so a valid fill reads like a real
  // crossword rather than the first alphabetically-consistent candidate.
  // Deliberately not randomized (unlike slot choice above) -- see the
  // class comment in solver.hpp.
  //
  // Shared body for both candidate-iteration paths below: assign, recurse,
  // undo, and report whether this attempt aborted (stop trying siblings)
  // or a solution was found (unwind immediately).
  auto try_candidate = [&](size_t idx) -> std::optional<Solution> {
    stats_.nodes++;
    if (global_node_counter_ != nullptr) {
      global_node_counter_->fetch_add(1, std::memory_order_relaxed);
    }
    size_t domain_mark = trail.domains.size();
    size_t used_mark = trail.used.size();
    if (Assign(slot, idx, domains, used_by_length, assigned, trail,
               crossing_weights)) {
      auto result = Backtrack(domains, used_by_length, assigned, trail,
                               crossing_weights);
      if (result) return result;
    }
    Undo(slot, domains, used_by_length, assigned, trail, domain_mark, used_mark);
    return std::nullopt;
  };

  // dict_.ScoreOrder(length) spans every word of this length, so walking it
  // costs O(NumWordsOfLength) regardless of how narrow the domain actually
  // is -- fine when most words are live candidates, wasteful once deep
  // search has narrowed the domain to a handful out of a dictionary length
  // group that can run into the thousands (short slots especially).
  // `domain_count` -- reused from SelectBranchSlot above, not recomputed
  // here, since a second CountAndNot pass on every single branching node
  // (most of which take the plain scan below anyway) would cost more than
  // this fast path saves -- upper-bounds the true candidate count (it
  // predates nogood_forbidden's own narrowing, if any; a safe direction to
  // be wrong, since AppendSetBits' max_bits below only ever needs to be
  // *at least* the real count). Below the threshold it's cheaper to
  // extract just those candidates and sort that small set by score than to
  // filter the whole dictionary length group. 1000 was picked the same way
  // as Propagate's kDirectLookupThreshold: re-running
  // benchmarks/bench_subset.py (single-threaded, for a noise-free reading
  // -- see --threads there) at 200/1000/4000 and taking the plateau (200:
  // -1.2%, 1000: -3.0%, 4000: -2.9%, all vs. the pre-this-change baseline,
  // averaged over 3 seeds).
  constexpr size_t kCandidateDirectThreshold = 1000;

  if (domain_count <= kCandidateDirectThreshold) {
    // Shared scratch is safe here (unlike the pointer above): this frame
    // only ever reads `candidates` while building `ordered_candidates`,
    // entirely before the loop -- and thus before any recursive call that
    // could reuse this same length's buffer -- below starts.
    WordBitset& candidates =
        candidate_scratch_by_length_[static_cast<size_t>(length)];
    candidates = slot_domain;
    candidates.AndNotAssign(*used);

    // A local vector, not a reused scratch member: this list, unlike
    // `candidates` above, has to survive across the recursive calls in the
    // loop below, so a shared buffer would get clobbered by a same-length
    // descendant the same way the nogood pointer above did.
    std::vector<size_t> ordered_candidates;
    ordered_candidates.reserve(domain_count);
    candidates.AppendSetBits(ordered_candidates, domain_count);
    // Gated on the *same* shuffle_words flag as the large-domain branch
    // below, not unconditional -- an unconditional version of this
    // regressed grid_328.txt from solved in 0.83s to a 20s timeout (see
    // docs/design.md), because that grid never needed more than 3
    // restarts and so never needed diversity here either. But gating this
    // branch on shuffle_words specifically (not skipping it, as an
    // earlier version of this fix did) turned out to matter: direct
    // instrumentation on a real wide-open 7x7 grid (Hawksley's, no black
    // squares) showed its live domains stay at or below
    // kCandidateDirectThreshold for effectively the *entire* search --
    // the large-domain branch below never fires at all -- so scoping word
    // shuffling to only that branch left this grid's actual search path
    // exactly as deterministic and stuck as the unmodified solver's,
    // despite this project believing otherwise for a while. The
    // restart-count/worker-offset gate in shuffle_words already protects
    // grid_328.txt-like grids (few restarts needed, shuffle_words stays
    // false for their whole solve) while still kicking in here for a
    // grid that's demonstrably still stuck after many restarts, wherever
    // its actual domains happen to live.
    if (shuffle_words) {
      std::shuffle(ordered_candidates.begin(), ordered_candidates.end(), rng_);
    } else {
      std::sort(ordered_candidates.begin(), ordered_candidates.end(),
                [this, length](size_t a, size_t b) {
                  return dict_.ScoreRank(length, a) < dict_.ScoreRank(length, b);
                });
    }

    for (size_t idx : ordered_candidates) {
      if (auto result = try_candidate(idx)) return result;
      // A deeper call may have aborted this whole attempt (backtrack
      // budget exceeded) rather than genuinely exhausting its options --
      // stop trying sibling candidates and unwind immediately instead of
      // continuing to search a doomed attempt.
      if (aborted_) return std::nullopt;
    }
  } else if (shuffle_words) {
    // On a restart, when this slot's live domain is too large for the
    // direct-lookup branch above (thousands of candidates -- the regime a
    // wide-open, weakly-constrained grid produces), shuffle instead of
    // taking dict_.ScoreOrder(length)'s strict best-first order. See the
    // class comment's "restart" section: a slot's true solution word isn't
    // necessarily top-scored, and strict best-first order meant *every*
    // restart re-tried the same top-ranked words before ever reaching
    // whatever word the real solution needed -- slot-choice diversity
    // alone couldn't route around that, since word order at any slot was
    // identical across all restarts. See docs/design.md, "Word-choice
    // randomization on restarts", for the full derivation and the
    // rejected cell-level-branching alternative: letter-at-a-time
    // branching (mirroring orca's own architecture) was a genuine, clean
    // win on the general 15x15 benchmark corpus, but consistently
    // underperformed this simpler word-level shuffle on the specific
    // motivating case (a real 7x7 with no black squares) across several
    // independent, controlled comparisons -- reverted in favor of this,
    // since meeting that concrete goal mattered more than the broader
    // (but here counterproductive) architectural change.
    std::vector<size_t> candidates;
    candidates.reserve(domain_count);
    for (size_t idx : dict_.ScoreOrder(length)) {
      if (slot_domain.Test(idx) && !used->Test(idx)) candidates.push_back(idx);
    }
    std::shuffle(candidates.begin(), candidates.end(), rng_);
    for (size_t idx : candidates) {
      if (auto result = try_candidate(idx)) return result;
      if (aborted_) return std::nullopt;
    }
  } else {
    for (size_t idx : dict_.ScoreOrder(length)) {
      if (!slot_domain.Test(idx) || used->Test(idx)) continue;
      if (auto result = try_candidate(idx)) return result;
      if (aborted_) return std::nullopt;
    }
  }

  stats_.backtracks++;
  if (++attempt_backtracks_ >= attempt_backtrack_limit_) {
    aborted_ = true;
    // `slot`'s candidate loop just ran to completion -- every candidate
    // genuinely tried and undone with aborted_ still false at each prior
    // step (the check above would have returned early otherwise) -- so
    // this ancestor assignment really is a proven dead end, not an
    // artifact of the budget cutoff. Recording it here (only on the
    // specific exhaustion that triggers a restart, not on every ordinary
    // backtrack) keeps the nogood count bounded by the restart count,
    // exactly as in Lecoutre et al.'s nogood-recording-from-restarts.
    RecordNogoodFromDeadEnd(domains, assigned);
  }
  return std::nullopt;
}

Solution Solver::ExtractSolution(const std::vector<WordBitset>& domains) const {
  Solution solution;
  for (const Slot& slot : grid_.slots()) {
    // Every slot passed through Assign() by the time the search concludes,
    // so its domain is a true singleton (no need to mask used words here).
    auto bits = domains[static_cast<size_t>(slot.id)].SetBits();
    size_t idx = bits.empty() ? 0 : bits.front();
    solution.assignment[slot.id] = dict_.WordsOfLength(slot.length)[idx];
  }
  return solution;
}

}  // namespace xfill
