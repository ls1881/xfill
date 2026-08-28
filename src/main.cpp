#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xfill/dictionary.hpp"
#include "xfill/grid.hpp"
#include "xfill/solver.hpp"

namespace {

// One string per row, '#' for a block, the filled letter (or '.' if this
// slot's word assignment somehow left it empty -- shouldn't happen for a
// genuine solution) otherwise. Shared by the plain-text and --json output
// paths so a caller consuming --json (e.g. the GUI backend) gets the
// solved grid directly, without needing to independently replicate this
// project's internal slot-id numbering scheme just to place each word's
// letters back onto the grid.
std::vector<std::string> FilledGridRows(const xfill::Grid& grid, const xfill::Solution& solution) {
  int width = grid.width();
  int height = grid.height();
  std::vector<char> chars(static_cast<size_t>(width) * static_cast<size_t>(height), '#');

  for (int r = 0; r < height; ++r) {
    for (int c = 0; c < width; ++c) {
      if (!grid.IsBlocked(r, c)) {
        // An isolated open cell -- part of no across/down run of length
        // >= 2, so ComputeSlots never creates a slot covering it -- is
        // never touched by the per-slot loop below. Falling back to
        // whatever was seeded there directly (the same PrefilledLetter
        // BuildInitialDomains/to_grid_spec already consult) means a
        // pre-typed letter in such a cell survives into the output,
        // instead of silently reverting to '.' even on a genuine,
        // complete solution.
        char prefilled = grid.PrefilledLetter(r, c);
        chars[static_cast<size_t>(r) * static_cast<size_t>(width) + static_cast<size_t>(c)] =
            prefilled != '\0' ? prefilled : '.';
      }
    }
  }

  for (const auto& slot : grid.slots()) {
    auto it = solution.assignment.find(slot.id);
    if (it == solution.assignment.end()) continue;
    const std::string& word = it->second;
    for (size_t k = 0; k < slot.cells.size(); ++k) {
      chars[static_cast<size_t>(slot.cells[k])] = word[k];
    }
  }

  std::vector<std::string> rows;
  rows.reserve(static_cast<size_t>(height));
  for (int r = 0; r < height; ++r) {
    rows.emplace_back(chars.begin() + r * width, chars.begin() + (r + 1) * width);
  }
  return rows;
}

void WriteFilledGrid(std::ostream& out, const xfill::Grid& grid,
                      const xfill::Solution& solution) {
  for (const std::string& row : FilledGridRows(grid, solution)) out << row << '\n';
}

// Minimal JSON string escaping -- solution words and grid chars are always
// A-Z, but this is also used nowhere near untrusted input, so this only
// needs to handle the characters that can actually appear.
std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

void WriteJsonGridArray(std::ostream& out, const xfill::Grid& grid, const xfill::Solution& solution) {
  out << "[";
  std::vector<std::string> rows = FilledGridRows(grid, solution);
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i != 0) out << ",";
    out << "\"" << JsonEscape(rows[i]) << "\"";
  }
  out << "]";
}

// [row, col] pairs for every cell belonging to a forced slot (see
// Solution::forced_slot_ids) -- a slot whose word had no real
// alternative when it was assigned. A cell shared by a forced across
// slot and a non-forced down slot (or vice versa) still counts as
// forced: at least one direction had no choice there, so the letter had
// to be what it is regardless of the other direction's freedom.
// Deduplicated (a crossing cell belongs to two slots, either of which
// could independently mark it) and left in whatever order forced_slot_ids
// iterates in -- the caller (WriteJsonForcedCells) doesn't need any
// particular order, just the full set.
std::vector<std::pair<int, int>> ForcedCells(const xfill::Grid& grid,
                                              const xfill::Solution& solution) {
  int width = grid.width();
  std::vector<std::pair<int, int>> cells;
  std::vector<bool> seen(static_cast<size_t>(width) * static_cast<size_t>(grid.height()), false);
  for (const auto& slot : grid.slots()) {
    if (solution.forced_slot_ids.find(slot.id) == solution.forced_slot_ids.end()) continue;
    for (int cell : slot.cells) {
      if (seen[static_cast<size_t>(cell)]) continue;
      seen[static_cast<size_t>(cell)] = true;
      cells.emplace_back(cell / width, cell % width);
    }
  }
  return cells;
}

void WriteJsonForcedCells(std::ostream& out, const xfill::Grid& grid,
                           const xfill::Solution& solution) {
  out << "[";
  std::vector<std::pair<int, int>> cells = ForcedCells(grid, solution);
  for (size_t i = 0; i < cells.size(); ++i) {
    if (i != 0) out << ",";
    out << "[" << cells[i].first << "," << cells[i].second << "]";
  }
  out << "]";
}

void WriteJsonResult(std::ostream& out, const xfill::Grid& grid,
                      const xfill::ParallelSolveResult& result, double seconds) {
  out << "{";
  out << "\"solved\":" << (result.solution ? "true" : "false") << ",";
  out << "\"nodes\":" << result.stats.nodes << ",";
  out << "\"backtracks\":" << result.stats.backtracks << ",";
  out << "\"restarts\":" << result.stats.restarts << ",";
  out << "\"time_seconds\":" << seconds << ",";
  out << "\"threads\":" << result.num_threads << ",";
  out << "\"grid\":";
  if (result.solution) {
    WriteJsonGridArray(out, grid, *result.solution);
  } else {
    out << "null";
  }
  out << ",\"forced_cells\":";
  if (result.solution) {
    WriteJsonForcedCells(out, grid, *result.solution);
  } else {
    out << "[]";
  }
  out << "}";
}

// One line per improvement found by --maximize, streamed to stdout as soon
// as it's found (this is an anytime search -- see Solver::MaximizeScoreParallel's
// doc comment). The "type" key distinguishes this from both the "progress"
// lines (which have a "progress" key instead) and the final "done" line
// below, so a line-by-line NDJSON reader (the GUI backend) can tell all
// three apart without ambiguity.
void WriteJsonImprovement(std::ostream& out, const xfill::Grid& grid, const xfill::Solution& solution,
                           int64_t score) {
  out << "{\"type\":\"improved\",\"score\":" << score << ",\"grid\":";
  WriteJsonGridArray(out, grid, solution);
  out << "}";
}

void WriteJsonMaximizeResult(std::ostream& out, const xfill::Grid& grid,
                              const std::optional<xfill::Solution>& best_solution, int64_t best_score,
                              uint64_t nodes, double seconds, unsigned num_threads) {
  out << "{";
  out << "\"type\":\"done\",";
  out << "\"solved\":" << (best_solution ? "true" : "false") << ",";
  out << "\"score\":" << (best_solution ? std::to_string(best_score) : "null") << ",";
  out << "\"nodes\":" << nodes << ",";
  out << "\"time_seconds\":" << seconds << ",";
  out << "\"threads\":" << num_threads << ",";
  out << "\"grid\":";
  if (best_solution) {
    WriteJsonGridArray(out, grid, *best_solution);
  } else {
    out << "null";
  }
  out << "}";
}

struct Args {
  std::string grid_path;
  std::string across_dict_path;
  std::string down_dict_path;
  int across_min_score = 0;
  int down_min_score = 0;
  // length -> min score, for lengths that need a different threshold than
  // the direction's across_min_score/down_min_score default. See
  // ParseLengthScoreMap for the "3:25,5:60" wire format these come from.
  std::unordered_map<int, int> across_min_overrides;
  std::unordered_map<int, int> down_min_overrides;
  unsigned num_threads = 0;
  bool json = false;
  bool progress = false;
  bool maximize = false;
};

// Parses "<length>:<score>,<length>:<score>,..." (e.g. "3:25,5:60") into a
// length->score map. Empty string yields an empty map. Used for
// --across-min-overrides/--down-min-overrides, below.
std::unordered_map<int, int> ParseLengthScoreMap(const std::string& s) {
  std::unordered_map<int, int> out;
  std::stringstream ss(s);
  std::string pair;
  while (std::getline(ss, pair, ',')) {
    if (pair.empty()) continue;
    size_t colon = pair.find(':');
    if (colon == std::string::npos) {
      throw std::runtime_error("malformed length:score pair: " + pair);
    }
    int length = std::stoi(pair.substr(0, colon));
    int score = std::stoi(pair.substr(colon + 1));
    out[length] = score;
  }
  return out;
}

// std::stoul silently accepts a leading '-' and wraps a negative value into
// a huge unsigned one instead of throwing (it parses the digits after the
// sign, then two's-complement-wraps the negation) -- "--threads -1" would
// otherwise become num_threads=4294967295 on a typical build, which
// SolveParallel/MaximizeScoreParallel then try to actually size a thread
// pool and spawn loop from, rather than a clean "invalid --threads" error.
unsigned ParseThreadCount(const std::string& s) {
  if (s.empty() || s.find('-') != std::string::npos) {
    throw std::runtime_error("invalid thread count: " + s + " (must be a non-negative integer)");
  }
  return static_cast<unsigned>(std::stoul(s));
}

// New flag-based invocation, used by the GUI backend so across/down can
// have independent dictionaries and min scores:
//   xfill_cli <grid_file> --dict <path> [--min-score <n>]
//   xfill_cli <grid_file> --across-dict <path> --across-min <n>
//                         --down-dict <path> --down-min <n>
//             [--threads <n>] [--json]
// --dict/--min-score set both directions at once; --across-*/--down-*
// override per direction. At least one of --dict or both --across-dict and
// --down-dict must be given.
Args ParseFlagArgs(int argc, char** argv) {
  Args args;
  args.grid_path = argv[1];
  std::optional<std::string> shared_dict;
  int shared_min_score = 0;

  for (int i = 2; i < argc; ++i) {
    std::string flag = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + flag);
      return argv[++i];
    };
    if (flag == "--dict") {
      shared_dict = next();
    } else if (flag == "--min-score") {
      shared_min_score = std::stoi(next());
    } else if (flag == "--across-dict") {
      args.across_dict_path = next();
    } else if (flag == "--across-min") {
      args.across_min_score = std::stoi(next());
    } else if (flag == "--down-dict") {
      args.down_dict_path = next();
    } else if (flag == "--down-min") {
      args.down_min_score = std::stoi(next());
    } else if (flag == "--across-min-overrides") {
      args.across_min_overrides = ParseLengthScoreMap(next());
    } else if (flag == "--down-min-overrides") {
      args.down_min_overrides = ParseLengthScoreMap(next());
    } else if (flag == "--threads") {
      args.num_threads = ParseThreadCount(next());
    } else if (flag == "--json") {
      args.json = true;
    } else if (flag == "--progress") {
      args.progress = true;
    } else if (flag == "--maximize") {
      args.maximize = true;
    } else {
      throw std::runtime_error("unknown flag: " + flag);
    }
  }

  if (shared_dict) {
    if (args.across_dict_path.empty()) {
      args.across_dict_path = *shared_dict;
      args.across_min_score = shared_min_score;
    }
    if (args.down_dict_path.empty()) {
      args.down_dict_path = *shared_dict;
      args.down_min_score = shared_min_score;
    }
  }
  if (args.across_dict_path.empty() || args.down_dict_path.empty()) {
    throw std::runtime_error(
        "need --dict, or both --across-dict and --down-dict");
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr
        << "usage: xfill_cli <grid_spec_file> <dictionary_file> "
           "[min_score] [num_threads]\n"
           "   or: xfill_cli <grid_spec_file> --dict <path> "
           "[--min-score <n>] [--threads <n>] [--json]\n"
           "   or: xfill_cli <grid_spec_file> --across-dict <path> "
           "--across-min <n> --down-dict <path> --down-min <n> "
           "[--across-min-overrides <spec>] [--down-min-overrides <spec>] "
           "[--threads <n>] [--json] [--progress] [--maximize]\n"
           "  --across-min-overrides/--down-min-overrides <spec>: per-"
           "word-length min score thresholds, overriding --across-min/"
           "--down-min for just those lengths.\n"
           "  <spec> is \"<length>:<score>,<length>:<score>,...\", e.g. "
           "\"3:25,5:60\" -- 3-letter words there need only 25, 5-letter "
           "need 60,\n"
           "  every other length still uses --across-min/--down-min. "
           "Flag-mode only.\n"
           "  --progress: while solving, write a "
           "{\"progress\":true,\"nodes\":N} line to stdout roughly every "
           "150ms\n"
           "  (N = total nodes visited across every worker so far), "
           "ahead of the final result line. Implies --json (so the whole "
           "stream stays line-delimited JSON). Flag-mode only.\n"
           "  --maximize: run the separate branch-and-bound score-"
           "maximizing search instead of the default first-solution "
           "search.\n"
           "  This is an anytime algorithm: every time it finds a "
           "complete fill with a higher total score than any found so "
           "far,\n"
           "  it streams a {\"type\":\"improved\",\"score\":N,\"grid\":"
           "[...]} line immediately, then keeps searching for something "
           "better\n"
           "  until it either proves optimality or is killed by the "
           "caller. A final {\"type\":\"done\",...} line reports the "
           "best fill found.\n"
           "  Always streams NDJSON to stdout regardless of --json. "
           "Flag-mode only.\n"
           "  num_threads: 0 (default) = "
           "std::thread::hardware_concurrency(); 1 = single-threaded,\n"
           "  for reproducible timing or comparing against a build "
           "predating SolveParallel.\n";
    return 1;
  }

  try {
    // argv[2] starting with "--" selects the new flag-based form (needed
    // for independent across/down dictionaries); otherwise this is the
    // original positional form, kept byte-for-byte compatible since
    // benchmarks/bench_subset.py's STATS_RE regex and this project's other
    // tooling depend on its exact stdout format.
    bool flag_mode = std::string(argv[2]).rfind("--", 0) == 0;

    Args args;
    if (flag_mode) {
      args = ParseFlagArgs(argc, argv);
    } else {
      args.grid_path = argv[1];
      args.across_dict_path = args.down_dict_path = argv[2];
      args.across_min_score = args.down_min_score = argc >= 4 ? std::stoi(argv[3]) : 0;
      args.num_threads = argc >= 5 ? ParseThreadCount(argv[4]) : 0;
    }
    // --progress's own output is always a JSON line ({"progress":true,...})
    // -- without --json, the plain-text final-result path below would still
    // follow it with non-JSON lines (a WriteFilledGrid grid dump, then a
    // "nodes=... time=..." summary), leaving a line-by-line JSON reader
    // unable to parse the stream past the progress lines. --maximize
    // already has this same "always JSON regardless of the flag" rule (see
    // its own help text above); --progress gets it for the same reason.
    if (args.progress) args.json = true;

    xfill::Grid grid = xfill::Grid::FromFile(args.grid_path);
    xfill::MinScoreByLength across_min(args.across_min_score);
    across_min.overrides = args.across_min_overrides;
    xfill::MinScoreByLength down_min(args.down_min_score);
    down_min.overrides = args.down_min_overrides;
    bool same_across_down = args.across_dict_path == args.down_dict_path &&
                             args.across_min_score == args.down_min_score &&
                             args.across_min_overrides == args.down_min_overrides;
    xfill::Dictionary dict =
        same_across_down
            ? xfill::Dictionary::LoadFromFile(args.across_dict_path, across_min)
            : xfill::Dictionary::LoadDual(args.across_dict_path, across_min, args.down_dict_path,
                                           down_min);

    if (args.maximize) {
      // Entirely separate call path from Solve()/SolveParallel() below --
      // Solver::MaximizeScoreParallel is its own branch-and-bound search
      // (see its doc comment in solver.hpp), so choosing --maximize here
      // costs the default (untoggled) search path nothing.
      std::atomic<uint64_t> last_nodes{0};
      std::function<void(uint64_t)> on_progress = [&](uint64_t nodes) {
        last_nodes.store(nodes, std::memory_order_relaxed);
        if (args.progress) {
          std::cout << "{\"progress\":true,\"nodes\":" << nodes << "}\n";
          std::cout.flush();
        }
      };

      int64_t best_score = 0;
      auto on_improved = [&](const xfill::Solution& sol, int64_t score) {
        best_score = score;
        WriteJsonImprovement(std::cout, grid, sol, score);
        std::cout << "\n";
        std::cout.flush();
      };

      auto start = std::chrono::steady_clock::now();
      std::optional<xfill::Solution> best = xfill::Solver::MaximizeScoreParallel(
          grid, dict, args.num_threads, on_improved, /*cancel=*/nullptr, on_progress);
      auto end = std::chrono::steady_clock::now();
      double seconds = std::chrono::duration<double>(end - start).count();

      unsigned reported_threads = args.num_threads;
      if (reported_threads == 0) {
        reported_threads = std::thread::hardware_concurrency();
        if (reported_threads == 0) reported_threads = 1;
      }

      WriteJsonMaximizeResult(std::cout, grid, best, best_score, last_nodes.load(), seconds,
                               reported_threads);
      std::cout << "\n";
      return best ? 0 : 1;
    }

    std::function<void(uint64_t)> on_progress;
    if (args.progress) {
      // A distinct, unambiguous shape (the "progress" key) from the
      // final result line below, which never has one -- lets a line-by-
      // line reader (the GUI backend) tell interim updates from the
      // terminal one without needing a second stream, which would risk
      // the classic two-pipe subprocess deadlock (child blocks writing
      // to a full pipe the parent isn't currently reading, while the
      // parent blocks reading the *other* pipe to EOF). Flushed
      // immediately: stdout is otherwise fully buffered when it isn't
      // attached to a terminal, e.g. when a parent process reads it
      // through a pipe, and an update the caller can't see until some
      // later flush defeats the entire point of live progress.
      on_progress = [](uint64_t nodes) {
        std::cout << "{\"progress\":true,\"nodes\":" << nodes << "}\n";
        std::cout.flush();
      };
    }

    auto start = std::chrono::steady_clock::now();
    xfill::ParallelSolveResult result =
        xfill::Solver::SolveParallel(grid, dict, args.num_threads, on_progress);
    auto end = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();

    if (args.json) {
      WriteJsonResult(std::cout, grid, result, seconds);
      std::cout << "\n";
      return result.solution ? 0 : 1;
    }

    std::filesystem::path output_dir = "output";
    std::filesystem::create_directories(output_dir);
    std::filesystem::path output_path =
        output_dir / (std::filesystem::path(args.grid_path).stem().string() + "_output.txt");
    std::ofstream out(output_path, std::ios::trunc);

    for (std::ostream* stream : {static_cast<std::ostream*>(&std::cout),
                                  static_cast<std::ostream*>(&out)}) {
      if (!result.solution) {
        *stream << "No solution found.\n";
      } else {
        WriteFilledGrid(*stream, grid, *result.solution);
      }
      *stream << "\nnodes=" << result.stats.nodes
              << " backtracks=" << result.stats.backtracks
              << " restarts=" << result.stats.restarts
              << " time=" << seconds << "s"
              << " threads=" << result.num_threads << "\n";
    }

    std::cerr << "wrote " << output_path.string() << "\n";
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
