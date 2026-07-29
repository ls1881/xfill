#include "xfill/solver.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace xfill {

namespace {
constexpr uint32_t kAllLettersMask = (1u << 26) - 1;
// How much a crossing weight decays toward 1 every time some *other*
// crossing causes a wipeout -- lower prioritizes recent conflicts over
// older ones. Value taken from rf-/ingrid_core's WEIGHT_AGE_FACTOR.
constexpr float kWeightAgeFactor = 0.99f;

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
  for (const Slot& slot : grid_.slots()) {
    slots_by_length_[slot.length].push_back(slot.id);
  }
  const std::vector<Crossing>& crossings = grid_.crossings();
  for (size_t i = 0; i < crossings.size(); ++i) {
    const Crossing& cr = crossings[i];
    int crossing_id = static_cast<int>(i);
    crossings_by_slot_[static_cast<size_t>(cr.slot_a)].push_back(
        {cr.slot_b, cr.offset_a, cr.offset_b, crossing_id});
    crossings_by_slot_[static_cast<size_t>(cr.slot_b)].push_back(
        {cr.slot_a, cr.offset_b, cr.offset_a, crossing_id});
  }
}

std::optional<Solution> Solver::Solve() {
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

  std::vector<float> crossing_weights(grid_.crossings().size(), 1.0f);

  Trail root_trail;
  bool changed = true;
  while (changed) {
    changed = false;
    if (!Propagate(domains, all_slots, root_trail, 0, crossing_weights)) {
      return std::nullopt;
    }
    root_trail.domains.clear();  // one-time pass -- nothing to undo to
    if (!EnforceUniqueWordsOnce(domains, changed)) return std::nullopt;
  }

  int max_length = 0;
  for (const auto& [length, ids] : slots_by_length_) {
    if (length > max_length) max_length = length;
  }
  std::vector<WordBitset> used_by_length(static_cast<size_t>(max_length) + 1);
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
    aborted_ = false;
    attempt_backtracks_ = 0;
    randomize_slot_choice_ = attempt > 0;
    rng_.seed(attempt);

    std::vector<WordBitset> attempt_domains = domains;
    std::vector<WordBitset> attempt_used_by_length = used_by_length;
    std::vector<bool> assigned(grid_.slots().size(), false);
    Trail trail;

    auto result = Backtrack(attempt_domains, attempt_used_by_length, assigned,
                             trail, crossing_weights);
    if (result) return result;
    if (!aborted_) return std::nullopt;

    stats_.restarts++;
    attempt_backtrack_limit_ = std::max<uint64_t>(
        attempt_backtrack_limit_ + 1,
        static_cast<uint64_t>(static_cast<float>(attempt_backtrack_limit_) *
                               kRetryGrowthFactor));
  }
}

void Solver::SaveDomainOnce(int slot, const std::vector<WordBitset>& domains,
                             Trail& trail, size_t level_mark) const {
  for (size_t i = level_mark; i < trail.domains.size(); ++i) {
    if (trail.domains[i].slot == slot) return;
  }
  trail.domains.push_back({slot, domains[static_cast<size_t>(slot)]});
}

void Solver::BumpCrossingWeight(std::vector<float>& crossing_weights,
                                int culprit) const {
  for (size_t i = 0; i < crossing_weights.size(); ++i) {
    float increment = (static_cast<int>(i) == culprit) ? 1.0f : 0.0f;
    crossing_weights[i] =
        1.0f + (crossing_weights[i] - 1.0f) * kWeightAgeFactor + increment;
  }
}

bool Solver::Propagate(std::vector<WordBitset>& domains,
                        const std::vector<int>& seed_slots, Trail& trail,
                        size_t level_mark,
                        std::vector<float>& crossing_weights) const {
  std::vector<bool> in_queue(grid_.slots().size(), false);
  for (int s : seed_slots) in_queue[static_cast<size_t>(s)] = true;

  while (true) {
    // Pop the queued slot with the smallest domain -- checking wipeouts
    // (via SelectBranchSlot/the candidate loop, not here) happens sooner
    // when the most-constrained slots propagate first.
    int slot = -1;
    size_t best_count = 0;
    for (size_t i = 0; i < in_queue.size(); ++i) {
      if (!in_queue[i]) continue;
      size_t count = domains[i].Count();
      if (slot == -1 || count < best_count) {
        slot = static_cast<int>(i);
        best_count = count;
      }
    }
    if (slot == -1) break;
    in_queue[static_cast<size_t>(slot)] = false;

    int length = grid_.SlotById(slot).length;
    const WordBitset& slot_domain = domains[static_cast<size_t>(slot)];

    for (const SlotCrossing& sc : crossings_by_slot_[static_cast<size_t>(slot)]) {
      // Which letters are still viable at this crossing position, given
      // the slot's current domain?
      uint32_t possible = 0;
      for (int c = 0; c < 26; ++c) {
        if (slot_domain.Intersects(dict_.LetterMask(length, sc.my_offset,
                                                      static_cast<char>('A' + c)))) {
          possible |= (1u << c);
        }
      }
      if (possible == kAllLettersMask) continue;  // no constraint to apply

      int neighbor_length = grid_.SlotById(sc.neighbor).length;
      WordBitset& neighbor_domain = domains[static_cast<size_t>(sc.neighbor)];

      WordBitset filter(neighbor_domain.size(), false);
      for (int c = 0; c < 26; ++c) {
        if (possible & (1u << c)) {
          filter |= dict_.LetterMask(neighbor_length, sc.neighbor_offset,
                                      static_cast<char>('A' + c));
        }
      }

      // The neighbor's domain only shrinks over the life of the search, so
      // if it's already a subset of the filter, intersecting would be a
      // no-op -- skip the snapshot and the write.
      if (neighbor_domain.IsSubsetOf(filter)) continue;

      SaveDomainOnce(sc.neighbor, domains, trail, level_mark);
      neighbor_domain &= filter;
      if (!neighbor_domain.Any()) {
        BumpCrossingWeight(crossing_weights, sc.crossing_id);
        return false;
      }

      in_queue[static_cast<size_t>(sc.neighbor)] = true;
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

float Solver::SlotWeight(int slot, const std::vector<float>& crossing_weights,
                          const std::vector<bool>& assigned) const {
  float total = 0.0f;
  for (const SlotCrossing& sc : crossings_by_slot_[static_cast<size_t>(slot)]) {
    if (!assigned[static_cast<size_t>(sc.neighbor)]) {
      total += crossing_weights[static_cast<size_t>(sc.crossing_id)];
    }
  }
  return total > 0.0f ? total : 1.0f;
}

int Solver::SelectBranchSlot(const std::vector<WordBitset>& domains,
                              const std::vector<WordBitset>& used_by_length,
                              const std::vector<bool>& assigned,
                              const std::vector<float>& crossing_weights) const {
  std::vector<std::pair<float, int>> candidates;  // (priority, slot id)

  for (const Slot& slot : grid_.slots()) {
    if (assigned[static_cast<size_t>(slot.id)]) continue;

    WordBitset effective = domains[static_cast<size_t>(slot.id)];
    effective.AndNot(used_by_length[static_cast<size_t>(slot.length)]);
    size_t count = effective.Count();

    float weight = SlotWeight(slot.id, crossing_weights, assigned);
    candidates.push_back({static_cast<float>(count) / weight, slot.id});
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
                     size_t level_mark,
                     std::vector<float>& crossing_weights) const {
  int length = grid_.SlotById(slot).length;

  assigned[static_cast<size_t>(slot)] = true;

  SaveDomainOnce(slot, domains, trail, level_mark);
  WordBitset chosen(domains[static_cast<size_t>(slot)].size(), false);
  chosen.Set(word_index);
  domains[static_cast<size_t>(slot)] = chosen;

  trail.used.push_back({length, word_index});
  used_by_length[static_cast<size_t>(length)].Set(word_index);

  return Propagate(domains, {slot}, trail, level_mark, crossing_weights);
}

void Solver::Undo(int slot, std::vector<WordBitset>& domains,
                   std::vector<WordBitset>& used_by_length,
                   std::vector<bool>& assigned, Trail& trail,
                   size_t domain_mark, size_t used_mark) const {
  assigned[static_cast<size_t>(slot)] = false;

  while (trail.used.size() > used_mark) {
    const UsedSnapshot& u = trail.used.back();
    used_by_length[static_cast<size_t>(u.length)].Clear(u.word_index);
    trail.used.pop_back();
  }
  while (trail.domains.size() > domain_mark) {
    DomainSnapshot& d = trail.domains.back();
    domains[static_cast<size_t>(d.slot)] = std::move(d.domain);
    trail.domains.pop_back();
  }
}

std::optional<Solution> Solver::Backtrack(std::vector<WordBitset>& domains,
                                           std::vector<WordBitset>& used_by_length,
                                           std::vector<bool>& assigned,
                                           Trail& trail,
                                           std::vector<float>& crossing_weights) {
  if (aborted_) return std::nullopt;

  int slot = SelectBranchSlot(domains, used_by_length, assigned, crossing_weights);
  if (slot == -1) {
    return ExtractSolution(domains);
  }

  int length = grid_.SlotById(slot).length;
  WordBitset effective = domains[static_cast<size_t>(slot)];
  effective.AndNot(used_by_length[static_cast<size_t>(length)]);

  // Try higher-quality words first so a valid fill reads like a real
  // crossword rather than the first alphabetically-consistent candidate.
  // Deliberately not randomized (unlike slot choice above) -- see the
  // class comment in solver.hpp.
  for (size_t idx : dict_.ScoreOrder(length)) {
    if (!effective.Test(idx)) continue;
    stats_.nodes++;

    size_t domain_mark = trail.domains.size();
    size_t used_mark = trail.used.size();

    if (Assign(slot, idx, domains, used_by_length, assigned, trail, domain_mark,
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
