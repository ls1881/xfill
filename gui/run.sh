#!/usr/bin/env bash
# Launches the xfill crossword-construction GUI. First run sets up a venv
# and installs backend deps; subsequent runs just start the server.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

if [ ! -d .venv ]; then
  python3 -m venv .venv
  ./.venv/bin/pip install --quiet --upgrade pip
  ./.venv/bin/pip install --quiet -r backend/requirements.txt
fi

if [ ! -x ../build/xfill_cli ]; then
  echo "xfill_cli not built yet -- building it now..." >&2
  (cd .. && cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build --target xfill_cli --parallel)
fi

PORT="${PORT:-8791}"
echo "Starting xfill GUI at http://127.0.0.1:${PORT}"
exec ./.venv/bin/uvicorn app:app --app-dir backend --port "$PORT"
