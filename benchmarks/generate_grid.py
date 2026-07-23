import random

def make_grid(size=15, target_block_pairs=18, seed=1, min_run=3):
    random.seed(seed)
    blocked = [[False]*size for _ in range(size)]

    def run_ok(row_or_col):
        # No run of open cells has length 1 or 2 (0 is fine -- that's a block).
        run = 0
        for open_cell in row_or_col:
            if open_cell:
                run += 1
            else:
                if 0 < run < min_run:
                    return False
                run = 0
        if 0 < run < min_run:
            return False
        return True

    def all_rows_cols_ok():
        for r in range(size):
            row = [not blocked[r][c] for c in range(size)]
            if not run_ok(row):
                return False
        for c in range(size):
            col = [not blocked[r][c] for r in range(size)]
            if not run_ok(col):
                return False
        return True

    placed = 0
    attempts = 0
    cells = [(r, c) for r in range(size) for c in range(size) if r <= c or (r == c)]
    # try random symmetric pairs
    while placed < target_block_pairs and attempts < 5000:
        attempts += 1
        r = random.randint(0, size - 1)
        c = random.randint(0, size - 1)
        r2, c2 = size - 1 - r, size - 1 - c
        if blocked[r][c] or blocked[r2][c2]:
            continue
        blocked[r][c] = True
        blocked[r2][c2] = True
        if all_rows_cols_ok():
            placed += 1
        else:
            blocked[r][c] = False
            blocked[r2][c2] = False

    return blocked

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(
        description="Generate a symmetric, playable crossword grid spec.")
    parser.add_argument("--size", type=int, default=15)
    parser.add_argument("--block-pairs", type=int, default=18,
                         help="Number of symmetric block pairs to place.")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--min-run", type=int, default=3,
                         help="Minimum open-run length (no 1/2-letter slots).")
    args = parser.parse_args()

    blocked = make_grid(size=args.size, target_block_pairs=args.block_pairs,
                         seed=args.seed, min_run=args.min_run)
    for row in blocked:
        print(''.join('#' if b else '.' for b in row))
