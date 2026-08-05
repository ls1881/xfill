// Headless CLI wrapper for the crossword-composer library, added purely
// for benchmarking (upstream only ships a hardcoded demo `main.rs` and a
// browser UI, no file-driven CLI). Reads a grid in xfill's plain-text
// format ('.' open, '#' block) and a WORD;SCORE dictionary file (score
// column ignored -- this crate has no notion of word scoring), and times
// a single solve() call.
use std::env;
use std::fs;
use std::time::Instant;

// Pull the solver modules in directly as local sources rather than going
// through lib.rs, which only exposes them via a wasm-bindgen JS binding
// (private `mod`, not `pub mod`) and pulls in wasm-only dependencies this
// native binary doesn't need.
#[path = "../dictionary.rs"]
mod dictionary;
#[path = "../grid.rs"]
mod grid;
#[path = "../index.rs"]
mod index;
#[path = "../solver.rs"]
mod solver;

use dictionary::Dictionary;
use grid::Grid;
use solver::solve;

fn read_grid_rows(path: &str) -> Vec<Vec<char>> {
    fs::read_to_string(path)
        .expect("failed to read grid file")
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| l.chars().collect())
        .collect()
}

// Builds the same slot structure xfill/orca use: maximal open runs of
// length >= 2, scanned row-then-column for across, column-then-row for
// down. Each word is expressed as the ordered list of flat cell indices
// (row * width + col) it covers, matching Grid::new's expected input.
fn compute_words(rows: &[Vec<char>]) -> Vec<Vec<usize>> {
    let height = rows.len();
    let width = rows.iter().map(|r| r.len()).max().unwrap_or(0);
    let is_open = |r: usize, c: usize| -> bool {
        r < rows.len() && c < rows[r].len() && rows[r][c] != '#'
    };
    let mut words = Vec::new();

    // Across
    for r in 0..height {
        let mut c = 0;
        while c < width {
            if !is_open(r, c) {
                c += 1;
                continue;
            }
            let start = c;
            while c < width && is_open(r, c) {
                c += 1;
            }
            if c - start >= 2 {
                words.push((start..c).map(|cc| r * width + cc).collect());
            }
        }
    }
    // Down
    for c in 0..width {
        let mut r = 0;
        while r < height {
            if !is_open(r, c) {
                r += 1;
                continue;
            }
            let start = r;
            while r < height && is_open(r, c) {
                r += 1;
            }
            if r - start >= 2 {
                words.push((start..r).map(|rr| rr * width + c).collect());
            }
        }
    }
    words
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: cli <grid_file> <dict_file>");
        std::process::exit(1);
    }
    let rows = read_grid_rows(&args[1]);
    let word_slots = compute_words(&rows);
    let grid = Grid::new(word_slots);

    let dict_words: Vec<String> = fs::read_to_string(&args[2])
        .expect("failed to read dict file")
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| l.split(';').next().unwrap().to_string())
        .collect();
    let dict = Dictionary::from_vec(dict_words);

    let start = Instant::now();
    let result = solve(&grid, &dict);
    let elapsed = start.elapsed().as_secs_f64();

    match result {
        Some(_) => println!("SOLVED time={:.4}s", elapsed),
        None => println!("UNSAT time={:.4}s", elapsed),
    }
}
