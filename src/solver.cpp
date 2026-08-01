#include "xfill/solver.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
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
  for (const auto& [length, ids] : slots_by_length_) {
    filter_scratch_by_length_[static_cast<size_t>(length)] =
        WordBitset(dict_.NumWordsOfLength(length), false);
    nogood_forbidden_scratch_by_length_[static_cast<size_t>(length)] =
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

std::optional<Solution> Solver::Solve(uint64_t attempt_offset,
                                       const std::atomic<bool>* cancel) {
  cancel_ = cancel;
  std::vector<WordBitset> domains(grid_.slots().size());
  for (const Slot& slot : grid_.slots()) {
    domains[static_cast<size_t>(slot.id)] = dict_.FullDomain(slot.length);
  }

  // Catch domains that start empty (e.g. no dictionary word of that
  // length) even when the owning slot has no crossings to narrow it --
  // Propagate only ever visits slots reachable from a seed via crossings,
  // so an isolated slot's domain would otherwise go unchecked.
  for (const WordBitset& domain : domains) {
    if (!domain.Any()) return std::nullopt;
  }

  std::vector<int> all_slots;
  all_slots.reserve(grid_.slots().size());
  for (const Slot& slot : grid_.slots()) all_slots.push_back(slot.id);

  CrossingWeights crossing_weights(grid_.crossings().size());

  Trail root_trail;
  bool changed = true;
  while (changed) {
    changed = false;
    if (!Propagate(domains, all_slots, root_trail, next_save_epoch_++, crossing_weights)) {
      return std::nullopt;
    }
    root_trail.domains.clear();  // one-time pass -- nothing to undo to
    if (!EnforceUniqueWordsOnce(domains, changed)) return std::nullopt;
  }

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
  attempt_backtrack_limit_ = kInitialBacktrackLimit;
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
    attempt_backtrack_limit_ = std::max<uint64_t>(
        attempt_backtrack_limit_ + 1,
        static_cast<uint64_t>(static_cast<float>(attempt_backtrack_limit_) *
                               kRetryGrowthFactor));
  }
}

ParallelSolveResult Solver::SolveParallel(const Grid& grid, const Dictionary& dict,
                                           unsigned num_threads) {
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
    threads.emplace_back([&, i]() {
      auto solution = solvers[i]->Solve(static_cast<uint64_t>(i) * kAttemptStride, &cancel);
      if (!solution) return;
      // First solution to arrive here wins; cancel tells every other
      // worker to unwind (checked once per node in Backtrack). A loser
      // of this race still has a genuine solution, just not the one
      // reported -- any solution is as good as any other, so it's
      // simply discarded rather than compared.
      bool expected = false;
      if (cancel.compare_exchange_strong(expected, true)) {
        std::lock_guard<std::mutex> lock(result_mutex);
        winning_solution = std::move(solution);
        winning_stats = solvers[i]->stats();
      }
    });
  }
  for (std::thread& t : threads) t.join();

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
      total += crossing_weights.Get(sc.crossing_id);
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
                              const CrossingWeights& crossing_weights) const {
  int active = ActiveComponent();
  if (active == -1) return -1;

  std::vector<std::pair<float, int>>& candidates = branch_candidates_scratch_;
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
    candidates.push_back({static_cast<float>(count) / weight, sid});
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
  return candidates[dist(rng_)].second;
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

  int slot = SelectBranchSlot(domains, used_by_length, assigned, crossing_weights);
  if (slot == -1) {
    return ExtractSolution(domains);
  }

  int length = slot_length_[static_cast<size_t>(slot)];
  // Test membership against domains[slot] and used_by_length separately
  // instead of copying the domain to AndNot() it first: ScoreOrder(length)
  // spans the whole dictionary at that length, and this loop runs once
  // per node, so the avoided allocation (and the O(chunks) AndNot pass it
  // replaces) adds up -- most candidates fail the first (domain) test
  // immediately, so the second test is rarely even reached.
  const WordBitset& slot_domain = domains[static_cast<size_t>(slot)];
  const WordBitset& used = used_by_length[static_cast<size_t>(length)];
  // Words already proven, by an earlier restart's fully-exhausted search,
  // to be a dead end given exactly the current ancestor assignment -- skip
  // them without re-deriving the same failure again. nullptr (the common
  // case: no recorded nogood even mentions this slot) costs one hash
  // lookup and nothing else.
  const WordBitset* nogood_forbidden = NogoodForbiddenWords(slot, domains, assigned);

  // Try higher-quality words first so a valid fill reads like a real
  // crossword rather than the first alphabetically-consistent candidate.
  // Deliberately not randomized (unlike slot choice above) -- see the
  // class comment in solver.hpp.
  for (size_t idx : dict_.ScoreOrder(length)) {
    if (!slot_domain.Test(idx) || used.Test(idx)) continue;
    if (nogood_forbidden != nullptr && nogood_forbidden->Test(idx)) continue;
    stats_.nodes++;

    size_t domain_mark = trail.domains.size();
    size_t used_mark = trail.used.size();

    if (Assign(slot, idx, domains, used_by_length, assigned, trail,
               crossing_weights)) {
      auto result = Backtrack(domains, used_by_length, assigned, trail,
                               crossing_weights);
      if (result) return result;
    }
    Undo(slot, domains, used_by_length, assigned, trail, domain_mark, used_mark);

    // A deeper call may have aborted this whole attempt (backtrack budget
    // exceeded) rather than genuinely exhausting its options -- stop
    // trying sibling candidates and unwind immediately instead of
    // continuing to search a doomed attempt.
    if (aborted_) return std::nullopt;
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
