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
        chars[static_cast<size_t>(r) * static_cast<size_t>(width) + static_cast<size_t>(c)] = '.';
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
  unsigned num_threads = 0;
  bool json = false;
  bool progress = false;
  bool maximize = false;
};

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
    } else if (flag == "--threads") {
      args.num_threads = static_cast<unsigned>(std::stoul(next()));
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
           "[--threads <n>] [--json] [--progress] [--maximize]\n"
           "  --progress: while solving, write a "
           "{\"progress\":true,\"nodes\":N} line to stdout roughly every "
           "150ms\n"
           "  (N = total nodes visited across every worker so far), "
           "ahead of the final result line. Flag-mode only.\n"
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
      args.num_threads = argc >= 5 ? static_cast<unsigned>(std::stoul(argv[4])) : 0;
    }

    xfill::Grid grid = xfill::Grid::FromFile(args.grid_path);
    xfill::Dictionary dict =
        args.across_dict_path == args.down_dict_path && args.across_min_score == args.down_min_score
            ? xfill::Dictionary::LoadFromFile(args.across_dict_path, args.across_min_score)
            : xfill::Dictionary::LoadDual(args.across_dict_path, args.across_min_score,
                                           args.down_dict_path, args.down_min_score);

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
