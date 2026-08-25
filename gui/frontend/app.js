"use strict";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let puzzle = null;      // {width,height,blocks,letters,title,author,copyright,notes,clues}
let slots = [];         // server-computed slot list
let stats = {};
let selected = null;    // {row, col}
let direction = "across";
let dictionaries = [];
let dictSelections = {
  across: { path: "", minScore: 0 },
  down: { path: "", minScore: 0 },
};
let symmetryMode = "rotational180";
let americanStyle = true;
let pendingBlockToggle = null; // setTimeout id, used to disambiguate a click from the first half of a double-click
let slotsRequestSeq = 0;   // guards against an in-flight /api/puzzle/slots response arriving after a newer one
let optionsRequestSeq = 0; // same, for /api/options

const EMPTY = "-";

// ---------------------------------------------------------------------------
// API helpers
// ---------------------------------------------------------------------------

async function api(path, options = {}) {
  const resp = await fetch(path, options);
  if (!resp.ok) {
    let detail = resp.statusText;
    try {
      const body = await resp.json();
      detail = body.detail || detail;
    } catch (_) {}
    throw new Error(detail);
  }
  const ct = resp.headers.get("content-type") || "";
  if (ct.includes("application/json")) return resp.json();
  return resp;
}

function apiJson(path, body) {
  return api(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

function setStatus(msg, kind) {
  const el = document.getElementById("status-line");
  el.textContent = msg;
  el.className = kind || "";
}

// ---------------------------------------------------------------------------
// Puzzle mutation + sync
// ---------------------------------------------------------------------------

async function newPuzzle(width, height) {
  const data = await apiJson(`/api/puzzle/new?width=${width}&height=${height}`, {});
  puzzle = data.puzzle;
  slots = data.slots;
  selected = null;
  renderAll();
}

async function refreshSlotsAndStats() {
  // Typing fast (or toggling several blocks quickly) can have more than
  // one of these in flight; only the most recently *issued* one's
  // response should ever get applied, regardless of which one's response
  // happens to land first over the network.
  const seq = ++slotsRequestSeq;
  const data = await apiJson("/api/puzzle/slots", puzzle);
  if (seq !== slotsRequestSeq) return;
  slots = data.slots;
  stats = data.stats;
  renderGrid();
  renderClues();
  renderSummary();
  updateOptionsPanel();
}

// Mirror position for the currently selected symmetry mode, or null for
// "none" / out-of-bounds (the two diagonal modes only make sense on a
// square grid; the select disables them otherwise, but this guards
// against a stale selection surviving a resize).
function symmetryMirror(r, c) {
  const w = puzzle.width, h = puzzle.height;
  let mr, mc;
  switch (symmetryMode) {
    case "rotational180": mr = h - 1 - r; mc = w - 1 - c; break;
    case "horizontal": mr = r; mc = w - 1 - c; break;
    case "vertical": mr = h - 1 - r; mc = c; break;
    case "diagonal-main": mr = c; mc = r; break;
    case "diagonal-anti": mr = w - 1 - c; mc = h - 1 - r; break;
    default: return null;
  }
  if (mr < 0 || mr >= h || mc < 0 || mc >= w) return null;
  return [mr, mc];
}

function toggleBlockAt(r, c) {
  const newState = !puzzle.blocks[r][c];
  puzzle.blocks[r][c] = newState;
  puzzle.letters[r][c] = EMPTY;
  const mirror = symmetryMirror(r, c);
  if (mirror) {
    const [mr, mc] = mirror;
    puzzle.blocks[mr][mc] = newState;
    puzzle.letters[mr][mc] = EMPTY;
  }
  // Render immediately with the local mutation -- don't wait on the
  // slots/stats round trip just to show the block that was just placed
  // (that used to be the only redraw path, so a toggle looked like it did
  // nothing until the network response landed a beat later).
  renderGrid();
  refreshSlotsAndStats();
}

function setLetterAt(r, c, ch) {
  if (puzzle.blocks[r][c]) return;
  puzzle.letters[r][c] = ch || EMPTY;
}

// American-style construction rules: every open cell must be checked
// (crossed) both across and down, and no entry may be shorter than 3
// letters. compute_slots() already only creates slots for runs of length
// >= 2, so a length-1 run simply produces no slot at all in that
// direction -- naturally falling out of the "not covered" check below
// without needing to special-case it.
function computeStyleIssues() {
  const issues = new Set();
  if (!americanStyle) return issues;
  const coverage = new Map(); // "r,c" -> Set(direction)
  for (const s of slots) {
    for (const [r, c] of s.cells) {
      const key = `${r},${c}`;
      if (!coverage.has(key)) coverage.set(key, new Set());
      coverage.get(key).add(s.direction);
      if (s.length <= 2) issues.add(key);
    }
  }
  for (let r = 0; r < puzzle.height; r++) {
    for (let c = 0; c < puzzle.width; c++) {
      if (puzzle.blocks[r][c]) continue;
      const dirs = coverage.get(`${r},${c}`);
      if (!dirs || !dirs.has("across") || !dirs.has("down")) {
        issues.add(`${r},${c}`);
      }
    }
  }
  return issues;
}

function computeSymmetryDiscrepancies() {
  const bad = new Set();
  if (symmetryMode === "none") return bad;
  for (let r = 0; r < puzzle.height; r++) {
    for (let c = 0; c < puzzle.width; c++) {
      const mirror = symmetryMirror(r, c);
      if (!mirror) continue;
      const [mr, mc] = mirror;
      if (puzzle.blocks[r][c] !== puzzle.blocks[mr][mc]) {
        bad.add(`${r},${c}`);
      }
    }
  }
  return bad;
}

// ---------------------------------------------------------------------------
// Grid rendering + interaction
// ---------------------------------------------------------------------------

function slotStartNumbers() {
  const map = new Map();
  for (const s of slots) {
    map.set(`${s.row},${s.col}`, s.number);
  }
  return map;
}

function slotsAt(r, c) {
  return slots.filter((s) => s.cells.some(([rr, cc]) => rr === r && cc === c));
}

function currentSlot() {
  if (!selected) return null;
  const here = slotsAt(selected.row, selected.col);
  let s = here.find((s) => s.direction === direction);
  if (!s && here.length) {
    s = here[0];
    direction = s.direction;
  }
  return s || null;
}

function renderGrid() {
  const grid = document.getElementById("grid");
  grid.style.gridTemplateColumns = `repeat(${puzzle.width}, 36px)`;
  grid.innerHTML = "";
  const numbers = slotStartNumbers();
  const active = currentSlot();
  const activeCells = new Set((active ? active.cells : []).map(([r, c]) => `${r},${c}`));
  const styleIssues = computeStyleIssues();
  const symmetryIssues = computeSymmetryDiscrepancies();

  for (let r = 0; r < puzzle.height; r++) {
    for (let c = 0; c < puzzle.width; c++) {
      const cell = document.createElement("div");
      cell.className = "cell";
      const blocked = puzzle.blocks[r][c];
      const key = `${r},${c}`;
      if (blocked) cell.classList.add("block");
      if (activeCells.has(key)) cell.classList.add("in-word");
      if (selected && selected.row === r && selected.col === c) cell.classList.add("selected");
      if (styleIssues.has(key) || symmetryIssues.has(key)) cell.classList.add("style-issue");

      if (!blocked) {
        const num = numbers.get(`${r},${c}`);
        if (num) {
          const numEl = document.createElement("div");
          numEl.className = "num";
          numEl.textContent = num;
          cell.appendChild(numEl);
        }
        const letter = puzzle.letters[r][c];
        if (letter && letter !== EMPTY) {
          cell.appendChild(document.createTextNode(letter));
        }
      }

      cell.addEventListener("click", () => onCellClick(r, c));
      cell.addEventListener("dblclick", () => onCellDoubleClick(r, c));
      grid.appendChild(cell);
    }
  }
}

// A plain click on the already-selected cell toggles its block -- but
// that same click is also the first half of a double-click, which instead
// changes direction (onCellDoubleClick). The two can't be told apart from
// a single click event alone, so the toggle is delayed briefly; if a
// dblclick follows, it cancels the pending toggle before it fires.
function onCellClick(r, c) {
  if (selected && selected.row === r && selected.col === c) {
    if (pendingBlockToggle) clearTimeout(pendingBlockToggle);
    pendingBlockToggle = setTimeout(() => {
      pendingBlockToggle = null;
      toggleBlockAt(r, c);
    }, 250);
    return;
  }
  selected = { row: r, col: c };
  renderGrid();
  updateOptionsPanel();
  highlightActiveClue();
}

function onCellDoubleClick(r, c) {
  if (pendingBlockToggle) {
    clearTimeout(pendingBlockToggle);
    pendingBlockToggle = null;
  }
  selected = { row: r, col: c };
  direction = direction === "across" ? "down" : "across";
  renderGrid();
  updateOptionsPanel();
  highlightActiveClue();
}

function moveSelection(dr, dc) {
  if (!selected) {
    selected = { row: 0, col: 0 };
  } else {
    let r = selected.row + dr;
    let c = selected.col + dc;
    r = Math.max(0, Math.min(puzzle.height - 1, r));
    c = Math.max(0, Math.min(puzzle.width - 1, c));
    selected = { row: r, col: c };
  }
  renderGrid();
  updateOptionsPanel();
  highlightActiveClue();
}

function advanceInDirection(step) {
  if (!selected) return;
  const dr = direction === "down" ? step : 0;
  const dc = direction === "across" ? step : 0;
  const r = selected.row + dr;
  const c = selected.col + dc;
  if (r < 0 || r >= puzzle.height || c < 0 || c >= puzzle.width) return;
  if (puzzle.blocks[r][c]) return;
  selected = { row: r, col: c };
  renderGrid();
  updateOptionsPanel();
  highlightActiveClue();
}

document.addEventListener("keydown", (e) => {
  if (!puzzle) return;

  // Cmd+F (Mac) / Ctrl+F (elsewhere) fills the puzzle -- checked before
  // any other bailout, so it works even without a selected cell and even
  // while a clue/dictionary input has focus, matching how a browser's own
  // Ctrl/Cmd+F "Find" shortcut is global rather than field-scoped.
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "f") {
    e.preventDefault();
    runFill();
    return;
  }

  if (!selected) return;
  const tag = (document.activeElement && document.activeElement.tagName) || "";
  if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") return;

  const { row, col } = selected;
  if (e.key === ".") {
    e.preventDefault();
    toggleBlockAt(row, col);
  } else if (e.key === "ArrowUp" || e.key === "ArrowDown") {
    // Arrow keys change direction too: a perpendicular arrow (vertical,
    // here) switches into "down" as well as moving, the same convention
    // most crossword solving apps use -- so pressing it always both
    // orients and moves, rather than requiring Space first.
    e.preventDefault();
    direction = "down";
    moveSelection(e.key === "ArrowUp" ? -1 : 1, 0);
  } else if (e.key === "ArrowLeft" || e.key === "ArrowRight") {
    e.preventDefault();
    direction = "across";
    moveSelection(0, e.key === "ArrowLeft" ? -1 : 1);
  } else if (e.key === " ") {
    e.preventDefault();
    direction = direction === "across" ? "down" : "across";
    renderGrid();
    updateOptionsPanel();
    highlightActiveClue();
  } else if (e.key === "Backspace") {
    e.preventDefault();
    if (!puzzle.blocks[row][col] && puzzle.letters[row][col] !== EMPTY) {
      setLetterAt(row, col, EMPTY);
      renderGrid();
      refreshSlotsAndStats();
    } else {
      advanceInDirection(-1);
    }
  } else if (/^[a-zA-Z]$/.test(e.key)) {
    e.preventDefault();
    if (!puzzle.blocks[row][col]) {
      setLetterAt(row, col, e.key.toUpperCase());
      renderGrid();
      refreshSlotsAndStats();
      advanceInDirection(1);
    }
  }
});

// ---------------------------------------------------------------------------
// Summary tab
// ---------------------------------------------------------------------------

function renderSummary() {
  const general = document.getElementById("stats-general");
  const rows = [
    ["Words", stats.word_count],
    ["Avg. word length", stats.avg_word_length],
    ["Blocks", `${stats.block_count} (${stats.block_percent}%)`],
    ["Letters filled", stats.letter_count],
  ];
  general.innerHTML = rows.map(([k, v]) => `<tr><td>${k}</td><td>${v ?? ""}</td></tr>`).join("");

  const lengths = document.getElementById("stats-lengths");
  const entries = Object.entries(stats.length_breakdown || {});
  lengths.innerHTML = entries
    .map(([len, count]) => `<tr><td>${len} letters</td><td>${count}</td></tr>`)
    .join("");

  renderLetterGrid();
}

function renderLetterGrid() {
  const el = document.getElementById("letter-grid");
  const counts = stats.letter_counts || {};
  el.innerHTML = "";
  for (let i = 0; i < 26; i++) {
    const letter = String.fromCharCode(65 + i);
    const div = document.createElement("div");
    div.className = "letter-cell";
    div.innerHTML = `<span class="lc">${letter}</span><span class="lv">${counts[letter] || 0}</span>`;
    el.appendChild(div);
  }
}

// ---------------------------------------------------------------------------
// Clues tab
// ---------------------------------------------------------------------------

function renderClues() {
  const across = slots.filter((s) => s.direction === "across").sort((a, b) => a.number - b.number);
  const down = slots.filter((s) => s.direction === "down").sort((a, b) => a.number - b.number);
  const activeId = currentSlot() ? currentSlot().id : null;

  const build = (list) =>
    list
      .map(
        (s) => `
      <div class="clue-row${s.id === activeId ? " active-slot" : ""}" data-slot="${s.id}">
        <span class="clue-num">${s.number}</span>${s.pattern}
        <input type="text" value="${escapeAttr(puzzle.clues[s.id] || "")}" data-slot-input="${s.id}" placeholder="Clue text…" />
      </div>`
      )
      .join("");

  document.getElementById("clues-across").innerHTML = build(across);
  document.getElementById("clues-down").innerHTML = build(down);

  document.querySelectorAll("[data-slot-input]").forEach((input) => {
    input.addEventListener("input", (e) => {
      const id = e.target.getAttribute("data-slot-input");
      puzzle.clues[id] = e.target.value;
    });
  });
  document.querySelectorAll(".clue-row").forEach((row) => {
    row.addEventListener("click", (e) => {
      if (e.target.tagName === "INPUT") return;
      const id = row.getAttribute("data-slot");
      const s = slots.find((s) => s.id === id);
      if (!s) return;
      selected = { row: s.row, col: s.col };
      direction = s.direction;
      renderGrid();
      updateOptionsPanel();
      highlightActiveClue();
    });
  });
}

function highlightActiveClue() {
  const activeId = currentSlot() ? currentSlot().id : null;
  document.querySelectorAll(".clue-row").forEach((row) => {
    row.classList.toggle("active-slot", row.getAttribute("data-slot") === activeId);
  });
}

function escapeAttr(s) {
  return String(s).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;");
}

// ---------------------------------------------------------------------------
// Options ("Fill") tab
// ---------------------------------------------------------------------------

async function updateOptionsPanel() {
  const heading = document.getElementById("options-heading");
  const patternEl = document.getElementById("options-pattern");
  const listEl = document.getElementById("options-list");
  const s = currentSlot();
  if (!s) {
    heading.textContent = "Options for selected slot";
    patternEl.textContent = "";
    listEl.innerHTML = "";
    return;
  }
  heading.textContent = `${s.direction === "across" ? "Across" : "Down"} ${s.number} — options`;
  patternEl.textContent = s.pattern.replace(/\?/g, "_");

  const sel = s.direction === "across" ? dictSelections.across : dictSelections.down;
  if (!sel.path) {
    listEl.innerHTML = '<div class="hint">Select a dictionary in the Dictionaries tab.</div>';
    return;
  }
  const seq = ++optionsRequestSeq;
  try {
    const data = await apiJson("/api/options", {
      pattern: s.pattern,
      dict_path: sel.path,
      min_score: sel.minScore,
      limit: 50,
    });
    if (seq !== optionsRequestSeq) return; // a newer selection has since superseded this request
    if (!data.candidates.length) {
      listEl.innerHTML = '<div class="hint">No matches.</div>';
      return;
    }
    listEl.innerHTML = data.candidates
      .map((c) => `<div class="option-row" data-word="${c.word}"><span class="word">${c.word}</span><span class="score">${c.score}</span></div>`)
      .join("");
    listEl.querySelectorAll(".option-row").forEach((row) => {
      row.addEventListener("click", () => applyWordToSlot(s, row.getAttribute("data-word")));
    });
  } catch (err) {
    if (seq !== optionsRequestSeq) return;
    listEl.innerHTML = `<div class="hint">${err.message}</div>`;
  }
}

function applyWordToSlot(slot, word) {
  for (let i = 0; i < slot.cells.length; i++) {
    const [r, c] = slot.cells[i];
    puzzle.letters[r][c] = word[i];
  }
  renderGrid();
  refreshSlotsAndStats();
}

// ---------------------------------------------------------------------------
// Dictionaries tab
// ---------------------------------------------------------------------------

async function loadDictionaries() {
  const data = await api("/api/dictionaries");
  dictionaries = data.dictionaries;
  const acrossSel = document.getElementById("across-dict-select");
  const downSel = document.getElementById("down-dict-select");
  const options = dictionaries.map((d) => `<option value="${d.path}">${escapeAttr(d.name)}</option>`).join("");
  acrossSel.innerHTML = options;
  downSel.innerHTML = options;
  if (dictionaries.length) {
    dictSelections.across.path = acrossSel.value = dictionaries[0].path;
    dictSelections.down.path = downSel.value = dictionaries[0].path;
  }
}

function wireDictTab() {
  const acrossSel = document.getElementById("across-dict-select");
  const downSel = document.getElementById("down-dict-select");
  const acrossMin = document.getElementById("across-min-score");
  const downMin = document.getElementById("down-min-score");

  acrossSel.addEventListener("change", () => {
    dictSelections.across.path = acrossSel.value;
    updateOptionsPanel();
  });
  downSel.addEventListener("change", () => {
    dictSelections.down.path = downSel.value;
    updateOptionsPanel();
  });
  acrossMin.addEventListener("input", () => {
    dictSelections.across.minScore = parseInt(acrossMin.value || "0", 10);
    updateOptionsPanel();
  });
  downMin.addEventListener("input", () => {
    dictSelections.down.minScore = parseInt(downMin.value || "0", 10);
    updateOptionsPanel();
  });

  document.getElementById("input-dict-upload").addEventListener("change", async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    const formData = new FormData();
    formData.append("file", file);
    await api("/api/dictionaries/upload", { method: "POST", body: formData });
    await loadDictionaries();
    setStatus(`Uploaded dictionary "${file.name}"`, "ok");
  });
}

// ---------------------------------------------------------------------------
// Info tab
// ---------------------------------------------------------------------------

function wireInfoTab() {
  const fields = [
    ["meta-title", "title"],
    ["meta-author", "author"],
    ["meta-copyright", "copyright"],
    ["meta-notes", "notes"],
  ];
  for (const [elId, key] of fields) {
    document.getElementById(elId).addEventListener("input", (e) => {
      puzzle[key] = e.target.value;
    });
  }
}

function renderInfo() {
  document.getElementById("meta-title").value = puzzle.title || "";
  document.getElementById("meta-author").value = puzzle.author || "";
  document.getElementById("meta-copyright").value = puzzle.copyright || "";
  document.getElementById("meta-notes").value = puzzle.notes || "";
}

// ---------------------------------------------------------------------------
// Toolbar: new / import / export / fill
// ---------------------------------------------------------------------------

function wireToolbar() {
  document.getElementById("btn-new").addEventListener("click", async () => {
    const width = parseInt(prompt("Grid width", "15") || "", 10);
    const height = parseInt(prompt("Grid height", "15") || "", 10);
    if (!width || !height) return;
    await newPuzzle(width, height);
    setStatus(`New ${width}×${height} grid`, "ok");
  });

  document.getElementById("input-import").addEventListener("change", async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    const formData = new FormData();
    formData.append("file", file);
    try {
      const data = await api("/api/puzzle/import", { method: "POST", body: formData });
      puzzle = data.puzzle;
      slots = data.slots;
      selected = null;
      renderAll();
      setStatus(data.warning ? `Imported "${file.name}" — ${data.warning}` : `Imported "${file.name}"`, data.warning ? "error" : "ok");
    } catch (err) {
      setStatus(`Import failed: ${err.message}`, "error");
    }
    e.target.value = "";
  });

  const exportBtn = document.getElementById("btn-export");
  const exportMenu = document.getElementById("export-menu");
  exportBtn.addEventListener("click", () => exportMenu.classList.toggle("open"));
  document.addEventListener("click", (e) => {
    if (!exportBtn.contains(e.target) && !exportMenu.contains(e.target)) {
      exportMenu.classList.remove("open");
    }
  });
  exportMenu.querySelectorAll("button").forEach((btn) => {
    btn.addEventListener("click", async () => {
      exportMenu.classList.remove("open");
      const format = btn.getAttribute("data-format");
      try {
        const resp = await api(`/api/puzzle/export?format=${format}`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(puzzle),
        });
        const blob = await resp.blob();
        const disposition = resp.headers.get("Content-Disposition") || "";
        const match = /filename="([^"]+)"/.exec(disposition);
        const filename = match ? match[1] : `puzzle.${format}`;
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
        setStatus(`Exported ${filename}`, "ok");
      } catch (err) {
        setStatus(`Export failed: ${err.message}`, "error");
      }
    });
  });

  document.getElementById("btn-fill").addEventListener("click", runFill);
}

let filling = false;

// Shared by the Fill button and the Cmd+F / Ctrl+F shortcut.
async function runFill() {
  if (!puzzle || filling) return;
  if (!dictSelections.across.path || !dictSelections.down.path) {
    setStatus("Select across/down dictionaries first (Dictionaries tab)", "error");
    return;
  }
  filling = true;
  setStatus("Filling…");
  try {
    const data = await apiJson("/api/fill", {
      puzzle,
      across_dict_path: dictSelections.across.path,
      across_min_score: dictSelections.across.minScore,
      down_dict_path: dictSelections.down.path,
      down_min_score: dictSelections.down.minScore,
      threads: 0,
    });
    puzzle = data.puzzle;
    await refreshSlotsAndStats();
    renderGrid();
    const st = data.stats;
    if (data.solved) {
      setStatus(`Solved in ${st.time_seconds.toFixed(2)}s (${st.nodes} nodes, ${st.restarts} restarts)`, "ok");
    } else {
      setStatus(`No solution found (${st.time_seconds.toFixed(2)}s)`, "error");
    }
  } catch (err) {
    setStatus(`Fill failed: ${err.message}`, "error");
  } finally {
    filling = false;
  }
}

// ---------------------------------------------------------------------------
// Style controls: American-style highlighting + symmetry mode
// ---------------------------------------------------------------------------

function wireStyleControls() {
  document.getElementById("chk-american-style").addEventListener("change", (e) => {
    americanStyle = e.target.checked;
    renderGrid();
  });
  document.getElementById("symmetry-select").addEventListener("change", (e) => {
    symmetryMode = e.target.value;
    renderGrid();
  });
}

// The two diagonal modes only make sense on a square grid -- disable them
// otherwise rather than silently no-op-ing a selection that can't apply.
function updateSymmetryOptionAvailability() {
  const select = document.getElementById("symmetry-select");
  const square = puzzle && puzzle.width === puzzle.height;
  for (const opt of select.options) {
    if (opt.value.startsWith("diagonal-")) opt.disabled = !square;
  }
  if (!square && symmetryMode.startsWith("diagonal-")) {
    symmetryMode = "rotational180";
    select.value = symmetryMode;
  }
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

function wireTabs() {
  document.querySelectorAll(".tab-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      document.querySelectorAll(".tab-btn").forEach((b) => b.classList.remove("active"));
      document.querySelectorAll(".tab-panel").forEach((p) => p.classList.remove("active"));
      btn.classList.add("active");
      document.getElementById(`tab-${btn.getAttribute("data-tab")}`).classList.add("active");
    });
  });
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

function renderAll() {
  updateSymmetryOptionAvailability();
  renderGrid();
  renderClues();
  renderInfo();
  updateOptionsPanel();
  refreshSlotsAndStats();
}

async function main() {
  wireToolbar();
  wireTabs();
  wireDictTab();
  wireInfoTab();
  wireStyleControls();
  await loadDictionaries();
  await newPuzzle(15, 15);
}

main().catch((err) => setStatus(err.message, "error"));
