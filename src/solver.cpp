#include "xfill/solver.hpp"

#include <utility>

namespace xfill {

Solver::Solver(const Grid& grid, const Dictionary& dict)
    : grid_(grid), dict_(dict) {
  for (const Slot& slot : grid_.slots()) {
    slots_by_length_[slot.length].push_back(slot.id);
  }
}

std::optional<Solution> Solver::Solve() {
  std::vector<WordBitset> domains(grid_.slots().size());
  for (const Slot& slot : grid_.slots()) {
    domains[static_cast<size_t>(slot.id)] = dict_.FullDomain(slot.length);
  }
  return Backtrack(std::move(domains));
}

bool Solver::Propagate(std::vector<WordBitset>& domains) const {
  // Catch domains that start empty (e.g. no dictionary word of that
  // length) even when the owning slot has no crossings to narrow it --
  // the loop below only ever inspects crossings, so an isolated slot's
  // domain would otherwise go unchecked.
  for (const WordBitset& domain : domains) {
    if (!domain.Any()) return false;
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (const Crossing& cr : grid_.crossings()) {
      int len_a = grid_.SlotById(cr.slot_a).length;
      int len_b = grid_.SlotById(cr.slot_b).length;

      // Narrow slot_b to words whose letter at offset_b is consistent
      // with at least one remaining candidate in slot_a, by unioning the
      // per-letter masks for every letter still viable in slot_a.
      WordBitset allowed_b(domains[static_cast<size_t>(cr.slot_b)].size(),
                            false);
      for (int c = 0; c < 26; ++c) {
        char letter = static_cast<char>('A' + c);
        WordBitset test = domains[static_cast<size_t>(cr.slot_a)];
        test &= dict_.LetterMask(len_a, cr.offset_a, letter);
        if (test.Any()) {
          allowed_b |= dict_.LetterMask(len_b, cr.offset_b, letter);
        }
      }
      size_t before = domains[static_cast<size_t>(cr.slot_b)].Count();
      domains[static_cast<size_t>(cr.slot_b)] &= allowed_b;
      size_t after = domains[static_cast<size_t>(cr.slot_b)].Count();
      if (after == 0) return false;
      if (after != before) changed = true;

      // Same narrowing in the other direction.
      WordBitset allowed_a(domains[static_cast<size_t>(cr.slot_a)].size(),
                            false);
      for (int c = 0; c < 26; ++c) {
        char letter = static_cast<char>('A' + c);
        WordBitset test = domains[static_cast<size_t>(cr.slot_b)];
        test &= dict_.LetterMask(len_b, cr.offset_b, letter);
        if (test.Any()) {
          allowed_a |= dict_.LetterMask(len_a, cr.offset_a, letter);
        }
      }
      before = domains[static_cast<size_t>(cr.slot_a)].Count();
      domains[static_cast<size_t>(cr.slot_a)] &= allowed_a;
      after = domains[static_cast<size_t>(cr.slot_a)].Count();
      if (after == 0) return false;
      if (after != before) changed = true;
    }

    if (!EnforceUniqueWords(domains, changed)) return false;
  }
  return true;
}

bool Solver::EnforceUniqueWords(std::vector<WordBitset>& domains,
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

int Solver::SelectBranchSlot(const std::vector<WordBitset>& domains) const {
  int best = -1;
  size_t best_count = 0;
  for (size_t i = 0; i < domains.size(); ++i) {
    size_t count = domains[i].Count();
    if (count <= 1) continue;
    if (best == -1 || count < best_count) {
      best = static_cast<int>(i);
      best_count = count;
    }
  }
  return best;
}

std::optional<Solution> Solver::Backtrack(std::vector<WordBitset> domains) {
  if (!Propagate(domains)) {
    stats_.backtracks++;
    return std::nullopt;
  }

  int slot = SelectBranchSlot(domains);
  if (slot == -1) {
    return ExtractSolution(domains);
  }

  int length = grid_.SlotById(slot).length;
  const WordBitset& domain = domains[static_cast<size_t>(slot)];
  // Try higher-quality words first so a valid fill reads like a real
  // crossword rather than the first alphabetically-consistent candidate.
  for (size_t idx : dict_.ScoreOrder(length)) {
    if (!domain.Test(idx)) continue;
    stats_.nodes++;
    std::vector<WordBitset> trial = domains;
    WordBitset chosen(domain.size(), false);
    chosen.Set(idx);
    trial[static_cast<size_t>(slot)] = chosen;

    auto result = Backtrack(std::move(trial));
    if (result) return result;
  }

  stats_.backtracks++;
  return std::nullopt;
}

Solution Solver::ExtractSolution(const std::vector<WordBitset>& domains) const {
  Solution solution;
  for (const Slot& slot : grid_.slots()) {
    auto bits = domains[static_cast<size_t>(slot.id)].SetBits();
    // Propagate() + MRV guarantee exactly one set bit here by construction.
    size_t idx = bits.empty() ? 0 : bits.front();
    solution.assignment[slot.id] = dict_.WordsOfLength(slot.length)[idx];
  }
  return solution;
}

}  // namespace xfill
