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
let slotsRequestSeq = 0;   // guards against an in-flight /api/puzzle/slots response arriving after a newer one
let optionsRequestSeq = 0; // same, for /api/options
let lastClick = { row: null, col: null, time: 0 }; // same-cell click timing, used to detect a double-click ourselves
const DOUBLE_CLICK_MS = 350;
let undoStack = []; // deep-cloned puzzle snapshots, most recent last
const MAX_UNDO = 100;
let fillFailedCells = new Set(); // "r,c" keys highlighted after a failed Fill; cleared on the next edit

// Per-candidate solve-feasibility checks for the Options panel (see
// updateOptionsPanel): word -> {feasible: true|false, grid: [...] | null}.
// Reset whenever the (slot, pattern, dictionary) combination changes --
// see lastVerifiedKey.
let verifiedResults = new Map();
let lastVerifiedKey = null;
let verifyBatchToken = 0; // incremented per batch; a stale batch stops issuing further checks
const VERIFY_LIMIT = 10; // how many top-scored candidates get checked per slot
const VERIFY_THREADS = 2; // kept modest since these run one at a time, in the background

// How the Options list is ordered: "smart" (verified first, then score,
// then A-Z -- the default) sorts a candidate the moment it's confirmed
// verified, not just when the list is first fetched, so a row visibly
// jumps to the top as its background check completes. "score" and
// "alpha" ignore verified status entirely.
let optionsSortMode = "smart";
// The full candidate list + slot from the last successful /api/options
// fetch, kept so changing the sort dropdown can just re-render (see
// wireOptionsSort) instead of re-fetching from the server for a purely
// client-side reordering.
let lastRenderedSlot = null;
let lastRenderedCandidates = [];

// The grid preview shown when a verified candidate is clicked: a full
// solved grid (row strings, '#' for block) from that candidate's own
// verify check. Only ever drawn into cells the user hasn't actually
// filled in yet (see renderGrid).
let previewSlotId = null;
let previewWord = null;
let previewGrid = null;

const EMPTY = "-";

// ---------------------------------------------------------------------------
// Local persistence -- so reloading the page (or reopening the app later)
// picks up where you left off instead of starting from a blank grid.
// Purely client-side (localStorage), no backend involved: this app has no
// server-side session (see app.py's module docstring), and a single
// "resume my last grid" convenience doesn't need one either.
// ---------------------------------------------------------------------------

const SAVE_KEY = "xfill-gui-state-v1";
let saveTimer = null;

// Debounced so rapid typing doesn't hit localStorage on every keystroke --
// called from every place puzzle/dictSelections/style-control state
// actually changes (see renderGrid, wireInfoTab, renderClues,
// wireDictTab, wireStyleControls, wireOptionsSort).
function scheduleSave() {
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(saveStateNow, 400);
}

function saveStateNow() {
  if (!puzzle) return;
  try {
    localStorage.setItem(
      SAVE_KEY,
      JSON.stringify({ puzzle, dictSelections, symmetryMode, americanStyle, optionsSortMode })
    );
  } catch (_) {
    // localStorage can throw (quota exceeded, some browsers' private
    // windows, storage disabled, ...) -- losing autosave silently beats
    // crashing the app over a convenience feature.
  }
}

// Returns the saved state if present and structurally sane, else null.
// Deliberately paranoid about validating shape: this is user-editable
// browser storage (and the save format could change across versions of
// this app), so a corrupt or stale value should fall back to a blank grid
// rather than half-apply and break rendering.
function loadSavedState() {
  let data;
  try {
    const raw = localStorage.getItem(SAVE_KEY);
    if (!raw) return null;
    data = JSON.parse(raw);
  } catch (_) {
    return null;
  }
  const p = data && data.puzzle;
  if (
    !p ||
    typeof p.width !== "number" ||
    typeof p.height !== "number" ||
    !Array.isArray(p.blocks) ||
    !Array.isArray(p.letters) ||
    p.blocks.length !== p.height ||
    p.letters.length !== p.height
  ) {
    return null;
  }
  return data;
}

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
  document.getElementById("status-text").textContent = msg;
  document.getElementById("status-line").className = kind || "";
}

function setFillSpinner(visible) {
  document.getElementById("fill-spinner").hidden = !visible;
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

// Call before any in-place mutation of `puzzle` -- captures the
// pre-mutation state so undo() can restore it. A plain deep clone is fine
// here: puzzle objects are small (a grid's worth of strings/booleans plus
// a clues map), and undo doesn't need to be fast, just correct.
function snapshotForUndo() {
  if (!puzzle) return;
  undoStack.push(JSON.parse(JSON.stringify(puzzle)));
  if (undoStack.length > MAX_UNDO) undoStack.shift();
  clearFillFailedHighlight();
  clearPreview();
}

function clearFillFailedHighlight() {
  if (fillFailedCells.size) fillFailedCells = new Set();
}

// A previewed candidate's grid is only meaningful relative to the exact
// puzzle state it was verified against and the slot it was requested for
// -- any real edit or change of selection invalidates it. Called from
// snapshotForUndo() (every real grid edit already goes through that) and
// directly wherever `selected`/`direction` change on their own without an
// edit (moveSelection, onCellClick, etc).
function clearPreview() {
  previewSlotId = null;
  previewWord = null;
  previewGrid = null;
}

function undo() {
  if (!undoStack.length) {
    setStatus("Nothing to undo", "error");
    return;
  }
  puzzle = undoStack.pop();
  // Bounds/direction could be stale if the undone step crossed a New or
  // Import (different grid size) -- drop selection rather than risk it
  // pointing outside the restored grid.
  selected = null;
  clearFillFailedHighlight();
  renderAll();
  setStatus("Undid last change", "ok");
}

// ---------------------------------------------------------------------------
// Puzzle mutation + sync
// ---------------------------------------------------------------------------

async function newPuzzle(width, height) {
  const previous = puzzle;
  const data = await apiJson(`/api/puzzle/new?width=${width}&height=${height}`, {});
  if (previous) {
    undoStack.push(previous);
    if (undoStack.length > MAX_UNDO) undoStack.shift();
  }
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
  snapshotForUndo();
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
  scheduleSave(); // every puzzle-content mutation renders the grid afterward, so this is the one reliable choke point for autosave
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
      if (fillFailedCells.has(key)) cell.classList.add("fill-failed");

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
        } else if (previewGrid && previewGrid[r][c] !== "#") {
          // A previewed candidate's letter for a cell the user hasn't
          // actually filled yet -- dimmed so it reads as "not committed,"
          // and never shown over a real letter (checked above: this
          // branch only runs when the cell is still EMPTY).
          const previewEl = document.createElement("span");
          previewEl.className = "preview-letter";
          previewEl.textContent = previewGrid[r][c];
          cell.appendChild(previewEl);
        }
      }

      cell.addEventListener("click", () => onCellClick(r, c));
      grid.appendChild(cell);
    }
  }
}

// Clicking NEVER places or removes a block, no matter how many times or
// how fast -- only "." and Backspace do that (see the keydown handler).
// An earlier version toggled a block on a second click of an
// already-selected cell, which produced exactly the bug this comment used
// to describe workarounds for: a double-click (meant to change direction)
// landing as two ordinary clicks could still leave a scheduled toggle
// armed, and a *triple* click -- the double-click consuming clicks 1-2,
// then click 3 landing on the now-selected cell as an unrelated "second
// click" with a reset click-timer -- reliably placed a block nobody
// asked for. Removing click-driven toggling entirely removes the
// ambiguity at its root instead of chasing further edge cases in it.
//
// Double-click detection itself is still done by hand, purely from click
// timestamps on the same (row, col): renderGrid() rebuilds every cell
// <div> from scratch on each render (including the one just clicked,
// since a click on a new cell re-renders to show the new selection), and
// relying on the browser's own click-target tracking across a DOM node
// being replaced mid-gesture proved unreliable -- double-clicks were
// landing as two independent single clicks instead of being recognized as
// one gesture at all.
function onCellClick(r, c) {
  const now = Date.now();
  const isDoubleClick =
    lastClick.row === r && lastClick.col === c && now - lastClick.time < DOUBLE_CLICK_MS;
  lastClick = isDoubleClick ? { row: null, col: null, time: 0 } : { row: r, col: c, time: now };

  selected = { row: r, col: c };
  if (isDoubleClick) direction = direction === "across" ? "down" : "across";
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

// Backspace's backward step, used instead of advanceInDirection(-1): when
// the cell one step back (in the current across/down direction) is a
// block, this removes it and moves onto it, rather than refusing to move
// at all. Lets holding Backspace "eat backward" through a block the same
// way it already eats through letters, instead of getting stuck right in
// front of one. Deliberately NOT folded into advanceInDirection itself --
// that function is also used for the forward step after typing a letter,
// where hitting a block should keep stopping the cursor there, not erase
// grid structure just because the user kept typing.
function backspaceStepBack() {
  if (!selected) return;
  const dr = direction === "down" ? -1 : 0;
  const dc = direction === "across" ? -1 : 0;
  const r = selected.row + dr;
  const c = selected.col + dc;
  if (r < 0 || r >= puzzle.height || c < 0 || c >= puzzle.width) return;
  if (puzzle.blocks[r][c]) {
    toggleBlockAt(r, c); // removes it (also snapshots for undo, re-renders)
  }
  selected = { row: r, col: c };
  renderGrid();
  updateOptionsPanel();
  highlightActiveClue();
}

// Cmd+F/Ctrl+F (fill), Cmd+Z/Ctrl+Z (undo), and Escape (cancel an
// in-progress fill) are registered on their own CAPTURE-phase listener,
// ahead of everything else including the browser's own handling: a
// bubble-phase listener plus preventDefault() was not enough to reliably
// stop the browser's native "Find in page" from also opening in every
// browser tested, since some browsers resolve that shortcut before page
// JS ever sees it in the bubble phase. Capture-phase interception is the
// more reliable way to win that race. (Some browsers -- notably Safari on
// macOS -- bind Find at the OS/Services level in a way no page script can
// override; if it still opens there, that's a browser limitation the
// Fill/Undo buttons remain the reliable fallback for, not a bug in this
// handler.)
document.addEventListener(
  "keydown",
  (e) => {
    if (!puzzle) return;

    if (e.key === "Escape" && filling) {
      e.preventDefault();
      e.stopPropagation();
      cancelFill();
      return;
    }

    const key = e.key.toLowerCase();
    if (!(e.metaKey || e.ctrlKey) || (key !== "f" && key !== "z")) return;

    const tag = (document.activeElement && document.activeElement.tagName) || "";
    const inTextField = tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
    // Undo inside a text field should undo the *text edit* (native browser
    // behavior), not the grid -- Fill has no such field-local meaning, so
    // it stays global regardless of focus.
    if (key === "z" && inTextField) return;

    e.preventDefault();
    e.stopPropagation();
    if (key === "f") runFill();
    else undo();
  },
  true
);

document.addEventListener("keydown", (e) => {
  if (!puzzle || !selected) return;
  const tag = (document.activeElement && document.activeElement.tagName) || "";
  if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") return;

  const { row, col } = selected;
  const blocked = puzzle.blocks[row][col];
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
    if (blocked) {
      // Backspace on a block deletes it -- the same action as clicking it
      // or pressing "." while selected, just from the keyboard.
      toggleBlockAt(row, col);
    } else if (puzzle.letters[row][col] !== EMPTY) {
      // Clear the current letter AND step back in one press, so holding
      // Backspace erases one letter per press while walking back through
      // a word -- previously this only cleared the current cell and left
      // the cursor there, so the *next* press just moved (without
      // clearing) onto the still-filled previous cell, and only the press
      // after *that* cleared it: two presses per letter instead of one.
      snapshotForUndo();
      setLetterAt(row, col, EMPTY);
      renderGrid();
      refreshSlotsAndStats();
      backspaceStepBack();
    } else {
      backspaceStepBack();
    }
  } else if (/^[a-zA-Z]$/.test(e.key)) {
    e.preventDefault();
    if (!blocked) {
      snapshotForUndo();
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

  // This rebuilds every clue <input> from scratch, which would otherwise
  // steal focus (and cursor position) out from under the user's hands if
  // they're mid-typing a clue when an unrelated, still-in-flight grid
  // edit's refresh happens to resolve and call this -- e.g. typing a grid
  // letter kicks off an async refreshSlotsAndStats(), and if the user
  // switches to the Clues tab and starts typing before that resolves, it
  // would otherwise land while their cursor is in a clue field. Restoring
  // focus + selection afterward, when the focused element is one of these
  // inputs, avoids that.
  const active = document.activeElement;
  let restoreFocus = null;
  if (active && active.hasAttribute && active.hasAttribute("data-slot-input")) {
    restoreFocus = {
      slotId: active.getAttribute("data-slot-input"),
      selectionStart: active.selectionStart,
      selectionEnd: active.selectionEnd,
    };
  }

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
      scheduleSave();
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

  if (restoreFocus) {
    const input = document.querySelector(`[data-slot-input="${restoreFocus.slotId}"]`);
    if (input) {
      input.focus();
      input.setSelectionRange(restoreFocus.selectionStart, restoreFocus.selectionEnd);
    }
  }
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

  // A preview is only meaningful for the slot it was generated for --
  // this is the single choke point every selection change already
  // funnels through, so it's the one place that needs to know "the user
  // moved on" rather than sprinkling the same check across every
  // selection-changing function (click, arrows, double-click, Space,
  // clue-row click, ...).
  if (previewSlotId !== null && (!s || s.id !== previewSlotId)) {
    clearPreview();
    renderGrid();
  }

  if (!s) {
    heading.textContent = "Options for selected slot";
    patternEl.textContent = "";
    listEl.innerHTML = "";
    lastRenderedSlot = null;
    lastRenderedCandidates = [];
    return;
  }
  heading.textContent = `${s.direction === "across" ? "Across" : "Down"} ${s.number} — options`;
  patternEl.textContent = s.pattern.replace(/\?/g, "_");

  const sel = s.direction === "across" ? dictSelections.across : dictSelections.down;
  if (!sel.path) {
    listEl.innerHTML = '<div class="hint">Select a dictionary in the Dictionaries tab.</div>';
    lastRenderedSlot = null;
    lastRenderedCandidates = [];
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

    // Solve-feasibility checks (verifiedResults) are scoped to one exact
    // (slot, pattern, dictionary) combination -- if any of those changed,
    // start a fresh background verification pass and drop whatever was
    // known before, since it no longer applies to what's now on screen.
    const key = `${s.id}|${s.pattern}|${sel.path}|${sel.minScore}`;
    if (key !== lastVerifiedKey) {
      lastVerifiedKey = key;
      verifiedResults = new Map();
      startVerificationBatch(s, data.candidates);
    }

    lastRenderedSlot = s;
    lastRenderedCandidates = data.candidates;
    renderOptionsList(s, data.candidates);
  } catch (err) {
    if (seq !== optionsRequestSeq) return;
    listEl.innerHTML = `<div class="hint">${err.message}</div>`;
  }
}

function sortCandidates(candidates) {
  const arr = [...candidates];
  const verified = (c) => verifiedResults.get(c.word)?.feasible === true;
  arr.sort((a, b) => {
    if (optionsSortMode === "smart") {
      const av = verified(a) ? 1 : 0;
      const bv = verified(b) ? 1 : 0;
      if (av !== bv) return bv - av;
    }
    if (optionsSortMode !== "alpha" && a.score !== b.score) return b.score - a.score;
    return a.word.localeCompare(b.word);
  });
  return arr;
}

// Renders the candidate list against whatever verifiedResults currently
// knows: a candidate confirmed feasible (a full grid completion actually
// exists with it in this slot) is bolded; one confirmed infeasible is
// dropped from the list entirely; anything not yet checked (or checked
// and inconclusive -- see verify_option's "feasible: null" error case)
// stays plain. Called both right after fetching candidates and again,
// incrementally, as each background verification result comes in -- so a
// row re-sorts to the top the moment it's confirmed verified under the
// default "smart" order, not only when the list is first built.
function renderOptionsList(slot, candidates) {
  const listEl = document.getElementById("options-list");
  const visible = sortCandidates(candidates.filter((c) => verifiedResults.get(c.word)?.feasible !== false));
  if (!visible.length) {
    listEl.innerHTML = '<div class="hint">No matches.</div>';
    return;
  }
  listEl.innerHTML = visible
    .map((c) => {
      const verified = verifiedResults.get(c.word)?.feasible === true;
      return `<div class="option-row${verified ? " verified" : ""}" data-word="${c.word}"><span class="word">${c.word}</span><span class="score">${c.score}</span></div>`;
    })
    .join("");
  listEl.querySelectorAll(".option-row").forEach((row) => {
    row.addEventListener("click", () => onOptionClick(slot, row.getAttribute("data-word")));
  });
}

// Terminates whatever verify-check subprocess the backend is currently
// running, if any (see solver_bridge.cancel_all_verify_checks). Called
// right before a new verification batch's first request and right before
// a real Fill starts. This closes a real gap that let multiple xfill_cli
// processes accumulate and run indefinitely, confirmed directly: this
// batch's own token-based staleness check (below) only stops it from
// *issuing further* requests once superseded -- it can't reach a request
// already sent, whose subprocess by then already exists server-side with
// nothing tracking or able to stop it if the batch that launched it no
// longer cares about the answer. A user clicking through several slots
// faster than one verify solve completes hit this every time.
async function cancelAllVerifyChecks() {
  try {
    await api("/api/options/verify/cancel-all", { method: "POST" });
  } catch (_) {
    // Best-effort -- a failed cancel here shouldn't block starting the
    // next batch or Fill; the 20s server-side timeout is the backstop.
  }
}

// Fully stops verification: bumps the token FIRST so startVerificationBatch's
// loop won't issue another request once its current one (which this also
// kills) resolves, then kills whatever's currently running. Both halves
// matter -- confirmed directly: calling only cancelAllVerifyChecks()
// killed the in-flight subprocess, but the batch loop, still holding a
// valid token, just treated that as an inconclusive result for that one
// candidate and immediately moved on to spawn a new subprocess for the
// next one, so nothing ever actually stopped.
async function stopAllVerification() {
  verifyBatchToken++;
  await cancelAllVerifyChecks();
}

// Checks, one at a time in the background, whether each of the top
// VERIFY_LIMIT candidates actually leads to a complete grid (not just
// whether it matches the pattern -- see /api/options/verify). Sequential
// and capped deliberately: each check is a full solve, so checking every
// returned candidate eagerly and/or in parallel would make selecting a
// slot expensive instead of the fast, cheap thing it already is via
// slot_options' plain pattern match.
async function startVerificationBatch(slot, allCandidates) {
  const token = ++verifyBatchToken;
  await cancelAllVerifyChecks();
  if (token !== verifyBatchToken) return; // superseded again while the cancel was in flight
  const toCheck = allCandidates.slice(0, VERIFY_LIMIT);
  for (const c of toCheck) {
    if (token !== verifyBatchToken) return; // superseded by a newer slot/pattern/dictionary
    let result;
    try {
      result = await apiJson("/api/options/verify", {
        puzzle,
        slot_id: slot.id,
        word: c.word,
        across_dict_path: dictSelections.across.path,
        across_min_score: dictSelections.across.minScore,
        down_dict_path: dictSelections.down.path,
        down_min_score: dictSelections.down.minScore,
        threads: VERIFY_THREADS,
      });
    } catch (_) {
      continue; // leave this one unchecked rather than aborting the whole batch
    }
    if (token !== verifyBatchToken) return;
    if (result.feasible === true) {
      verifiedResults.set(c.word, { feasible: true, grid: result.grid });
    } else if (result.feasible === false) {
      verifiedResults.set(c.word, { feasible: false, grid: null });
    } // feasible === null (a verify-side error, not a real infeasibility finding) -- leave unset

    if (currentSlot() && currentSlot().id === slot.id) {
      renderOptionsList(slot, allCandidates);
    }
  }
}

// A confirmed-feasible candidate previews its full solved grid on click
// (dimmed, into cells the user hasn't actually filled -- see renderGrid);
// clicking that same candidate again commits it. An unverified candidate
// has no precomputed completion to preview, so it keeps the original
// behavior: click commits just that one slot's word immediately.
function onOptionClick(slot, word) {
  const verified = verifiedResults.get(word);
  if (verified?.feasible && verified.grid) {
    if (previewSlotId === slot.id && previewWord === word) {
      commitPreview();
    } else {
      previewSlotId = slot.id;
      previewWord = word;
      previewGrid = verified.grid;
      renderGrid();
    }
  } else {
    applyWordToSlot(slot, word);
  }
}

function commitPreview() {
  if (!previewGrid) return;
  const gridToApply = previewGrid; // captured before snapshotForUndo() clears the preview
  snapshotForUndo();
  for (let r = 0; r < puzzle.height; r++) {
    for (let c = 0; c < puzzle.width; c++) {
      // Only fills in still-blank cells -- never overwrites a letter the
      // user already actually typed, whether that's part of this slot or
      // one the preview grid's completion happened to also cover.
      if (!puzzle.blocks[r][c] && puzzle.letters[r][c] === EMPTY && gridToApply[r][c] !== "#") {
        puzzle.letters[r][c] = gridToApply[r][c];
      }
    }
  }
  renderGrid();
  refreshSlotsAndStats();
}

function applyWordToSlot(slot, word) {
  snapshotForUndo();
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
    scheduleSave();
  });
  downSel.addEventListener("change", () => {
    dictSelections.down.path = downSel.value;
    updateOptionsPanel();
    scheduleSave();
  });
  acrossMin.addEventListener("input", () => {
    dictSelections.across.minScore = parseInt(acrossMin.value || "0", 10);
    updateOptionsPanel();
    scheduleSave();
  });
  downMin.addEventListener("input", () => {
    dictSelections.down.minScore = parseInt(downMin.value || "0", 10);
    updateOptionsPanel();
    scheduleSave();
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
      scheduleSave();
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
      if (puzzle) {
        undoStack.push(puzzle);
        if (undoStack.length > MAX_UNDO) undoStack.shift();
      }
      puzzle = data.puzzle;
      slots = data.slots;
      selected = null;
      clearFillFailedHighlight();
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
  document.getElementById("btn-cancel-fill").addEventListener("click", cancelFill);

  document.getElementById("btn-undo").addEventListener("click", undo);

  document.getElementById("btn-clear").addEventListener("click", () => {
    if (!puzzle) return;
    snapshotForUndo();
    for (let r = 0; r < puzzle.height; r++) {
      for (let c = 0; c < puzzle.width; c++) {
        if (!puzzle.blocks[r][c]) puzzle.letters[r][c] = EMPTY;
      }
    }
    renderGrid();
    refreshSlotsAndStats();
    setStatus("Cleared letters (grid shape and clues kept)", "ok");
  });
}

let filling = false;

function setCancelButtonVisible(visible) {
  document.getElementById("btn-cancel-fill").hidden = !visible;
}

// Shared by the Fill button and the Cmd+F / Ctrl+F shortcut. Streams
// newline-delimited JSON from POST /api/fill (see app.py's docstring on
// that endpoint) instead of awaiting one parsed response: a plain
// await-the-whole-response call can't show live progress at all, since
// nothing arrives until the request finishes.
async function runFill() {
  if (!puzzle || filling) return;
  if (!dictSelections.across.path || !dictSelections.down.path) {
    setStatus("Select across/down dictionaries first (Dictionaries tab)", "error");
    return;
  }
  filling = true;
  clearFillFailedHighlight();
  renderGrid();
  setFillSpinner(true);
  setCancelButtonVisible(true);
  stopAllVerification(); // free up the CPU/process a background verify check is using for the real Fill

  const startedAt = Date.now();
  let lastNodes = 0;
  const elapsed = () => ((Date.now() - startedAt) / 1000).toFixed(1);
  const showProgress = () => setStatus(`Filling… ${lastNodes.toLocaleString()} nodes explored (${elapsed()}s)`);
  showProgress();
  // Keeps the elapsed-time portion visibly live even during a gap between
  // progress events (~150ms apart at the solver end, but network/JS
  // scheduling can widen that) -- the node count itself only ever updates
  // from an actual progress event, never guessed at here.
  const tickInterval = setInterval(showProgress, 200);

  const beforeFill = JSON.parse(JSON.stringify(puzzle));
  try {
    const resp = await fetch("/api/fill", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        puzzle,
        across_dict_path: dictSelections.across.path,
        across_min_score: dictSelections.across.minScore,
        down_dict_path: dictSelections.down.path,
        down_min_score: dictSelections.down.minScore,
        threads: 0,
      }),
    });
    if (!resp.ok) {
      let detail = resp.statusText;
      try {
        detail = (await resp.json()).detail || detail;
      } catch (_) {}
      throw new Error(detail);
    }

    const reader = resp.body.getReader();
    const decoder = new TextDecoder();
    let buffer = "";
    let finalEvent = null;

    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      let newlineIndex;
      while ((newlineIndex = buffer.indexOf("\n")) !== -1) {
        const line = buffer.slice(0, newlineIndex).trim();
        buffer = buffer.slice(newlineIndex + 1);
        if (!line) continue;
        let event;
        try {
          event = JSON.parse(line);
        } catch (_) {
          continue; // tolerate a stray malformed line rather than aborting the whole stream
        }
        if (event.type === "progress") {
          lastNodes = event.nodes;
          showProgress();
        } else {
          finalEvent = event;
        }
      }
    }

    if (!finalEvent) {
      throw new Error("connection closed before a result arrived");
    } else if (finalEvent.type === "done") {
      const st = finalEvent.stats;
      if (finalEvent.solved) {
        undoStack.push(beforeFill);
        if (undoStack.length > MAX_UNDO) undoStack.shift();
        puzzle = finalEvent.puzzle;
        await refreshSlotsAndStats();
        renderGrid();
        setStatus(`Solved in ${st.time_seconds.toFixed(2)}s (${st.nodes} nodes, ${st.restarts} restarts)`, "ok");
      } else {
        setStatus(`No solution found (${st.time_seconds.toFixed(2)}s) — diagnosing…`, "error");
        await diagnoseFillFailure();
        renderGrid();
        setStatus(
          fillFailedCells.size
            ? `No solution found (${st.time_seconds.toFixed(2)}s) — cells in red have no dictionary candidates given their current letters`
            : `No solution found (${st.time_seconds.toFixed(2)}s) — the conflict spans the whole grid, not one isolated slot`,
          "error"
        );
      }
    } else if (finalEvent.type === "cancelled") {
      setStatus(`Cancelled after ${lastNodes.toLocaleString()} nodes (${elapsed()}s) — grid unchanged`, "error");
    } else if (finalEvent.type === "error") {
      setStatus(`Fill failed: ${finalEvent.message}`, "error");
    }
  } catch (err) {
    setStatus(`Fill failed: ${err.message}`, "error");
  } finally {
    clearInterval(tickInterval);
    setFillSpinner(false);
    setCancelButtonVisible(false);
    filling = false;
  }
}

async function cancelFill() {
  if (!filling) return;
  try {
    await api("/api/fill/cancel", { method: "POST" });
  } catch (err) {
    setStatus(`Cancel failed: ${err.message}`, "error");
  }
}

// After a failed Fill, flags every open cell belonging to a slot that has
// zero dictionary candidates for its *current* fixed letters -- a sound
// (if partial) diagnosis: such a slot is provably part of why no solution
// exists. This can't localize a failure caused purely by how open slots
// interlock with each other (a blank grid that's still unsatisfiable has
// no single slot with zero candidates in isolation); the solver itself
// doesn't expose a minimal-conflict-set analysis to point at that case
// more precisely, so nothing is highlighted for it.
async function diagnoseFillFailure() {
  fillFailedCells = new Set();
  const checks = slots.map(async (s) => {
    const sel = s.direction === "across" ? dictSelections.across : dictSelections.down;
    if (!sel.path) return;
    try {
      const data = await apiJson("/api/options", {
        pattern: s.pattern,
        dict_path: sel.path,
        min_score: sel.minScore,
        limit: 1,
      });
      if (data.candidates.length === 0) {
        for (const [r, c] of s.cells) fillFailedCells.add(`${r},${c}`);
      }
    } catch (_) {
      // A dictionary lookup failing here shouldn't block reporting the
      // rest of the diagnosis -- just skip this slot.
    }
  });
  await Promise.all(checks);
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

function wireOptionsSort() {
  document.getElementById("options-sort-select").addEventListener("change", (e) => {
    optionsSortMode = e.target.value;
    if (lastRenderedSlot) renderOptionsList(lastRenderedSlot, lastRenderedCandidates);
    scheduleSave();
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

// Pushes dictSelections/symmetryMode/americanStyle/optionsSortMode into
// their DOM controls -- needed after restoring saved state, since
// loadDictionaries() (called first, so the <option> lists exist at all)
// already set its own defaults into those same elements.
function syncControlsToState() {
  document.getElementById("across-dict-select").value = dictSelections.across.path;
  document.getElementById("across-min-score").value = dictSelections.across.minScore;
  document.getElementById("down-dict-select").value = dictSelections.down.path;
  document.getElementById("down-min-score").value = dictSelections.down.minScore;
  document.getElementById("chk-american-style").checked = americanStyle;
  document.getElementById("symmetry-select").value = symmetryMode;
  document.getElementById("options-sort-select").value = optionsSortMode;
}

async function main() {
  wireToolbar();
  wireTabs();
  wireDictTab();
  wireInfoTab();
  wireStyleControls();
  wireOptionsSort();
  await loadDictionaries();

  const saved = loadSavedState();
  if (saved) {
    puzzle = saved.puzzle;
    if (saved.symmetryMode) symmetryMode = saved.symmetryMode;
    if (typeof saved.americanStyle === "boolean") americanStyle = saved.americanStyle;
    if (saved.optionsSortMode) optionsSortMode = saved.optionsSortMode;
    // Only restore a dictionary selection if that exact file still exists
    // -- it may have been deleted or renamed since the save, in which case
    // loadDictionaries()'s own default (already applied above) stands.
    const knownPaths = new Set(dictionaries.map((d) => d.path));
    if (saved.dictSelections?.across?.path && knownPaths.has(saved.dictSelections.across.path)) {
      dictSelections.across = { ...saved.dictSelections.across };
    }
    if (saved.dictSelections?.down?.path && knownPaths.has(saved.dictSelections.down.path)) {
      dictSelections.down = { ...saved.dictSelections.down };
    }
    syncControlsToState();
    selected = null;
    renderAll();
    setStatus("Restored your previous grid", "ok");
  } else {
    await newPuzzle(15, 15);
  }
}

// Best-effort cleanup for whatever a normal fetch() can't reach: if a
// verify-check is still running when the tab closes/navigates away, an
// ordinary fetch() call here would very likely get cut off mid-flight by
// the page unloading before it completes. sendBeacon is built for exactly
// this -- a fire-and-forget POST the browser keeps alive across
// navigation. Not the only defense against an orphaned verify subprocess
// (see solve_stream's timeout_seconds for the backstop that covers this
// even when a beacon doesn't fire, e.g. the process being killed outright
// rather than the tab closing normally), but it means the common case
// (closing the tab) cleans up immediately instead of waiting out that
// timeout.
//
// Also bumps verifyBatchToken (not just the subprocess kill) for the same
// reason stopAllVerification pairs the two everywhere else: pagehide can
// fire on tab backgrounding, not only an actual close (bfcache, switching
// tabs in some browsers), so this script can still be running afterward
// -- without the token bump, a live batch loop would just treat the
// killed subprocess as one inconclusive result and immediately spawn a
// replacement for the next candidate, undoing the cleanup this is for.
window.addEventListener("pagehide", () => {
  verifyBatchToken++;
  navigator.sendBeacon("/api/options/verify/cancel-all");
});

main().catch((err) => setStatus(err.message, "error"));
