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
// lengthOverrides: {length (number) -> minScore}, for word lengths that
// need a different threshold than minScore's default -- see
// effectiveMinScore below, the one place that resolves the two together.
let dictSelections = {
  across: { path: "", minScore: 0, lengthOverrides: {} },
  down: { path: "", minScore: 0, lengthOverrides: {} },
};

// The min score that actually applies to a word of `length` in `sel`
// (one of dictSelections.across/down): its length-specific override if
// one's set, else sel's plain default. The one place this resolution
// happens, so every caller -- the Options panel, verify checks, Fill --
// agrees with the C++ engine's own MinScoreByLength::For (dictionary.hpp).
function effectiveMinScore(sel, length) {
  const override = sel.lengthOverrides[length];
  return override === undefined ? sel.minScore : override;
}
// When true (the default), editing across's min score or a length
// override also writes the same value into down's, and vice versa -- so
// the common case (one grid, one dictionary "quality bar") doesn't
// silently leave a slot's own direction unfilterable at a threshold the
// user only meant to set once. Explicitly opting into "separate" is what
// unlocks genuinely independent across/down thresholds again. See
// setLinkedMinScore/setLinkedOverride/deleteLinkedOverride and
// renderLinkedOverrides.
let separateMinScores = false;
let currentSaveName = null; // last name Save/Load used -- so a later Save reuses it instead of prompting fresh
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
// updateOptionsPanel), cached by (slot, pattern, dictionary) key so
// switching away from a slot and back reuses what's already known instead
// of re-solving from scratch -- key -> Map(word -> {feasible, grid}).
// verifiedCompleteCount tracks, per key, how many of that slot's
// top-scored candidates have actually been checked all the way through
// (not just started -- a batch interrupted by switching slots again
// leaves the count at wherever it stopped, so a later revisit resumes
// instead of silently staying half-verified forever); see
// startVerificationBatch and verifyKeyFor. It grows past VERIFY_BATCH_SIZE
// as the user clicks "Show more" (see extendVerificationIfNeeded), up to
// VERIFY_MAX total per slot -- each check is a real solve, so verifying
// every candidate a large dictionary could return is not a cost worth
// paying just because the list *could* be paginated that far.
let verificationCache = new Map();
let verifiedCompleteCount = new Map();
let verifyBatchToken = 0; // incremented per batch; a stale batch stops issuing further checks
const VERIFY_BATCH_SIZE = 10; // how many additional candidates one batch step verifies
const VERIFY_MAX = 40; // hard cap on total verified candidates per slot, regardless of pagination
const VERIFY_THREADS = 2; // kept modest since these run one at a time, in the background

// Options list pagination: /api/options can return far more candidates
// than are worth verifying or displaying all at once (a lightly-
// constrained pattern can match hundreds of dictionary words), so the
// full fetched batch is cached and only OPTIONS_PAGE_SIZE are shown at a
// time, with a "Show more" control to reveal further pages -- see
// visibleLimit, reset per slot the same way verification is.
const OPTIONS_FETCH_LIMIT = 300; // how many candidates /api/options is asked for, once per slot
const OPTIONS_PAGE_SIZE = 25; // how many are actually shown per "page"
let visibleLimit = OPTIONS_PAGE_SIZE;
let visibleLimitKey = null;

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
      JSON.stringify({
        puzzle,
        dictSelections,
        symmetryMode,
        americanStyle,
        optionsSortMode,
        separateMinScores,
        currentSaveName,
      })
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
  updateAcceptFillButton();
}

function setPreview(slotId, word, grid) {
  previewSlotId = slotId;
  previewWord = word;
  previewGrid = grid;
  updateAcceptFillButton();
}

// The deferred single-click action from renderOptionsList's click handler
// (see its comment for why it's deferred at all), if one hasn't fired yet.
// Module-level, not scoped inside renderOptionsList, specifically so
// updateOptionsPanel's selection-changed check below can cancel it: that
// action was captured against whatever slot was selected at click time,
// and if the user has since moved on to a different slot before the
// DOUBLE_CLICK_MS window elapsed, letting it fire late would silently
// preview or place a word into a slot the user isn't even looking at
// anymore.
let pendingOptionClick = null;
let optionsPanelSlotId = null; // which slot updateOptionsPanel last ran for -- see its own comment

function cancelPendingOptionClick() {
  if (pendingOptionClick) {
    clearTimeout(pendingOptionClick);
    pendingOptionClick = null;
  }
}

// Shows/hides the "Accept full sample fill" button in lockstep with
// whether a preview is currently active -- the button is just an explicit,
// discoverable way to trigger the same commitPreview() a second click on
// the previewed option already does.
function updateAcceptFillButton() {
  const btn = document.getElementById("btn-accept-fill");
  if (btn) btn.hidden = previewGrid === null;
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

// A New or Import swaps in a whole different puzzle -- cached verification
// results are keyed by slot id + pattern, which have no relationship to
// the previous puzzle's slots, so hanging onto them would just be dead
// memory (harmless, but unbounded across many New/Import calls in one
// session) rather than anything that could still apply.
function clearVerificationCache() {
  verificationCache = new Map();
  verifiedCompleteCount = new Map();
  visibleLimit = OPTIONS_PAGE_SIZE;
  visibleLimitKey = null;
}

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
  clearVerificationCache();
  renderAll();
}

async function refreshSlotsAndStats() {
  // Typing fast (or toggling several blocks quickly) can have more than
  // one of these in flight; only the most recently *issued* one's
  // response should ever get applied, regardless of which one's response
  // happens to land first over the network.
  const seq = ++slotsRequestSeq;
  const params = new URLSearchParams();
  if (dictSelections.across.path) params.set("across_dict_path", dictSelections.across.path);
  if (dictSelections.down.path) params.set("down_dict_path", dictSelections.down.path);
  const data = await apiJson(`/api/puzzle/slots?${params}`, puzzle);
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

// Deliberately does NOT call updateOptionsPanel() -- its one caller (the
// letter-typing keydown handler) has already fired off refreshSlotsAndStats()
// a moment earlier without awaiting it, specifically so the just-typed
// letter shows up on screen immediately rather than waiting on a network
// round trip. That refresh's own eventual updateOptionsPanel() call is the
// one that's actually correct here, since by then `slots` reflects the
// edit that was just made; calling it again from here, synchronously,
// would race it using the STILL-STALE `slots` from before that edit --
// whichever response lands first wins (see updateOptionsPanel's
// optionsRequestSeq guard, which only protects against staleness by
// *issue* order, not by which call actually had fresh data to issue with)
// -- and the stale one winning is exactly what made the Options panel
// intermittently show outdated candidates right after typing. Confirmed
// directly: this was the cause, not merely a suspected one.
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

// Like backspaceStepBack, but never touches a block -- used right after
// Backspace clears a letter, so that single press only ever does the one
// thing (clear the letter): a block sitting right behind the cursor is
// left alone, and the cursor stops in front of it instead of moving onto
// or removing it. A second Backspace, now pressed from that already-empty
// cell, falls through to the plain backspaceStepBack() call below (in the
// keydown handler's "else" branch) and removes it then -- so clearing a
// letter and removing the block behind it are always two separate
// presses, never one.
function backspaceStepBackOntoOpenCell() {
  if (!selected) return;
  const dr = direction === "down" ? -1 : 0;
  const dc = direction === "across" ? -1 : 0;
  const r = selected.row + dr;
  const c = selected.col + dc;
  if (r < 0 || r >= puzzle.height || c < 0 || c >= puzzle.width) return;
  if (puzzle.blocks[r][c]) return; // stop right in front of it -- don't move onto or remove it yet
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
  // None of the shortcuts below are meant to require a modifier -- without
  // this, a browser/OS shortcut whose base key happens to match one of
  // them (Cmd+R reload, Cmd+A select-all, Cmd+C/V copy/paste, ...) gets
  // hijacked: e.key is still just "r"/"a"/"c" with a modifier held, so the
  // letter-typing branch below would preventDefault() it and type that
  // letter into the grid instead of letting the browser handle it.
  if (e.metaKey || e.ctrlKey || e.altKey) return;

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
      // The step-back here deliberately never removes a block, even if
      // one sits right behind the cursor -- that's a second press's job
      // (the "else" branch below, once this cell is the empty one), so a
      // single Backspace never does both at once (see
      // backspaceStepBackOntoOpenCell's own comment).
      snapshotForUndo();
      setLetterAt(row, col, EMPTY);
      renderGrid();
      refreshSlotsAndStats();
      backspaceStepBackOntoOpenCell();
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
    ["Avg. word score", stats.avg_word_score ?? "—"],
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

// s.score (see /api/puzzle/slots) is absent for a slot that isn't fully
// filled or has no dictionary selected for its direction (nothing to
// show), a real number for a fully-filled word found in that dictionary,
// or explicit `null` for a fully-filled word that ISN'T in it -- shown as
// "(N/A)" rather than silently looking the same as an unfilled slot.
function clueScoreSuffix(s) {
  if (s.score === undefined) return "";
  if (s.score === null) return ` <span class="clue-score clue-score-na">(N/A)</span>`;
  return ` <span class="clue-score">(${s.score})</span>`;
}

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
        <span class="clue-num">${s.number}</span>${s.pattern}${clueScoreSuffix(s)}
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

function verifyKeyFor(slot) {
  const sel = slot.direction === "across" ? dictSelections.across : dictSelections.down;
  return `${slot.id}|${slot.pattern}|${sel.path}|${effectiveMinScore(sel, slot.length)}`;
}

function getVerifiedMap(slot) {
  const key = verifyKeyFor(slot);
  let m = verificationCache.get(key);
  if (!m) {
    m = new Map();
    verificationCache.set(key, m);
  }
  return m;
}

// A verify check that comes back feasible doesn't just prove ONE slot's
// candidate works -- its `grid` is a complete, valid solution for the
// *entire* puzzle, which means every other slot's word within that same
// grid is, by definition, also a feasible candidate for that slot (at
// least one full completion exists using it). Recording that for every
// slot at once, not just the one slot whose check happened to produce it,
// is what lets a genuinely longer/different slot already show a verified
// option the moment you select it, if its word happened to appear in a
// solution some other slot's check already found.
function recordFeasibleGridForAllSlots(grid) {
  for (const s of slots) {
    const word = s.cells.map(([r, c]) => grid[r][c]).join("");
    if (!/^[A-Z]+$/.test(word)) continue; // guard against a malformed/short grid row
    const map = getVerifiedMap(s);
    if (!map.has(word)) map.set(word, { feasible: true, grid });
  }
}

async function updateOptionsPanel() {
  const heading = document.getElementById("options-heading");
  const patternEl = document.getElementById("options-pattern");
  const listEl = document.getElementById("options-list");
  const s = currentSlot();

  // This is the single choke point every selection change already funnels
  // through (click, arrows, double-click, Space, clue-row click, ...), so
  // it's the one place that needs to know "the user moved on" -- both for
  // a preview (only meaningful for the slot it was generated for) and for
  // a still-pending deferred single-click (see renderOptionsList): that
  // click was captured against whatever slot was selected when it
  // happened, and if the selection has since moved elsewhere before its
  // DOUBLE_CLICK_MS window elapsed, letting it fire late would act on a
  // slot the user isn't even looking at anymore.
  if (s?.id !== optionsPanelSlotId) cancelPendingOptionClick();
  optionsPanelSlotId = s ? s.id : null;

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
      min_score: effectiveMinScore(sel, s.length),
      limit: OPTIONS_FETCH_LIMIT,
    });
    if (seq !== optionsRequestSeq) return; // a newer selection has since superseded this request
    if (!data.candidates.length) {
      listEl.innerHTML = '<div class="hint">No matches.</div>';
      return;
    }

    const key = verifyKeyFor(s);
    if (key !== visibleLimitKey) {
      visibleLimitKey = key;
      visibleLimit = OPTIONS_PAGE_SIZE;
    }

    lastRenderedSlot = s;
    lastRenderedCandidates = data.candidates;
    extendVerificationIfNeeded(s, data.candidates);
    renderOptionsList(s, data.candidates);
  } catch (err) {
    if (seq !== optionsRequestSeq) return;
    listEl.innerHTML = `<div class="hint">${err.message}</div>`;
  }
}

function sortCandidates(candidates, verifiedMap) {
  const arr = [...candidates];
  const verified = (c) => verifiedMap.get(c.word)?.feasible === true;
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

// Renders the candidate list against whatever this slot's cached
// verification results currently know (see getVerifiedMap): a candidate
// confirmed feasible (a full grid completion actually exists with it in
// this slot) is bolded; one confirmed infeasible is dropped from the list
// entirely -- actively, the moment its check lands, not just left
// unbolded; anything not yet checked (or checked and inconclusive -- see
// verify_option's "feasible: null" error case) stays plain. Called both
// right after fetching candidates and again, incrementally, as each
// background verification result comes in -- so a row re-sorts to the top
// (or disappears) the moment its check lands under the default "smart"
// order, not only when the list is first built.
//
// Only the first `visibleLimit` of the (filtered, sorted) list are
// actually shown -- a "Show more" control reveals further pages, since
// /api/options can return up to OPTIONS_FETCH_LIMIT matches for a loosely
// constrained pattern and rendering (or verifying) all of them at once
// isn't worth the cost for a list this size.
function renderOptionsList(slot, candidates) {
  const listEl = document.getElementById("options-list");
  const verifiedMap = getVerifiedMap(slot);
  const visible = sortCandidates(
    candidates.filter((c) => verifiedMap.get(c.word)?.feasible !== false),
    verifiedMap
  );
  if (!visible.length) {
    listEl.innerHTML = '<div class="hint">No matches.</div>';
    return;
  }
  const shown = visible.slice(0, visibleLimit);
  const hiddenCount = visible.length - shown.length;

  let html = shown
    .map((c) => {
      const verified = verifiedMap.get(c.word)?.feasible === true;
      return `<div class="option-row${verified ? " verified" : ""}" data-word="${c.word}"><span class="word">${c.word}</span><span class="score">${c.score}</span></div>`;
    })
    .join("");
  if (hiddenCount > 0) {
    html += `<button type="button" id="options-show-more" class="show-more-btn">Show ${Math.min(OPTIONS_PAGE_SIZE, hiddenCount)} more (${hiddenCount} left)</button>`;
  }
  listEl.innerHTML = html;

  // A real double-click fires click, click, THEN dblclick -- so without
  // this, a verified (green) option's own click handler would already run
  // twice (previewing, then committing the WHOLE grid via commitPreview)
  // before dblclick ever got a chance to "just add the word" instead.
  // Deferring the single-click action (via the module-level
  // pendingOptionClick, so it can also be cancelled from
  // updateOptionsPanel if the user navigates away before it fires -- see
  // cancelPendingOptionClick) and cancelling it if a dblclick follows
  // within the window is the standard way to disambiguate the two; a
  // genuine single click just runs its action DOUBLE_CLICK_MS later than
  // before, the accepted tradeoff for making double-click mean something
  // clearly different.
  listEl.querySelectorAll(".option-row").forEach((row) => {
    row.addEventListener("click", () => {
      cancelPendingOptionClick();
      const word = row.getAttribute("data-word");
      pendingOptionClick = setTimeout(() => {
        pendingOptionClick = null;
        onOptionClick(slot, word);
      }, DOUBLE_CLICK_MS);
    });
    // Double-click always just places that one word into this slot,
    // regardless of verified status -- a direct shortcut past the
    // preview-then-commit dance single-click uses for a verified (green)
    // option, for whenever the whole-grid sample fill that preview offers
    // isn't what's wanted.
    row.addEventListener("dblclick", (e) => {
      e.preventDefault();
      cancelPendingOptionClick();
      applyWordToSlot(slot, row.getAttribute("data-word"));
    });
  });
  const moreBtn = document.getElementById("options-show-more");
  if (moreBtn) {
    moreBtn.addEventListener("click", () => {
      visibleLimit += OPTIONS_PAGE_SIZE;
      extendVerificationIfNeeded(slot, candidates);
      renderOptionsList(slot, candidates);
    });
  }
}

// Extends background verification to cover up to `visibleLimit`
// candidates (capped at VERIFY_MAX total) whenever more of the list
// becomes visible -- called on every render and again each time "Show
// more" is clicked, so newly-revealed candidates actually get checked
// instead of staying permanently unverified just because they weren't
// among the first page.
function extendVerificationIfNeeded(slot, candidates) {
  const key = verifyKeyFor(slot);
  const target = Math.min(visibleLimit, VERIFY_MAX, candidates.length);
  if ((verifiedCompleteCount.get(key) || 0) < target) {
    startVerificationBatch(slot, candidates, target);
  }
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
// `target` candidates actually leads to a complete grid (not just whether
// it matches the pattern -- see /api/options/verify). Sequential and
// capped deliberately: each check is a full solve, so checking every
// returned candidate eagerly and/or in parallel would make selecting a
// slot expensive instead of the fast, cheap thing it already is via
// slot_options' plain pattern match. `target` grows over the session as
// more of the list is paged into view (see extendVerificationIfNeeded);
// candidates already in this slot's cache -- from its own earlier partial
// runs, or shared from another slot's solved grid -- are skipped, so
// re-running with a bigger target only does the work for the new tail.
async function startVerificationBatch(slot, allCandidates, target) {
  const token = ++verifyBatchToken;
  await cancelAllVerifyChecks();
  if (token !== verifyBatchToken) return; // superseded again while the cancel was in flight
  const verifiedMap = getVerifiedMap(slot);
  const toCheck = allCandidates.slice(0, target);
  for (const c of toCheck) {
    if (token !== verifyBatchToken) return; // superseded by a newer slot/pattern/dictionary -- key stays incomplete, so a revisit resumes
    if (verifiedMap.has(c.word)) continue; // already known -- this slot's own prior check, or shared from another slot's solve (see recordFeasibleGridForAllSlots)
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
        across_min_overrides: dictSelections.across.lengthOverrides,
        down_min_overrides: dictSelections.down.lengthOverrides,
        threads: VERIFY_THREADS,
      });
    } catch (_) {
      continue; // leave this one unchecked rather than aborting the whole batch
    }
    if (token !== verifyBatchToken) return;
    if (result.feasible === true) {
      verifiedMap.set(c.word, { feasible: true, grid: result.grid });
      // This word's grid is a complete solution for the whole puzzle, not
      // just this slot -- every other slot's word within it is equally
      // feasible, so record it there too instead of letting a slot you
      // haven't checked yet show no verified options despite one clearly
      // existing (visible in this word's own preview).
      recordFeasibleGridForAllSlots(result.grid);
    } else if (result.feasible === false) {
      verifiedMap.set(c.word, { feasible: false, grid: null });
    } // feasible === null (a verify-side error, not a real infeasibility finding) -- leave unset

    // Re-render whatever's actually on screen right now, which may be a
    // DIFFERENT slot than the one this batch is for -- recordFeasibleGridForAllSlots
    // above can have just updated it via cross-slot sharing.
    if (lastRenderedSlot) renderOptionsList(lastRenderedSlot, lastRenderedCandidates);
  }
  if (token === verifyBatchToken) verifiedCompleteCount.set(verifyKeyFor(slot), target);
}

// A confirmed-feasible candidate previews its full solved grid on click
// (dimmed, into cells the user hasn't actually filled -- see renderGrid);
// clicking that same candidate again commits it (same action as the
// "Accept full sample fill" button, see updateAcceptFillButton). An
// unverified candidate has no precomputed completion to preview, so it
// keeps the original behavior: click commits just that one slot's word
// immediately -- same as what a double-click now does regardless of
// verified status (see the dblclick listener in renderOptionsList).
// A previewGrid-shaped array (row strings, '#' meaning "nothing to
// preview here", exactly like a real solved grid's block cells) holding
// only `word`'s own letters in `slot`'s cells. Lets a candidate that
// isn't verified yet (or might never be -- it could turn out infeasible)
// reuse the exact same preview rendering (renderGrid) and commit
// (commitPreview) machinery a verified option's real, whole-puzzle solved
// grid uses, without needing one of those to exist.
function singleSlotPreviewGrid(slot, word) {
  const rows = [];
  for (let r = 0; r < puzzle.height; r++) rows.push(new Array(puzzle.width).fill("#"));
  slot.cells.forEach(([r, c], i) => {
    rows[r][c] = word[i];
  });
  return rows.map((row) => row.join(""));
}

// A confirmed-feasible (green) candidate previews its full solved grid;
// a candidate that isn't verified yet -- still pending, or never checked
// at all -- previews just its own slot's word instead (see
// singleSlotPreviewGrid), dimmed the same way, but never written into
// the real grid on this first click: clicking blind on an unconfirmed
// guess shouldn't commit anything, only show what it would look like.
// Either way, clicking the SAME candidate again commits it -- upgrading
// to the real verified grid first if the background batch confirmed it
// feasible in the meantime, so the commit uses the better, whole-puzzle
// completion rather than just the one word. Double-click (see
// renderOptionsList) is the direct, single-step alternative to this
// two-click dance, regardless of verified status.
function onOptionClick(slot, word) {
  const verified = getVerifiedMap(slot).get(word);
  if (previewSlotId === slot.id && previewWord === word) {
    if (verified?.feasible && verified.grid) {
      setPreview(slot.id, word, verified.grid);
    }
    commitPreview();
    return;
  }
  if (verified?.feasible && verified.grid) {
    setPreview(slot.id, word, verified.grid);
  } else {
    setPreview(slot.id, word, singleSlotPreviewGrid(slot, word));
  }
  renderGrid();
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

// Rebuilds one length-override section's rows and wires each one's
// inputs. `mode` is "across" or "down" (in separate mode, editing that
// direction's own dictSelections[mode].lengthOverrides) or "linked" (the
// default: displays across's overrides as canonical and writes any edit
// into BOTH across's and down's, via setLinkedOverride/deleteLinkedOverride
// below). Called after any change to the underlying object (add/remove/
// edit a row) since there's no cheap way to patch just one row when a
// length key itself changes -- the whole small list is rebuilt instead,
// same tradeoff renderClues makes for the same reason.
function renderLengthOverrides(mode) {
  const container = document.getElementById(mode === "linked" ? "linked-overrides" : `${mode}-overrides`);
  // Linked mode keeps across's and down's overrides identical (see
  // setLinkedOverride), so across's object is the display source for both.
  const overrides = mode === "down" ? dictSelections.down.lengthOverrides : dictSelections.across.lengthOverrides;
  const lengths = Object.keys(overrides)
    .map(Number)
    .sort((a, b) => a - b);

  const setOverride = (length, score) => {
    if (mode === "linked") {
      dictSelections.across.lengthOverrides[length] = score;
      dictSelections.down.lengthOverrides[length] = score;
    } else {
      dictSelections[mode].lengthOverrides[length] = score;
    }
  };
  const deleteOverride = (length) => {
    if (mode === "linked") {
      delete dictSelections.across.lengthOverrides[length];
      delete dictSelections.down.lengthOverrides[length];
    } else {
      delete dictSelections[mode].lengthOverrides[length];
    }
  };

  container.innerHTML = lengths
    .map(
      (len) => `
    <div class="length-override-row" data-length="${len}">
      <input type="number" class="override-length" value="${len}" min="1" max="50" title="Word length" />
      letters need min score
      <input type="number" class="override-score" value="${overrides[len]}" min="0" max="100" title="Min score for this length" />
      <button type="button" class="remove-override" title="Remove this override">×</button>
    </div>`
    )
    .join("");

  container.querySelectorAll(".length-override-row").forEach((row) => {
    const oldLength = Number(row.getAttribute("data-length"));
    const lengthInput = row.querySelector(".override-length");
    const scoreInput = row.querySelector(".override-score");

    lengthInput.addEventListener("change", () => {
      const newLength = parseInt(lengthInput.value || "0", 10);
      const score = overrides[oldLength];
      deleteOverride(oldLength);
      if (newLength > 0) setOverride(newLength, score); // last-write-wins if it collides with another row's length
      renderLengthOverrides(mode);
      invalidatePreview();
      updateOptionsPanel();
      scheduleSave();
    });
    scoreInput.addEventListener("input", () => {
      setOverride(oldLength, parseInt(scoreInput.value || "0", 10));
      invalidatePreview();
      updateOptionsPanel();
      scheduleSave();
    });
    row.querySelector(".remove-override").addEventListener("click", () => {
      deleteOverride(oldLength);
      renderLengthOverrides(mode);
      invalidatePreview();
      updateOptionsPanel();
      scheduleSave();
    });
  });
}

// A verify-confirmed preview (see onOptionClick/commitPreview) is only
// trustworthy for the exact dictionary/min-score it was verified against
// -- clicking the same word text a second time re-applies that *cached*
// grid rather than re-verifying, so any change here has to invalidate it,
// even though the selected slot itself hasn't changed (updateOptionsPanel
// only clears a preview on its own when the slot changes).
function invalidatePreview() {
  if (previewSlotId === null) return;
  clearPreview();
  renderGrid();
}

// Reflects `separateMinScores` into which of the two min-score sections
// is visible, and the toggle checkbox itself (needed on restore from
// saved state, where the checkbox otherwise never learns the value).
function updateMinScoreSectionVisibility() {
  document.getElementById("linked-min-score-section").hidden = separateMinScores;
  document.getElementById("separate-min-score-section").hidden = !separateMinScores;
  document.getElementById("separate-min-scores").checked = separateMinScores;
}

function syncMinScoreInputs() {
  document.getElementById("across-min-score").value = dictSelections.across.minScore;
  document.getElementById("down-min-score").value = dictSelections.down.minScore;
  document.getElementById("linked-min-score").value = dictSelections.across.minScore;
}

function wireDictTab() {
  const acrossSel = document.getElementById("across-dict-select");
  const downSel = document.getElementById("down-dict-select");
  const acrossMin = document.getElementById("across-min-score");
  const downMin = document.getElementById("down-min-score");
  const linkedMin = document.getElementById("linked-min-score");

  acrossSel.addEventListener("change", () => {
    dictSelections.across.path = acrossSel.value;
    invalidatePreview();
    updateOptionsPanel();
    scheduleSave();
  });
  downSel.addEventListener("change", () => {
    dictSelections.down.path = downSel.value;
    invalidatePreview();
    updateOptionsPanel();
    scheduleSave();
  });
  acrossMin.addEventListener("input", () => {
    dictSelections.across.minScore = parseInt(acrossMin.value || "0", 10);
    invalidatePreview();
    updateOptionsPanel();
    scheduleSave();
  });
  downMin.addEventListener("input", () => {
    dictSelections.down.minScore = parseInt(downMin.value || "0", 10);
    invalidatePreview();
    updateOptionsPanel();
    scheduleSave();
  });
  linkedMin.addEventListener("input", () => {
    const v = parseInt(linkedMin.value || "0", 10);
    dictSelections.across.minScore = v;
    dictSelections.down.minScore = v;
    invalidatePreview();
    updateOptionsPanel();
    scheduleSave();
  });

  document.getElementById("separate-min-scores").addEventListener("change", (e) => {
    separateMinScores = e.target.checked;
    if (!separateMinScores) {
      // Switching back to linked -- across's current values become the
      // single source of truth for both directions again, so what's
      // displayed (across's) actually matches what's in effect for down
      // too, rather than down silently keeping whatever it had before.
      dictSelections.down.minScore = dictSelections.across.minScore;
      dictSelections.down.lengthOverrides = { ...dictSelections.across.lengthOverrides };
    }
    updateMinScoreSectionVisibility();
    syncMinScoreInputs();
    renderLengthOverrides("across");
    renderLengthOverrides("down");
    renderLengthOverrides("linked");
    invalidatePreview();
    updateOptionsPanel();
    scheduleSave();
  });

  for (const mode of ["across", "down", "linked"]) {
    document.getElementById(`${mode}-add-override`).addEventListener("click", () => {
      const overrides = mode === "down" ? dictSelections.down.lengthOverrides : dictSelections.across.lengthOverrides;
      // Starts from the shortest slot length a real crossword ever has;
      // steps up past whatever's already overridden so a repeated click
      // adds a new row instead of landing back on one that already
      // exists.
      let length = 3;
      while (overrides[length] !== undefined) length++;
      const defaultScore = mode === "down" ? dictSelections.down.minScore : dictSelections.across.minScore;
      if (mode === "linked") {
        dictSelections.across.lengthOverrides[length] = defaultScore;
        dictSelections.down.lengthOverrides[length] = defaultScore;
      } else {
        dictSelections[mode].lengthOverrides[length] = defaultScore;
      }
      renderLengthOverrides(mode);
      invalidatePreview();
      updateOptionsPanel();
      scheduleSave();
    });
  }
  updateMinScoreSectionVisibility();
  syncMinScoreInputs();
  renderLengthOverrides("across");
  renderLengthOverrides("down");
  renderLengthOverrides("linked");

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
      clearVerificationCache();
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

// ---------------------------------------------------------------------------
// Save / Load -- an in-app named save slot (see app.py's docstring on
// POST /api/puzzle/save), distinct from Import/Export: those round-trip
// through real crossword formats for other tools; this is just "remember
// this puzzle under a name I pick" so Save doesn't need a file picker
// every time. The first Save in a session prompts for a name; every Save
// after that (including in a later session, since currentSaveName is
// itself part of the autosaved state) reuses it silently.
// ---------------------------------------------------------------------------

async function saveToServer() {
  if (!puzzle) return;
  const name = prompt("Save as:", currentSaveName || puzzle.title || "My puzzle");
  if (!name) return;
  try {
    const result = await apiJson("/api/puzzle/save", { puzzle, name });
    currentSaveName = result.name;
    await refreshSavesList();
    setStatus(`Saved as "${currentSaveName}"`, "ok");
    scheduleSave();
  } catch (err) {
    setStatus(`Save failed: ${err.message}`, "error");
  }
}

async function refreshSavesList() {
  const sel = document.getElementById("load-select");
  try {
    const data = await api("/api/puzzle/saves");
    const options = data.saves.map((n) => `<option value="${escapeAttr(n)}">${escapeAttr(n)}</option>`).join("");
    sel.innerHTML = `<option value="">Load…</option>${options}`;
    sel.value = currentSaveName && data.saves.includes(currentSaveName) ? currentSaveName : "";
  } catch (_) {
    // Best-effort -- an empty/stale list here shouldn't block anything else.
  }
}

function wireSaveLoad() {
  document.getElementById("btn-save").addEventListener("click", saveToServer);
  document.getElementById("load-select").addEventListener("change", async (e) => {
    const name = e.target.value;
    if (!name) return;
    try {
      const data = await apiJson("/api/puzzle/load", { name });
      if (puzzle) {
        undoStack.push(puzzle);
        if (undoStack.length > MAX_UNDO) undoStack.shift();
      }
      currentSaveName = data.name;
      puzzle = data.puzzle;
      slots = data.slots;
      selected = null;
      clearFillFailedHighlight();
      clearVerificationCache();
      renderAll();
      setStatus(`Loaded "${data.name}"`, "ok");
      scheduleSave();
    } catch (err) {
      setStatus(`Load failed: ${err.message}`, "error");
      e.target.value = currentSaveName || "";
    }
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
  const maximize = document.getElementById("maximize-toggle").checked;

  filling = true;
  clearFillFailedHighlight();
  // A leftover preview (from clicking a verified option earlier without
  // committing or navigating away -- see setPreview/clearPreview) is only
  // ever a full, real solved grid, so it always spells out complete
  // words. Left uncleared through a Fill attempt that then fails, those
  // dimmed preview letters still render into every blank cell (see
  // renderGrid) including ones diagnoseFillFailure marks fill-failed --
  // making it look like a specific, non-dictionary "word" is the
  // diagnosed problem, when it's really just a stale, unrelated preview
  // bleeding through underneath the red highlighting.
  clearPreview();
  renderGrid();
  setFillSpinner(true);
  setCancelButtonVisible(true);
  stopAllVerification(); // free up the CPU/process a background verify check is using for the real Fill

  const startedAt = Date.now();
  let lastNodes = 0;
  let bestScore = null; // set from each "improved" event; only meaningful when maximize is on
  const elapsed = () => ((Date.now() - startedAt) / 1000).toFixed(1);
  const showProgress = () =>
    setStatus(
      bestScore === null
        ? `Filling… ${lastNodes.toLocaleString()} nodes explored (${elapsed()}s)`
        : `Maximizing… best score so far: ${bestScore.toLocaleString()} (${lastNodes.toLocaleString()} nodes, ${elapsed()}s)`
    );
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
        across_min_overrides: dictSelections.across.lengthOverrides,
        down_min_overrides: dictSelections.down.lengthOverrides,
        threads: 0,
        maximize,
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
        } else if (event.type === "improved") {
          // Anytime search: every one of these is a complete, better-
          // scoring fill than the last (see MaximizeScoreParallel's doc
          // comment) -- apply and show it immediately rather than waiting
          // for "done", which may be a long time away or may never come
          // if the user cancels first. The undo entry is pushed on the
          // *first* one, not deferred to "done"/"cancelled" below, so the
          // grid is always restorable back to its pre-Fill state no
          // matter which of those terminates the stream -- including the
          // "connection closed early" and thrown-error paths, which don't
          // have their own branch below to duplicate this in.
          if (bestScore === null) {
            undoStack.push(beforeFill);
            if (undoStack.length > MAX_UNDO) undoStack.shift();
          }
          bestScore = event.score;
          puzzle = event.puzzle;
          renderGrid(); // cheap, local; also autosaves (see renderGrid's scheduleSave call) -- the "and save it" half of this feature
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
        if (!maximize) {
          undoStack.push(beforeFill);
          if (undoStack.length > MAX_UNDO) undoStack.shift();
        } // maximize: already pushed above, on the first "improved" event
        puzzle = finalEvent.puzzle;
        await refreshSlotsAndStats();
        renderGrid();
        setStatus(
          maximize
            ? `Proven optimal — score ${st.score.toLocaleString()} (${st.time_seconds.toFixed(2)}s, ${st.nodes.toLocaleString()} nodes)`
            : `Solved in ${st.time_seconds.toFixed(2)}s (${st.nodes} nodes, ${st.restarts} restarts)`,
          "ok"
        );
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
      if (maximize && bestScore !== null) {
        await refreshSlotsAndStats();
        renderGrid();
        setStatus(
          `Cancelled — kept best fill found: score ${bestScore.toLocaleString()} (${lastNodes.toLocaleString()} nodes, ${elapsed()}s)`,
          "ok"
        );
      } else {
        setStatus(`Cancelled after ${lastNodes.toLocaleString()} nodes (${elapsed()}s) — grid unchanged`, "error");
      }
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
        min_score: effectiveMinScore(sel, s.length),
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

// Theme is intentionally its OWN, separate localStorage key
// ("xfill-theme"), not folded into SAVE_KEY's debounced puzzle-state
// blob: it has to be readable synchronously before any of that state
// loads (see index.html's inline <head> script, which already applied it
// before this ever runs) so there's no flash of the wrong theme, and it
// should keep working even with no puzzle open yet.
function wireThemeToggle() {
  const toggle = document.getElementById("theme-toggle");
  toggle.checked = document.documentElement.getAttribute("data-theme") === "light";
  toggle.addEventListener("change", () => {
    const next = toggle.checked ? "light" : "dark";
    document.documentElement.setAttribute("data-theme", next);
    try {
      localStorage.setItem("xfill-theme", next);
    } catch (_) {
      // Losing the preference across reloads beats crashing over it.
    }
  });
}

function wireOptionsSort() {
  document.getElementById("options-sort-select").addEventListener("change", (e) => {
    optionsSortMode = e.target.value;
    if (lastRenderedSlot) renderOptionsList(lastRenderedSlot, lastRenderedCandidates);
    scheduleSave();
  });
  document.getElementById("btn-accept-fill").addEventListener("click", commitPreview);
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
  document.getElementById("down-dict-select").value = dictSelections.down.path;
  updateMinScoreSectionVisibility();
  syncMinScoreInputs();
  renderLengthOverrides("across");
  renderLengthOverrides("down");
  renderLengthOverrides("linked");
  document.getElementById("chk-american-style").checked = americanStyle;
  document.getElementById("symmetry-select").value = symmetryMode;
  document.getElementById("options-sort-select").value = optionsSortMode;
}

async function main() {
  wireToolbar();
  wireSaveLoad();
  wireTabs();
  wireDictTab();
  wireInfoTab();
  wireStyleControls();
  wireThemeToggle();
  wireOptionsSort();
  await loadDictionaries();
  refreshSavesList();

  const saved = loadSavedState();
  if (saved) {
    puzzle = saved.puzzle;
    if (saved.symmetryMode) symmetryMode = saved.symmetryMode;
    if (typeof saved.americanStyle === "boolean") americanStyle = saved.americanStyle;
    if (saved.optionsSortMode) optionsSortMode = saved.optionsSortMode;
    if (typeof saved.separateMinScores === "boolean") separateMinScores = saved.separateMinScores;
    if (saved.currentSaveName) currentSaveName = saved.currentSaveName;
    // Only restore a dictionary selection if that exact file still exists
    // -- it may have been deleted or renamed since the save, in which case
    // loadDictionaries()'s own default (already applied above) stands.
    const knownPaths = new Set(dictionaries.map((d) => d.path));
    // lengthOverrides defaults to {} explicitly -- a state saved before
    // this feature existed won't have the key at all, and every reader
    // (effectiveMinScore, renderLengthOverrides) assumes it's always a
    // real object, never missing.
    if (saved.dictSelections?.across?.path && knownPaths.has(saved.dictSelections.across.path)) {
      dictSelections.across = {
        ...saved.dictSelections.across,
        lengthOverrides: saved.dictSelections.across.lengthOverrides || {},
      };
    }
    if (saved.dictSelections?.down?.path && knownPaths.has(saved.dictSelections.down.path)) {
      dictSelections.down = {
        ...saved.dictSelections.down,
        lengthOverrides: saved.dictSelections.down.lengthOverrides || {},
      };
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
