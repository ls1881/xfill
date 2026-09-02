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
// "r,c" keys of a successful Fill's letters that had no real alternative
// -- see main.cpp's ForcedCells/Solution::forced_slot_ids -- vs. cells
// the solver was actually free to choose among several valid words for.
// Cleared on the next edit, same lifecycle as fillFailedCells.
let forcedCells = new Set();

// Word lengths / letters currently toggled on in the Summary tab -- every
// grid cell belonging to a slot of a highlighted length, or currently
// showing a highlighted letter, gets the .stats-highlight treatment (see
// renderGrid). Clicking an already-highlighted row/letter again removes
// just that one (a real toggle, not "replace the whole selection"), so
// several lengths/letters can be highlighted together. Independent of
// each other (both apply at once) and never cleared by an edit or Fill --
// unlike fillFailedCells/forcedCells, this is a display filter the user
// set deliberately, not a stale solve result.
let highlightedLengths = new Set();
let highlightedLetters = new Set();
// Same idea, one more filter: every cell that's part of a substring (at
// least substringMinLength characters) shared between two or more
// DIFFERENT filled entries -- a constructor's editorial check for
// accidental fragment reuse across the grid. Off by default (a single
// on/off toggle, not a set -- there's only one length threshold active at
// once, unlike lengths/letters which can multi-select). See
// computeSharedSubstringCells.
let highlightSharedSubstrings = false;
let substringMinLength = 3;

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
// Which verifyKeyFor() a batch is CURRENTLY (actively, right now) running
// for, and up to what target -- distinct from verifiedCompleteCount,
// which is only ever written once a batch finishes ALL the way through.
// Without this, extendVerificationIfNeeded had no way to tell "already in
// progress" from "not started yet": updateOptionsPanel calls it after
// EVERY grid edit, anywhere, even in a totally unrelated slot -- and
// since verifiedCompleteCount stays at its stale (pre-batch) value for
// the batch's entire, possibly multi-second run, every one of those
// calls looked identical to the very first one, restarting the batch
// (via cancelAllVerifyChecks killing the in-flight subprocess) from
// scratch each time. A user typing continuously anywhere in the grid
// could keep the currently-viewed slot's verification perpetually
// interrupted before it ever finished a single candidate -- confirmed
// directly as the cause of verification appearing to load very slowly or
// not at all, not just a theoretical risk.
let activeVerificationKey = null;
let activeVerificationTarget = 0;
// Bumped every time verificationCache itself gets replaced with a fresh
// Map (invalidateVerificationCache/clearVerificationCache). A batch
// captures the epoch it started under; if the epoch has since moved on
// by the time it finishes, its verifiedMap reference (from getVerifiedMap,
// captured once up front) points at a Map that's been detached from the
// current verificationCache -- its results are invisible to anyone, and
// it must NOT mark its key "complete" (verifiedCompleteCount), which
// would wrongly tell a later, genuine check "nothing left to verify"
// for a key whose real, current results were never actually gathered.
let verificationCacheEpoch = 0;
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
// Whether previewGrid is a verified candidate's real, whole-puzzle solved
// grid, vs. an unverified candidate's own-word-only stand-in
// (singleSlotPreviewGrid) -- see updateAcceptFillButton, which only shows
// its button for the former: committing the latter wouldn't be a "full
// sample fill" at all, just one slot's word.
let previewIsVerified = false;

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
  clearForcedCells();
  clearPreview();
  invalidateVerificationCache();
}

function clearFillFailedHighlight() {
  if (fillFailedCells.size) fillFailedCells = new Set();
}

function clearForcedCells() {
  if (forcedCells.size) forcedCells = new Set();
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
  previewIsVerified = false;
  updateAcceptFillButton();
}

function setPreview(slotId, word, grid, isVerified) {
  previewSlotId = slotId;
  previewWord = word;
  previewGrid = grid;
  previewIsVerified = isVerified;
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
// whether a *verified* preview is currently active -- the button is just
// an explicit, discoverable way to trigger the same commitPreview() a
// second click on the previewed option already does, but only makes sense
// for a verified candidate's real, whole-puzzle preview: an unverified
// one's preview is just that single word (singleSlotPreviewGrid), not a
// "full sample fill" the button's own label promises.
function updateAcceptFillButton() {
  const btn = document.getElementById("btn-accept-fill");
  if (btn) btn.hidden = previewGrid === null || !previewIsVerified;
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
  clearForcedCells();
  // This is a real grid-state change (reverting to an earlier one) that
  // doesn't go through snapshotForUndo() -- doing so would push a new
  // undo entry for the undo itself -- so it needs its own call to the
  // same cache invalidation for the same reason: a cached verification
  // result reflects the grid state at the time it was checked, and undo
  // just changed that state out from under it.
  invalidateVerificationCache();
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
  verificationCacheEpoch++;
  activeVerificationKey = null;
  activeVerificationTarget = 0;
  visibleLimit = OPTIONS_PAGE_SIZE;
  visibleLimitKey = null;
}

// A "feasible" (or "infeasible") result is a property of the WHOLE grid --
// it comes from a full CSP solve, not just the checked slot's own pattern
// -- so an edit ANYWHERE, even in a totally different, non-crossing slot,
// can make a previously-feasible candidate infeasible (or vice versa)
// without verifyKeyFor()'s cache key for that OTHER slot changing at all
// (that key only tracks THIS slot's own pattern/dictionary/min-score).
// Left uninvalidated, the Options panel could keep showing a candidate as
// verified (green) after it had genuinely stopped working -- confirmed
// directly as a real, reported bug, not just a theoretical risk. Called
// from snapshotForUndo(), the universal "about to mutate the puzzle"
// choke point every real edit already funnels through, so this doesn't
// need its own call site at each mutation function. Deliberately doesn't
// touch visibleLimit/visibleLimitKey (unlike clearVerificationCache) --
// there's no reason a single keystroke should reset which page of an
// unrelated slot's options list the user was scrolled to.
function invalidateVerificationCache() {
  verificationCache = new Map();
  verifiedCompleteCount = new Map();
  verificationCacheEpoch++;
  // Also releases any batch's "actively running" claim -- its
  // verifiedMap reference (captured before this call) is about to be
  // detached from the fresh verificationCache above, so it can no
  // longer be trusted to actually finish gathering real, visible
  // results for that key. Clearing the claim here lets a genuine new
  // batch start on the very next extendVerificationIfNeeded call (e.g.
  // from this same edit's own refreshSlotsAndStats -> updateOptionsPanel)
  // instead of that call wrongly believing one is already in progress.
  activeVerificationKey = null;
  activeVerificationTarget = 0;
}

// Puzzle no longer corresponds to any named save slot -- New and Import
// both replace it wholesale with something that was never Saved under a
// name (unlike Load, which sets currentSaveName to the one it just
// loaded). Without this, the Load dropdown keeps showing whatever save
// was active before, and re-selecting that SAME option afterward is a
// native <select> no-op (a "change" event only fires when the value
// actually changes) -- so importing over an in-progress named save left
// no way to click straight back to it, only via some other save first.
function clearCurrentSave() {
  currentSaveName = null;
  const sel = document.getElementById("load-select");
  if (sel) sel.value = "";
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
  clearCurrentSave();
  clearFillFailedHighlight();
  clearForcedCells();
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
  // `slots` hasn't loaded yet (e.g. the very first render on page load,
  // before refreshSlotsAndStats()'s response lands -- renderAll() renders
  // once immediately with whatever `slots` currently is, then again once
  // the fetch resolves). With no slot data at all, every open cell would
  // otherwise look "uncovered" and flash the whole grid red for a moment;
  // there's nothing real to judge coverage against yet, so skip instead.
  if (!slots.length && puzzle.blocks.some((row) => row.some((b) => !b))) return issues;
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

// Every cell belonging to a slot whose length is toggled on in the Summary
// tab's "Word lengths" table, plus every cell whose current letter is
// toggled on in "Letter counts" -- see highlightedLengths/highlightedLetters
// and their click handlers in renderSummary/renderLetterGrid.
function computeStatsHighlightCells() {
  const cells = new Set();
  if (highlightedLengths.size) {
    for (const s of slots) {
      if (!highlightedLengths.has(s.length)) continue;
      for (const [r, c] of s.cells) cells.add(`${r},${c}`);
    }
  }
  if (highlightedLetters.size) {
    for (let r = 0; r < puzzle.height; r++) {
      for (let c = 0; c < puzzle.width; c++) {
        if (puzzle.blocks[r][c]) continue;
        const letter = puzzle.letters[r][c];
        if (!letter || letter === EMPTY) continue;
        // A rebus cell (see isRebusCell) matches if ANY of its own
        // characters is highlighted -- "STAR" lights up for a click on
        // S, T, A, or R, matching how the Summary tab's own letter counts
        // already count each of those individually (see grid_model.
        // Puzzle.stats on the backend).
        if ([...letter].some((ch) => highlightedLetters.has(ch))) cells.add(`${r},${c}`);
      }
    }
  }
  if (highlightSharedSubstrings) {
    for (const key of computeSharedSubstringCells(substringMinLength)) cells.add(key);
  }
  return cells;
}

// This slot's word, one character per CELL -- a rebus cell (see
// isRebusCell) contributes only its own first character, the same
// convention used everywhere else exactly one character per cell is
// needed (e.g. the solver's crossing constraints). Keeps every substring
// position mapped to exactly one grid cell, with no ambiguity from a
// multi-character rebus square spanning more than its one cell's worth of
// "substring space." Returns null if any cell is still blank -- a
// substring straddling an unfilled cell isn't a real match yet.
function slotSolvingWord(slot) {
  const chars = slot.cells.map(([r, c]) => {
    const letter = puzzle.letters[r][c];
    return letter && letter !== EMPTY ? letter[0] : null;
  });
  return chars.includes(null) ? null : chars.join("");
}

// Every cell that's part of a substring at least `minLength` characters
// long shared between two or more DIFFERENT slots (across and down mixed
// together -- a repeated fragment is worth flagging regardless of which
// directions it appears in) -- an editorial check for accidental fragment
// reuse across the grid. Implemented via minLength-character n-grams:
// hand-verified that this still fully covers any LONGER shared run too,
// since a shared run of length minLength+k contains k+1 overlapping
// minLength-length windows, each independently shared at correspondingly
// shifted positions -- so their union covers the run's every cell.
function computeSharedSubstringCells(minLength) {
  const cells = new Set();
  if (!minLength || minLength < 1) return cells;
  const ngramOccurrences = new Map(); // n-gram string -> [{slot, start}, ...]
  for (const s of slots) {
    const word = slotSolvingWord(s);
    if (!word || word.length < minLength) continue;
    for (let i = 0; i + minLength <= word.length; i++) {
      const gram = word.slice(i, i + minLength);
      if (!ngramOccurrences.has(gram)) ngramOccurrences.set(gram, []);
      ngramOccurrences.get(gram).push({ slot: s, start: i });
    }
  }
  for (const occurrences of ngramOccurrences.values()) {
    const distinctSlotIds = new Set(occurrences.map((o) => o.slot.id));
    if (distinctSlotIds.size < 2) continue; // shared within just one word's own repeat doesn't count
    for (const { slot, start } of occurrences) {
      for (let k = 0; k < minLength; k++) {
        const [r, c] = slot.cells[start + k];
        cells.add(`${r},${c}`);
      }
    }
  }
  return cells;
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
  const statsHighlights = computeStatsHighlightCells();

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
      if (forcedCells.has(key)) cell.classList.add("forced-letter");
      if (statsHighlights.has(key)) cell.classList.add("stats-highlight");

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
          if (letter.length > 1) {
            cell.classList.add("has-rebus");
            const rebusEl = document.createElement("span");
            rebusEl.className = "rebus-text";
            rebusEl.textContent = letter;
            cell.appendChild(rebusEl);
          } else {
            cell.appendChild(document.createTextNode(letter));
          }
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
  renderRebusTab();
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
    ["Scrabble avg.", stats.scrabble_avg ?? "—"],
    ["Blocks", `${stats.block_count} (${stats.block_percent}%)`],
    ["Letters filled", stats.letter_count],
    ["Open squares", stats.open_square_count],
  ];
  general.innerHTML = rows.map(([k, v]) => `<tr><td>${k}</td><td>${v ?? ""}</td></tr>`).join("");

  const lengths = document.getElementById("stats-lengths");
  const entries = Object.entries(stats.length_breakdown || {});
  lengths.innerHTML = entries
    .map(
      ([len, count]) =>
        `<tr class="stats-row${highlightedLengths.has(Number(len)) ? " stats-row-active" : ""}" data-length="${len}"><td>${len} letters</td><td>${count}</td></tr>`
    )
    .join("");
  lengths.querySelectorAll("tr[data-length]").forEach((row) => {
    row.addEventListener("click", () => {
      const len = Number(row.getAttribute("data-length"));
      if (highlightedLengths.has(len)) highlightedLengths.delete(len);
      else highlightedLengths.add(len);
      renderSummary();
      renderGrid();
    });
  });

  renderLetterGrid();

  const substringBtn = document.getElementById("btn-highlight-substrings");
  if (substringBtn) {
    // Reuses the existing button.primary look (already how e.g. the Fill
    // button reads as "the active/primary action") rather than inventing
    // a separate visual language just for this toggle.
    substringBtn.classList.toggle("primary", highlightSharedSubstrings);
    substringBtn.textContent = highlightSharedSubstrings ? "Hide" : "Highlight";
  }
}

function renderLetterGrid() {
  const el = document.getElementById("letter-grid");
  const counts = stats.letter_counts || {};
  el.innerHTML = "";
  for (let i = 0; i < 26; i++) {
    const letter = String.fromCharCode(65 + i);
    const div = document.createElement("div");
    div.className = "letter-cell" + (highlightedLetters.has(letter) ? " letter-cell-active" : "");
    div.innerHTML = `<span class="lc">${letter}</span><span class="lv">${counts[letter] || 0}</span>`;
    div.addEventListener("click", () => {
      if (highlightedLetters.has(letter)) highlightedLetters.delete(letter);
      else highlightedLetters.add(letter);
      renderLetterGrid();
      renderGrid();
    });
    el.appendChild(div);
  }
}

// Wired once (unlike the length rows/letter cells above, which are
// rebuilt from scratch -- and re-wired -- on every renderSummary() call):
// the button and number input here are static markup, so listening more
// than once would just fire the handler redundantly per click.
function wireSubstringHighlight() {
  const minLengthInput = document.getElementById("substring-min-length");
  document.getElementById("btn-highlight-substrings").addEventListener("click", () => {
    highlightSharedSubstrings = !highlightSharedSubstrings;
    renderSummary();
    renderGrid();
  });
  minLengthInput.addEventListener("input", () => {
    const value = parseInt(minLengthInput.value, 10);
    if (!value || value < 1) return;
    substringMinLength = value;
    if (highlightSharedSubstrings) renderGrid();
  });
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

// A clue counts as written if it has any non-whitespace text -- matches
// what a solver would actually see as "there's a clue here," not just
// whether the field happens to be non-empty.
function updateCluesProgress() {
  const total = slots.length;
  const cluedCount = slots.filter((s) => (puzzle.clues[s.id] || "").trim() !== "").length;
  document.getElementById("clues-progress").textContent = `(${cluedCount} of ${total} words clued)`;
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
  updateCluesProgress();

  document.querySelectorAll("[data-slot-input]").forEach((input) => {
    input.addEventListener("input", (e) => {
      const id = e.target.getAttribute("data-slot-input");
      puzzle.clues[id] = e.target.value;
      // A cheap text update, not a full renderClues() -- rebuilding every
      // clue <input> on each keystroke would steal focus out from under
      // whichever one the user is actively typing into (see this
      // function's own top-of-function comment on restoreFocus).
      updateCluesProgress();
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

// A puzzle title (arbitrary user text) isn't safe to use as-is in a
// downloaded filename -- "/" or invalid characters could confuse the
// browser's save dialog, quotes could look broken, etc. Mirrors the
// backend's own _safe_filename_stem (app.py) exactly, so a client-side
// filename (image export, a print document's suggested PDF name) and a
// server-side one (.puz/.ipuz/.cfp export) land on the same name for the
// same title.
function safeFilenameStem(name, fallback) {
  let base = String(name || "").trim().replace(/[^A-Za-z0-9 _-]/g, "");
  base = base.replace(/\s+/g, "_").replace(/^_+|_+$/g, "");
  return base || fallback;
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
  const slotChanged = s?.id !== optionsPanelSlotId;
  if (slotChanged) cancelPendingOptionClick();
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
      min_score: effectiveMinScore(sel, s.pattern.length), // s.pattern.length, not s.length (cell count) -- a rebus cell can make the real word longer than the slot's physical cells, and min-score overrides are keyed by word length
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
    // Only debounce a RESTART -- an edit while already looking at this
    // same slot (typing into it, or elsewhere in the grid). Freshly
    // SELECTING a slot starts verification immediately, with no delay:
    // that's not part of a rapid-edit burst the debounce exists to
    // coalesce, and waiting here made every slot selection feel like it
    // stalled for VERIFY_DEBOUNCE_MS before showing any progress at all
    // -- confirmed directly by timing real verify requests, not just a
    // suspected regression from adding the debounce.
    if (slotChanged) {
      extendVerificationIfNeeded(s, data.candidates);
    } else {
      scheduleVerification(s, data.candidates);
    }
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

  // A real double-click fires click(detail=1), click(detail=2), THEN
  // dblclick. Relying on a fixed timer to guess "was that a double-click"
  // (the previous approach here) has a real failure mode: if the actual
  // gap between the two clicks -- governed by the OS/browser's own
  // double-click-speed setting, which this code has no way to know --
  // exceeds whatever window was chosen, the first click's deferred action
  // fires anyway (setting a preview), and if a SECOND deferred action was
  // also scheduled for the second click, it can then see "same candidate
  // clicked again" and commit the WHOLE previewed grid -- which is
  // exactly the "double-click still fills the whole grid" bug this
  // replaced. `e.detail` sidesteps the guesswork entirely: it's the
  // browser's own native running click-count for a rapid sequence (1 for
  // a lone click, 2 for the second click of a double-click, using the
  // SAME threshold `dblclick` itself is based on) -- so a second click
  // (detail >= 2) is recognized immediately, synchronously, with no timer
  // race possible, and acts directly via applyWordToSlot rather than
  // going through onOptionClick's preview/commit state machine at all,
  // so there's no path left by which a double-click could ever trigger a
  // whole-grid commitPreview().
  listEl.querySelectorAll(".option-row").forEach((row) => {
    row.addEventListener("click", (e) => {
      cancelPendingOptionClick();
      const word = row.getAttribute("data-word");
      if (e.detail >= 2) {
        applyWordToSlot(slot, word);
        return;
      }
      // A lone click so far -- deferred briefly in case a second click
      // (handled above, on ITS OWN event) follows and upgrades this into
      // a double-click instead. Only ever reachable with detail === 1,
      // so this is never the second half of a double-click itself.
      pendingOptionClick = setTimeout(() => {
        pendingOptionClick = null;
        onOptionClick(slot, word);
      }, DOUBLE_CLICK_MS);
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

// Debounces the actual verification KICKOFF (not the candidate list
// itself, which updateOptionsPanel already renders immediately either
// way) -- called from there on every render, including one for every
// single keystroke, since a cached "verified" result is invalidated on
// every real edit (see invalidateVerificationCache) and correctness
// requires re-checking against the LATEST grid state before showing
// anything as verified again. Left undebounced, rapid typing anywhere in
// the grid would restart the currently-viewed slot's verification batch
// (a real subprocess spawn + solve per candidate) on every keystroke,
// never letting it run long enough to actually finish -- confirmed
// directly as a real cause of verification appearing to load very
// slowly or not at all. Waiting for a brief pause before actually
// starting means intermediate, about-to-be-superseded states never get
// verified at all (not wasted work), while the final state, once the
// user actually pauses, gets a real, uninterrupted batch.
let verifyDebounceTimer = null;
const VERIFY_DEBOUNCE_MS = 400;

function scheduleVerification(slot, candidates) {
  if (verifyDebounceTimer) clearTimeout(verifyDebounceTimer);
  verifyDebounceTimer = setTimeout(() => {
    verifyDebounceTimer = null;
    extendVerificationIfNeeded(slot, candidates);
  }, VERIFY_DEBOUNCE_MS);
}

// Extends background verification to cover up to `visibleLimit`
// candidates (capped at VERIFY_MAX total) whenever more of the list
// becomes visible -- called (debounced, see scheduleVerification above)
// on every render and again (directly, undebounced -- a deliberate,
// infrequent user action, not rapid-fire like typing) each time "Show
// more" is clicked, so newly-revealed candidates actually get checked
// instead of staying permanently unverified just because they weren't
// among the first page.
function extendVerificationIfNeeded(slot, candidates) {
  const key = verifyKeyFor(slot);
  const target = Math.min(visibleLimit, VERIFY_MAX, candidates.length);
  if ((verifiedCompleteCount.get(key) || 0) >= target) return; // already fully covered
  if (activeVerificationKey === key && activeVerificationTarget >= target) return; // already in progress, covers this much or more -- don't restart it
  startVerificationBatch(slot, candidates, target);
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
  // Bumping the token alone stops a running batch's loop from *issuing
  // further* checks (see its own token guard), but its "this key is
  // actively being verified" claim is only ever cleared from the
  // batch's OWN successful-completion path -- which a stop-mid-flight
  // never reaches. Left uncleared here, extendVerificationIfNeeded would
  // wrongly keep believing this key's verification is still in progress
  // (nothing running to ever finish and clear it), permanently blocking
  // a real, later attempt to verify it again.
  activeVerificationKey = null;
  activeVerificationTarget = 0;
  // A debounced kickoff (see scheduleVerification) that hasn't fired yet
  // would otherwise still go off VERIFY_DEBOUNCE_MS later regardless of
  // this call -- e.g. mid-Fill, exactly the CPU contention this function
  // exists to prevent right before starting one.
  if (verifyDebounceTimer) {
    clearTimeout(verifyDebounceTimer);
    verifyDebounceTimer = null;
  }
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
  const key = verifyKeyFor(slot);
  // Claimed immediately (synchronously, before the first await) so any
  // OTHER extendVerificationIfNeeded call for this exact key -- e.g. from
  // an edit elsewhere in the grid triggering updateOptionsPanel again --
  // sees this batch as already in progress and skips starting a second,
  // redundant one that would just cancel this one's in-flight subprocess.
  // A genuinely newer call (different key, or a bigger target for this
  // one) still legitimately overwrites this claim -- that's the correct
  // "supersede" behavior the token check below already handles; only a
  // same-key/same-or-smaller-target call is what this exists to stop.
  activeVerificationKey = key;
  activeVerificationTarget = target;
  const epoch = verificationCacheEpoch;
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
      // existing (visible in this word's own preview). BUT: this result
      // was gathered against whatever grid state existed when the
      // request was SENT (see the `puzzle` field above), which -- thanks
      // to scheduleVerification's debounce -- can be well before an edit
      // that's since invalidated the cache the token check alone doesn't
      // catch this: verifyBatchToken only moves when a NEW batch actually
      // starts, which the debounce delays, so a same-key request that was
      // already in flight when the cache was wiped can still resolve
      // with a valid-looking token, despite being stale. Without this
      // epoch check, recordFeasibleGridForAllSlots would re-populate the
      // freshly-wiped cache with pre-edit feasibility data for OTHER
      // slots via cross-slot sharing -- confirmed directly as a real bug
      // this introduced, not just a theoretical risk.
      if (epoch === verificationCacheEpoch) recordFeasibleGridForAllSlots(result.grid);
    } else if (result.feasible === false) {
      verifiedMap.set(c.word, { feasible: false, grid: null });
    } // feasible === null (a verify-side error, not a real infeasibility finding) -- leave unset

    // Re-render whatever's actually on screen right now, which may be a
    // DIFFERENT slot than the one this batch is for -- recordFeasibleGridForAllSlots
    // above can have just updated it via cross-slot sharing.
    if (lastRenderedSlot) renderOptionsList(lastRenderedSlot, lastRenderedCandidates);
  }
  // The epoch check guards against a cache wipe (invalidateVerificationCache,
  // from an edit elsewhere) having detached `verifiedMap` from the
  // CURRENT verificationCache partway through this loop: if that
  // happened, every result gathered above went into a Map nothing can
  // see anymore, so marking this key "complete" here would wrongly tell
  // a later, genuine check "nothing left to verify" for results that
  // were never actually gathered into anything reachable.
  if (token === verifyBatchToken && epoch === verificationCacheEpoch) {
    verifiedCompleteCount.set(key, target);
    // Only clear the claim if it's still this call's own -- a NEWER call
    // (for a different key, or this same key at a bigger target) would
    // have already overwritten it with ITS OWN claim, which this older,
    // now-finished call must not clobber.
    if (activeVerificationKey === key && activeVerificationTarget === target) {
      activeVerificationKey = null;
      activeVerificationTarget = 0;
    }
  }
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
  // sliceWordForSlot, not word[i] per cell -- a rebus cell's own chunk can
  // be more than one character (e.g. "AD"), which word[i] would silently
  // truncate to just its first character. Returned as row ARRAYS, not
  // row.join("")-ed strings: renderGrid's previewGrid[r][c] indexing works
  // identically either way, but joining into a string would corrupt every
  // cell's column alignment after a multi-character chunk in the same row.
  const chunks = sliceWordForSlot(slot, word);
  slot.cells.forEach(([r, c], i) => {
    rows[r][c] = chunks[i];
  });
  return rows;
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
      setPreview(slot.id, word, verified.grid, true);
    }
    commitPreview();
    return;
  }
  if (verified?.feasible && verified.grid) {
    setPreview(slot.id, word, verified.grid, true);
  } else {
    setPreview(slot.id, word, singleSlotPreviewGrid(slot, word), false);
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
  // sliceWordForSlot, not word[i] per cell -- `word` was matched against
  // this slot's rebus-aware pattern (see updateOptionsPanel), so a rebus
  // cell's own chunk here can be more than one character (e.g. "AD" for a
  // 6-letter "ADAPTS" landing on a 5-cell slot); word[i] would silently
  // truncate it to one character and misalign every cell after it.
  const chunks = sliceWordForSlot(slot, word);
  for (let i = 0; i < slot.cells.length; i++) {
    const [r, c] = slot.cells[i];
    // Never overwrite a rebus square (see isRebusCell) -- its own chunk
    // above is provably identical to what's already there (sliced using
    // that same cell's current length), so skipping the write is a no-op
    // either way; kept explicit as the same defensive invariant every
    // other write path in this file follows.
    if (isRebusCell(r, c)) continue;
    puzzle.letters[r][c] = chunks[i];
  }
  renderGrid();
  refreshSlotsAndStats();
}

// ---------------------------------------------------------------------------
// Dictionaries tab
// ---------------------------------------------------------------------------

// Called on initial boot AND after uploading a new dictionary (to pick up
// the new entry in the <select> lists) -- those two cases need different
// defaulting behavior, which is the whole reason this doesn't just always
// reset to dictionaries[0]: on a fresh boot dictSelections starts empty,
// so defaulting to the first dictionary is exactly right, but re-running
// this after an upload was clobbering whatever the user had already
// selected back to dictionaries[0] every time, even though their actual
// selection was still perfectly valid in the refreshed list.
async function loadDictionaries() {
  const data = await api("/api/dictionaries");
  dictionaries = data.dictionaries;
  const acrossSel = document.getElementById("across-dict-select");
  const downSel = document.getElementById("down-dict-select");
  const options = dictionaries.map((d) => `<option value="${d.path}">${escapeAttr(d.name)}</option>`).join("");
  acrossSel.innerHTML = options;
  downSel.innerHTML = options;
  if (!dictionaries.length) return;

  const knownPaths = new Set(dictionaries.map((d) => d.path));
  // Only fall back to the first dictionary when there's genuinely nothing
  // valid selected -- a fresh boot's empty path, or a previously-selected
  // one that no longer exists in the refreshed list.
  if (!dictSelections.across.path || !knownPaths.has(dictSelections.across.path)) {
    dictSelections.across.path = dictionaries[0].path;
  }
  if (!dictSelections.down.path || !knownPaths.has(dictSelections.down.path)) {
    dictSelections.down.path = dictionaries[0].path;
  }
  acrossSel.value = dictSelections.across.path;
  downSel.value = dictSelections.down.path;
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
    try {
      await api("/api/dictionaries/upload", { method: "POST", body: formData });
      await loadDictionaries();
      setStatus(`Uploaded dictionary "${file.name}"`, "ok");
    } catch (err) {
      setStatus(`Dictionary upload failed: ${err.message}`, "error");
    }
    e.target.value = "";
  });
}

// ---------------------------------------------------------------------------
// Grid title -- an editable field directly above the grid, kept in sync
// with the Info tab's own title field (see wireInfoTab's "title" case).
// Two places to edit the same puzzle.title exist deliberately: the Info
// tab is where every other piece of metadata (author, copyright, notes)
// already lives, but a title is looked at and changed far more often than
// those -- worth surfacing right where the grid itself is, not just
// buried in a tab most sessions never open.
// ---------------------------------------------------------------------------

function renderGridTitle() {
  const el = document.getElementById("grid-title");
  if (!el || !puzzle) return;
  if (document.activeElement !== el) el.value = puzzle.title || "";
}

function wireGridTitle() {
  document.getElementById("grid-title").addEventListener("input", (e) => {
    if (!puzzle) return;
    puzzle.title = e.target.value;
    const metaTitle = document.getElementById("meta-title");
    if (metaTitle && document.activeElement !== metaTitle) metaTitle.value = puzzle.title;
    scheduleSave();
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
      // The title also has its own editable field above the grid (see
      // wireGridTitle) -- keep the two in sync, but never while the other
      // one currently has focus (that would stomp on an in-progress edit
      // there mid-keystroke).
      if (key === "title") {
        const gridTitle = document.getElementById("grid-title");
        if (gridTitle && document.activeElement !== gridTitle) gridTitle.value = puzzle.title;
      }
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
// Rebus tab -- lets a constructor put more than one character into a
// single square (e.g. "STAR"), a standard crossword convention ordinary
// typing has no way to produce (one keystroke always means one cell, one
// character). A rebus cell is just a puzzle.letters[r][c] value longer
// than one character -- no separate flag or parallel data structure --
// so every other feature that reads a cell's letter (rendering, stats,
// save/load, export) already sees it; the specific places that need
// exactly one character (Fill, dictionary matching) instead use its
// first character as a stand-in -- see grid_model.Puzzle.solving_letter
// on the backend, which every one of those paths funnels through.
// ---------------------------------------------------------------------------

function isRebusCell(r, c) {
  const letter = puzzle.letters[r][c];
  return !!letter && letter !== EMPTY && letter.length > 1;
}

// How many characters this cell currently contributes to its slot's
// spelled-out word -- 1 for a normal letter or a still-blank cell, or a
// rebus cell's own length (e.g. 2 for "AD"). Mirrors grid_model.py's
// Puzzle.slot_cell_lengths on the backend exactly.
function cellContentLength(r, c) {
  const letter = puzzle.letters[r][c];
  return letter && letter !== EMPTY ? letter.length : 1;
}

// Splits `word` into one chunk per cell of `slot`, each chunk's length
// matching cellContentLength -- the inverse of how a rebus-aware pattern
// (see updateOptionsPanel's use of s.pattern, built server-side by
// slot_pattern) concatenates cell content into one string. A candidate
// word matched against that pattern is guaranteed to have exactly the
// right total length; this is what lets e.g. "ADAPTS" (6 letters) land
// correctly on a 5-cell slot whose first cell already holds the rebus
// "AD" -- "AD" back into that one cell, "A","P","T","S" one each into the
// rest -- rather than naively assuming one character per cell.
function sliceWordForSlot(slot, word) {
  const chunks = [];
  let pos = 0;
  for (const [r, c] of slot.cells) {
    const length = cellContentLength(r, c);
    chunks.push(word.slice(pos, pos + length));
    pos += length;
  }
  return chunks;
}

function renderRebusTab() {
  const note = document.getElementById("rebus-selection-note");
  if (!note || !puzzle) return; // tab markup not mounted yet, or no puzzle loaded
  const input = document.getElementById("rebus-input");
  const setBtn = document.getElementById("btn-rebus-set");
  const clearBtn = document.getElementById("btn-rebus-clear");

  const disableAll = (message) => {
    note.textContent = message;
    input.disabled = true;
    setBtn.disabled = true;
    clearBtn.disabled = true;
  };

  if (!selected) {
    disableAll("Click a cell in the grid first.");
    input.value = "";
  } else {
    const { row, col } = selected;
    if (puzzle.blocks[row][col]) {
      disableAll(`Cell (${row + 1}, ${col + 1}) is a block -- select an open cell.`);
      input.value = "";
    } else {
      const current = puzzle.letters[row][col];
      note.textContent = `Selected cell: row ${row + 1}, column ${col + 1}${isRebusCell(row, col) ? " -- currently a rebus square" : ""}`;
      input.disabled = false;
      setBtn.disabled = false;
      clearBtn.disabled = current === EMPTY;
      // Only overwrite the input while the user isn't actively typing into
      // it -- renderGrid() (which calls this) fires on essentially every
      // grid edit, and stomping on an in-progress edit here on every one
      // of those would make the field unusable.
      if (document.activeElement !== input) {
        input.value = current !== EMPTY ? current : "";
      }
    }
  }

  const list = document.getElementById("rebus-list");
  const entries = [];
  for (let r = 0; r < puzzle.height; r++) {
    for (let c = 0; c < puzzle.width; c++) {
      if (!puzzle.blocks[r][c] && isRebusCell(r, c)) entries.push({ r, c, letter: puzzle.letters[r][c] });
    }
  }
  list.innerHTML = entries.length
    ? entries
        .map(
          (e) =>
            `<div class="rebus-list-row" data-row="${e.r}" data-col="${e.c}"><span class="rebus-list-pos">(${e.r + 1},${e.c + 1})</span> ${escapeAttr(e.letter)}</div>`
        )
        .join("")
    : `<div class="hint">No rebus squares yet.</div>`;
  list.querySelectorAll(".rebus-list-row").forEach((row) => {
    row.addEventListener("click", () => {
      selected = { row: Number(row.getAttribute("data-row")), col: Number(row.getAttribute("data-col")) };
      renderGrid();
      updateOptionsPanel();
      highlightActiveClue();
    });
  });
}

// Writes the Rebus tab's input into the selected cell -- any non-empty
// string, uppercased (matching every other letter already in the grid),
// one character or several. A single-character value lands as a
// completely ordinary letter, not specially marked as "set via this tab"
// -- there's nothing left to distinguish once it's in puzzle.letters,
// exactly as intended (see isRebusCell: length is the only thing that
// matters).
function setRebusAtSelected() {
  if (!selected || !puzzle) return;
  const { row, col } = selected;
  if (puzzle.blocks[row][col]) return;
  const value = document.getElementById("rebus-input").value.trim().toUpperCase();
  if (!value) return;
  snapshotForUndo();
  puzzle.letters[row][col] = value;
  renderGrid();
  refreshSlotsAndStats();
  setStatus(value.length > 1 ? `Set rebus square: ${value}` : `Set letter: ${value}`, "ok");
}

function clearRebusAtSelected() {
  if (!selected || !puzzle) return;
  const { row, col } = selected;
  if (puzzle.blocks[row][col]) return;
  snapshotForUndo();
  puzzle.letters[row][col] = EMPTY;
  renderGrid();
  refreshSlotsAndStats();
  setStatus("Cleared", "ok");
}

function wireRebusTab() {
  document.getElementById("btn-rebus-set").addEventListener("click", setRebusAtSelected);
  document.getElementById("btn-rebus-clear").addEventListener("click", clearRebusAtSelected);
  document.getElementById("rebus-input").addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      setRebusAtSelected();
    }
  });
}

// ---------------------------------------------------------------------------
// Print + image export
//
// Two different output paths, both built from the same puzzle/slots state
// already in memory (no backend round trip):
//   - Print: builds a full, self-contained HTML document string, then hands
//     it to printHtmlDocument, which is the only piece that actually opens
//     a window and calls print() -- kept thin and separate from the HTML
//     builders so the builders themselves (the part with actual layout
//     logic worth getting right) can be tested without a real print
//     dialog or popup window.
//   - Image: builds a plain array of per-cell data (buildImageCellData,
//     also pure/testable), then drawPuzzleCanvas renders it to a <canvas>
//     and exportImage downloads it as a PNG.
// ---------------------------------------------------------------------------

// One <table> for a grid diagram, reused by both print layouts.
// `classPrefix` scopes the cell/number/letter classes to whichever print
// stylesheet is using it (nyt-grid vs op-grid), since the two layouts size
// cells very differently. showLetters=false renders every open cell blank
// (a solving grid); true fills in whatever's in puzzle.letters (a rebus
// entry's multi-character string included, see the "rebus" class below).
function buildPrintGridTable(classPrefix, { showNumbers, showLetters }) {
  const numbers = showNumbers ? slotStartNumbers() : new Map();
  let html = `<table class="${classPrefix}">`;
  for (let r = 0; r < puzzle.height; r++) {
    html += "<tr>";
    for (let c = 0; c < puzzle.width; c++) {
      if (puzzle.blocks[r][c]) {
        html += `<td class="${classPrefix}-block"></td>`;
        continue;
      }
      const num = numbers.get(`${r},${c}`);
      const letter = showLetters && puzzle.letters[r][c] !== EMPTY ? puzzle.letters[r][c] : "";
      html += `<td class="${classPrefix}-cell">`;
      if (num) html += `<span class="${classPrefix}-num">${num}</span>`;
      if (letter) {
        html += `<span class="${classPrefix}-letter${letter.length > 1 ? " rebus" : ""}">${escapeAttr(letter)}</span>`;
      }
      html += `</td>`;
    }
    html += "</tr>";
  }
  html += "</table>";
  return html;
}

// Across/Down clue lists as plain HTML fragments, numbered and using
// whatever's currently in puzzle.clues (blank clues print as empty text
// after the number, same as the Clues tab shows an empty input).
function buildCluesHtml(classPrefix) {
  const bySlot = (dir) => slots.filter((s) => s.direction === dir).sort((a, b) => a.number - b.number);
  const line = (s) =>
    `<div class="${classPrefix}-clue"><span class="${classPrefix}-num">${s.number}.</span> ${escapeAttr(puzzle.clues[s.id] || "")}</div>`;
  return {
    acrossHtml: bySlot("across").map(line).join(""),
    downHtml: bySlot("down").map(line).join(""),
  };
}

// A submission-style packet: title/byline header, a blank (solvable) grid,
// the solved grid, then the full clue list -- the pieces a constructor
// submission conventionally bundles together. Multiple print-page divs
// (one per section) rather than trying to cram everything onto one page,
// unlike buildOnePagePuzzleHtml -- see that function's own comment for why
// the two have different space constraints.
function buildNytSubmissionHtml() {
  const title = puzzle.title || "Untitled";
  const author = puzzle.author || "";
  const { acrossHtml, downHtml } = buildCluesHtml("nyt");
  return `<!doctype html><html><head><meta charset="utf-8"><title>${escapeAttr(safeFilenameStem(title, "puzzle"))}_nytsubmission</title>
<style>
  @page { size: letter portrait; margin: 0.6in; }
  /* Without this, most browsers strip every background color (the block
     squares included) unless the user has separately opted into "print
     background graphics" -- a setting that defaults OFF in Chrome. The
     black squares are the content here, not decoration, so this forces
     them to print regardless of that setting. */
  * { -webkit-print-color-adjust: exact; print-color-adjust: exact; }
  body { font-family: Georgia, "Times New Roman", serif; color: #111; margin: 0; }
  .nyt-page { page-break-after: always; padding-top: 0.2in; }
  .nyt-page:last-child { page-break-after: auto; }
  .nyt-header { text-align: center; margin-bottom: 0.15in; }
  .nyt-header h1 { font-size: 20pt; margin: 0 0 4pt; }
  .nyt-byline { font-size: 12pt; color: #444; }
  .nyt-meta { font-size: 9pt; color: #777; margin-top: 4pt; }
  .nyt-page h2 { font-size: 13pt; text-align: center; margin: 0 0 0.15in; }
  table.nyt-grid { border-collapse: collapse; margin: 0 auto; }
  table.nyt-grid td { width: 26px; height: 26px; border: 1px solid #000; position: relative; padding: 0; }
  td.nyt-grid-block { background: #000; }
  .nyt-grid-num { position: absolute; top: 1px; left: 2px; font-size: 6.5pt; }
  .nyt-grid-letter { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-size: 14pt; font-weight: 600; }
  .nyt-grid-letter.rebus { font-size: 8pt; }
  .nyt-clue-columns { display: flex; gap: 0.4in; font-size: 10pt; }
  .nyt-clue-columns > div { flex: 1; }
  .nyt-clue-columns h3 { font-size: 11pt; border-bottom: 1px solid #000; padding-bottom: 2pt; }
  .nyt-clue { margin-bottom: 3pt; break-inside: avoid; }
  .nyt-num { font-weight: 600; }
</style></head><body>
  <div class="nyt-page">
    <div class="nyt-header">
      <h1>${escapeAttr(title)}</h1>
      <div class="nyt-byline">${author ? "by " + escapeAttr(author) : ""}</div>
      <div class="nyt-meta">${puzzle.width}×${puzzle.height} · ${stats.word_count ?? "?"} words · ${stats.block_count ?? "?"} blocks</div>
    </div>
    <h2>Grid</h2>
    ${buildPrintGridTable("nyt-grid", { showNumbers: true, showLetters: false })}
  </div>
  <div class="nyt-page">
    <h2>Solution</h2>
    ${buildPrintGridTable("nyt-grid", { showNumbers: true, showLetters: true })}
  </div>
  <div class="nyt-page">
    <h2>Clues</h2>
    <div class="nyt-clue-columns">
      <div><h3>Across</h3>${acrossHtml}</div>
      <div><h3>Down</h3>${downHtml}</div>
    </div>
  </div>
</body></html>`;
}

// A single printed page: header, grid, and clues side by side, clues laid
// out in CSS columns so they wrap to fit instead of running off the
// bottom of the page -- guaranteed to fit one page for the 15x15 case this
// was designed for; a much larger custom grid may still spill onto a
// second page (a browser print engine's own overflow, not something this
// markup can force).
function buildOnePagePuzzleHtml() {
  const title = puzzle.title || "Untitled";
  const author = puzzle.author || "";
  const { acrossHtml, downHtml } = buildCluesHtml("op");
  return `<!doctype html><html><head><meta charset="utf-8"><title>${escapeAttr(safeFilenameStem(title, "puzzle"))}_puzzle</title>
<style>
  @page { size: letter portrait; margin: 0.35in; }
  /* See buildNytSubmissionHtml's identical rule -- without it the block
     squares (a background color, not a border) disappear whenever the
     browser's "print background graphics" setting is off, which is the
     default in Chrome. */
  * { -webkit-print-color-adjust: exact; print-color-adjust: exact; }
  body { font-family: Georgia, "Times New Roman", serif; color: #111; margin: 0; font-size: 8pt; }
  .op-header { text-align: center; margin-bottom: 6pt; }
  .op-header h1 { font-size: 14pt; margin: 0; }
  .op-byline { font-size: 9pt; color: #444; }
  .op-layout { display: flex; gap: 10pt; align-items: flex-start; }
  table.op-grid { border-collapse: collapse; flex: 0 0 auto; }
  table.op-grid td { width: 17px; height: 17px; border: 0.5px solid #000; position: relative; padding: 0; }
  td.op-grid-block { background: #000; }
  .op-grid-num { position: absolute; top: 0; left: 1px; font-size: 5pt; }
  .op-grid-letter { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-size: 9pt; font-weight: 600; }
  .op-grid-letter.rebus { font-size: 5pt; }
  .op-clues { flex: 1 1 auto; display: flex; gap: 8pt; min-width: 0; }
  .op-clue-block { flex: 1; min-width: 0; }
  .op-clue-block h3 { font-size: 9pt; margin: 0 0 3pt; border-bottom: 0.5px solid #000; }
  .op-clue-list { column-count: 2; column-gap: 8pt; font-size: 7pt; line-height: 1.35; }
  .op-clue { break-inside: avoid; margin-bottom: 1pt; }
  .op-num { font-weight: 600; }
</style></head><body>
  <div class="op-header">
    <h1>${escapeAttr(title)}</h1>
    <div class="op-byline">${author ? "by " + escapeAttr(author) : ""}</div>
  </div>
  <div class="op-layout">
    ${buildPrintGridTable("op-grid", { showNumbers: true, showLetters: false })}
    <div class="op-clues">
      <div class="op-clue-block"><h3>Across</h3><div class="op-clue-list">${acrossHtml}</div></div>
      <div class="op-clue-block"><h3>Down</h3><div class="op-clue-list">${downHtml}</div></div>
    </div>
  </div>
</body></html>`;
}

// The only side-effecting piece of the print path: opens a new window,
// writes the fully-built document into it, and triggers the browser's own
// print dialog (from which the user can "Save as PDF" -- there's no
// separate PDF export path, since every modern browser's print dialog
// already is one). Kept to just this so the HTML-building functions above
// stay pure and testable.
function printHtmlDocument(html) {
  const win = window.open("", "_blank");
  if (!win) {
    setStatus("Couldn't open the print window -- check your browser's popup blocker", "error");
    return;
  }
  win.document.open();
  win.document.write(html);
  win.document.close();
  win.focus();
  win.print();
}

// Plain per-cell data for the image export -- see this section's own
// comment for why this is kept separate from actual canvas drawing.
// "grid": bare structure only (no numbers, no letters) -- just the
// black/white cell pattern. "puzzle": numbered, blank -- a solvable grid.
// "solution": numbered and filled in.
function buildImageCellData(kind) {
  const numbers = kind === "grid" ? new Map() : slotStartNumbers();
  const cells = [];
  for (let r = 0; r < puzzle.height; r++) {
    for (let c = 0; c < puzzle.width; c++) {
      const blocked = puzzle.blocks[r][c];
      const number = !blocked ? numbers.get(`${r},${c}`) || null : null;
      const letter = !blocked && kind === "solution" && puzzle.letters[r][c] !== EMPTY ? puzzle.letters[r][c] : null;
      cells.push({ row: r, col: c, blocked, number, letter });
    }
  }
  return cells;
}

// Renders buildImageCellData's output to a fresh <canvas>. Returns null
// (rather than throwing) if 2D canvas rendering isn't available at all --
// true of jsdom by default (no real canvas backend), and, in principle,
// possible in a real browser with canvas access blocked; either way,
// exportImage below turns that into a status message instead of a crash.
function drawPuzzleCanvas(cells, width, height, cellSize = 40) {
  const canvas = document.createElement("canvas");
  canvas.width = width * cellSize + 2;
  canvas.height = height * cellSize + 2;
  const ctx = canvas.getContext && canvas.getContext("2d");
  if (!ctx) return null;
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  for (const cell of cells) {
    const x = cell.col * cellSize + 1;
    const y = cell.row * cellSize + 1;
    if (cell.blocked) {
      ctx.fillStyle = "#000000";
      ctx.fillRect(x, y, cellSize, cellSize);
      continue;
    }
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(x, y, cellSize, cellSize);
    ctx.strokeStyle = "#000000";
    ctx.lineWidth = 1;
    ctx.strokeRect(x, y, cellSize, cellSize);
    if (cell.number) {
      ctx.fillStyle = "#000000";
      ctx.font = "9px sans-serif";
      ctx.textAlign = "left";
      ctx.textBaseline = "top";
      ctx.fillText(String(cell.number), x + 2, y + 1);
    }
    if (cell.letter) {
      // A rebus entry (see buildImageCellData) has more than one
      // character -- shrink the font so it still fits in one square
      // instead of overflowing into its neighbors.
      const fontSize = cell.letter.length > 1 ? Math.max(8, 18 - (cell.letter.length - 1) * 3) : 18;
      ctx.fillStyle = "#000000";
      ctx.font = `bold ${fontSize}px sans-serif`;
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText(cell.letter, x + cellSize / 2, y + cellSize / 2 + 2);
    }
  }
  return canvas;
}

function exportImage(kind) {
  if (!puzzle) return;
  const canvas = drawPuzzleCanvas(buildImageCellData(kind), puzzle.width, puzzle.height);
  if (!canvas) {
    setStatus("Image export isn't supported in this browser", "error");
    return;
  }
  const filename = `${safeFilenameStem(puzzle.title, "puzzle")}_${kind}.png`;
  canvas.toBlob((blob) => {
    // toBlob's callback, not the synchronous call below it, is when the
    // PNG actually exists (or doesn't) -- reporting "Exported" right
    // after just calling toBlob() claimed success unconditionally,
    // before the async encode even ran, let alone before knowing whether
    // it produced a real blob at all.
    if (!blob) {
      setStatus("Image export failed -- no image data was produced", "error");
      return;
    }
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    setStatus(`Exported ${filename}`, "ok");
  }, "image/png");
}

// ---------------------------------------------------------------------------
// Toolbar: new / import / export / fill
// ---------------------------------------------------------------------------

// New/Import/Load all replace `puzzle` wholesale -- none of them checked
// whether a Fill was still streaming in the background first. That Fill's
// next "improved"/"done" event would then call applyScopedResultLetters
// against whatever puzzle is current BY THEN (the new one), while reading
// letters computed for the OLD one: if the new grid is smaller, indexing
// throws (aborting the stream, confusingly, mid this unrelated action);
// if it's the same size or larger, it silently writes the old fill's
// letters into cells of the brand-new grid. A synchronous "cancel and
// proceed" isn't enough on its own to close this -- cancelFill() only
// sends the cancel signal, it doesn't wait for runFill()'s own stream
// loop to actually finish handling whatever event is already in flight --
// so this refuses outright instead, same as clicking New/Import/Load
// mid-Fill would feel like a confusing no-op or a silent corruption
// either way; an explicit Cancel first is one extra click, not a real cost.
function blockedByActiveFill() {
  if (!filling) return false;
  setStatus("A Fill is still running -- cancel it first", "error");
  return true;
}

function wireToolbar() {
  const newGridOverlay = document.getElementById("new-grid-overlay");
  const newGridWidth = document.getElementById("new-grid-width");
  const newGridHeight = document.getElementById("new-grid-height");

  const openNewGridDialog = () => {
    // Pre-filled with the current grid's own size rather than always
    // resetting to 15x15 -- "New" from an existing 21x21 grid most likely
    // means "start over at this same size," not "go back to the default."
    newGridWidth.value = puzzle ? puzzle.width : 15;
    newGridHeight.value = puzzle ? puzzle.height : 15;
    newGridOverlay.hidden = false;
    newGridWidth.focus();
    newGridWidth.select();
  };
  const closeNewGridDialog = () => {
    newGridOverlay.hidden = true;
  };
  const confirmNewGrid = async () => {
    const width = parseInt(newGridWidth.value, 10);
    const height = parseInt(newGridHeight.value, 10);
    closeNewGridDialog();
    if (!width || !height) return;
    if (blockedByActiveFill()) return;
    try {
      await newPuzzle(width, height);
      setStatus(`New ${width}×${height} grid`, "ok");
    } catch (err) {
      setStatus(`New grid failed: ${err.message}`, "error");
    }
  };

  document.getElementById("btn-new").addEventListener("click", openNewGridDialog);
  document.getElementById("new-grid-cancel").addEventListener("click", closeNewGridDialog);
  document.getElementById("new-grid-ok").addEventListener("click", confirmNewGrid);
  // Clicking the dimmed backdrop itself (not the modal box) cancels, same
  // as Cancel -- e.target is the overlay only when the click didn't land
  // on anything inside it.
  newGridOverlay.addEventListener("click", (e) => {
    if (e.target === newGridOverlay) closeNewGridDialog();
  });
  [newGridWidth, newGridHeight].forEach((input) => {
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        confirmNewGrid();
      } else if (e.key === "Escape") {
        e.preventDefault();
        closeNewGridDialog();
      }
    });
  });

  document.getElementById("input-import").addEventListener("change", async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    if (blockedByActiveFill()) {
      e.target.value = "";
      return;
    }
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
      clearCurrentSave();
      clearFillFailedHighlight();
      clearForcedCells();
      clearVerificationCache();
      renderAll();
      setStatus(`Imported "${file.name}"`, "ok");
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
  const aboutBtn = document.getElementById("btn-about");
  const aboutMenu = document.getElementById("about-menu");
  aboutBtn.addEventListener("click", () => aboutMenu.classList.toggle("open"));
  document.addEventListener("click", (e) => {
    if (!aboutBtn.contains(e.target) && !aboutMenu.contains(e.target)) {
      aboutMenu.classList.remove("open");
    }
  });

  // Every export path is named after the puzzle's title (see
  // safeFilenameStem's call sites), so none of them make sense without
  // one -- gate all three (print/image/format) behind this. A title
  // already set just runs `action` immediately; an empty one opens a
  // themed modal (matching Save as/New grid) to collect one first, then
  // runs `action` once it's set.
  const requireTitleOverlay = document.getElementById("require-title-overlay");
  const requireTitleInput = document.getElementById("require-title-input");
  let pendingExportAction = null;

  const closeRequireTitleDialog = () => {
    requireTitleOverlay.hidden = true;
    pendingExportAction = null;
  };
  const confirmRequireTitle = () => {
    const title = requireTitleInput.value.trim();
    if (!title) return; // still required -- leave the dialog open rather than silently dropping the export
    puzzle.title = title;
    renderGridTitle();
    const metaTitle = document.getElementById("meta-title");
    if (metaTitle) metaTitle.value = title;
    scheduleSave();
    const action = pendingExportAction;
    closeRequireTitleDialog();
    if (action) action();
  };
  document.getElementById("require-title-cancel").addEventListener("click", closeRequireTitleDialog);
  document.getElementById("require-title-ok").addEventListener("click", confirmRequireTitle);
  requireTitleOverlay.addEventListener("click", (e) => {
    if (e.target === requireTitleOverlay) closeRequireTitleDialog();
  });
  requireTitleInput.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      confirmRequireTitle();
    } else if (e.key === "Escape") {
      e.preventDefault();
      closeRequireTitleDialog();
    }
  });
  const ensureTitleThenRun = (action) => {
    if (!puzzle) return;
    if (puzzle.title && puzzle.title.trim()) {
      action();
      return;
    }
    pendingExportAction = action;
    requireTitleInput.value = "";
    requireTitleOverlay.hidden = false;
    requireTitleInput.focus();
  };

  exportMenu.querySelectorAll("button[data-print]").forEach((btn) => {
    btn.addEventListener("click", () => {
      exportMenu.classList.remove("open");
      const mode = btn.getAttribute("data-print");
      ensureTitleThenRun(() => {
        printHtmlDocument(mode === "nyt" ? buildNytSubmissionHtml() : buildOnePagePuzzleHtml());
      });
    });
  });

  exportMenu.querySelectorAll("button[data-image]").forEach((btn) => {
    btn.addEventListener("click", () => {
      exportMenu.classList.remove("open");
      ensureTitleThenRun(() => exportImage(btn.getAttribute("data-image")));
    });
  });

  exportMenu.querySelectorAll("button[data-format]").forEach((btn) => {
    btn.addEventListener("click", () => {
      exportMenu.classList.remove("open");
      const format = btn.getAttribute("data-format");
      ensureTitleThenRun(async () => {
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

async function saveAsNamed(name) {
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
  const saveAsOverlay = document.getElementById("save-as-overlay");
  const saveAsName = document.getElementById("save-as-name");

  const openSaveAsDialog = () => {
    if (!puzzle) return;
    saveAsName.value = currentSaveName || puzzle.title || "My puzzle";
    saveAsOverlay.hidden = false;
    saveAsName.focus();
    saveAsName.select();
  };
  const closeSaveAsDialog = () => {
    saveAsOverlay.hidden = true;
  };
  const confirmSaveAs = async () => {
    const name = saveAsName.value.trim();
    closeSaveAsDialog();
    await saveAsNamed(name);
  };

  document.getElementById("btn-save").addEventListener("click", openSaveAsDialog);
  document.getElementById("save-as-cancel").addEventListener("click", closeSaveAsDialog);
  document.getElementById("save-as-ok").addEventListener("click", confirmSaveAs);
  // Clicking the dimmed backdrop itself (not the modal box) cancels, same
  // as Cancel -- matches the New grid dialog's own backdrop behavior.
  saveAsOverlay.addEventListener("click", (e) => {
    if (e.target === saveAsOverlay) closeSaveAsDialog();
  });
  saveAsName.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      confirmSaveAs();
    } else if (e.key === "Escape") {
      e.preventDefault();
      closeSaveAsDialog();
    }
  });

  document.getElementById("load-select").addEventListener("change", async (e) => {
    const name = e.target.value;
    if (!name) return;
    if (blockedByActiveFill()) {
      e.target.value = currentSaveName || "";
      return;
    }
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
      clearForcedCells();
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

// The set of "r,c" keys reachable from the currently selected cell's
// slot(s) via crossings (BFS over the crossing-adjacency graph slots
// form) -- i.e. everything Fill should actually touch when run from
// here, leaving the rest of the grid untouched. Returns null (meaning
// "no scope -- the whole grid") if no cell is selected, or if the
// selected cell isn't part of any real slot at all (e.g. an isolated
// gap too short to be one) -- there's nothing to scope to in either
// case. Starts from BOTH the across and down slot at the selected cell
// (not just whichever direction the cursor is currently facing), since
// the connected region physically includes both from that cell.
function connectedFillScope() {
  if (!selected) return null;
  const key = (r, c) => `${r},${c}`;
  const startKey = key(selected.row, selected.col);

  const slotsByCell = new Map();
  for (const s of slots) {
    for (const [r, c] of s.cells) {
      const k = key(r, c);
      if (!slotsByCell.has(k)) slotsByCell.set(k, []);
      slotsByCell.get(k).push(s);
    }
  }

  const startSlots = slotsByCell.get(startKey);
  if (!startSlots || !startSlots.length) return null;

  const visitedSlotIds = new Set();
  const queue = [...startSlots];
  while (queue.length) {
    const s = queue.pop();
    if (visitedSlotIds.has(s.id)) continue;
    visitedSlotIds.add(s.id);
    for (const [r, c] of s.cells) {
      for (const other of slotsByCell.get(key(r, c)) || []) {
        if (!visitedSlotIds.has(other.id)) queue.push(other);
      }
    }
  }

  const scopeCells = new Set();
  for (const s of slots) {
    if (!visitedSlotIds.has(s.id)) continue;
    for (const [r, c] of s.cells) scopeCells.add(key(r, c));
  }
  return scopeCells;
}

// Writes `resultLetters` (a row-string array from a /api/fill response's
// puzzle.letters, or the plain-string rows FilledGridRows produces) into
// the REAL puzzle.letters, but only for cells inside `scopeCells` (or
// every open cell, if `scopeCells` is null -- no scoping, i.e. the whole
// grid was the request's scope). Cells outside the scope were
// artificially blocked in the request actually sent to the solver (see
// runFill), so the response's letters there are meaningless "#"
// placeholders standing in for a block, not real solver output --
// merging instead of wholesale-replacing puzzle (the old, unscoped
// behavior) is what keeps those from silently overwriting cells Fill
// was never asked to touch. Also never overwrites a cell that's currently
// a rebus square (isRebusCell): the solver only ever sees and echoes back
// that cell's single first-character stand-in (see grid_model.Puzzle.
// solving_letter on the backend), which is right as a crossing constraint
// but would silently collapse e.g. "STAR" down to just "S" if written
// back here.
function applyScopedResultLetters(resultLetters, scopeCells) {
  for (let r = 0; r < puzzle.height; r++) {
    for (let c = 0; c < puzzle.width; c++) {
      if (puzzle.blocks[r][c]) continue;
      if (scopeCells && !scopeCells.has(`${r},${c}`)) continue;
      if (isRebusCell(r, c)) continue;
      const ch = resultLetters[r][c];
      if (ch && ch !== "#") puzzle.letters[r][c] = ch;
    }
  }
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
  // Scoped to the connected region around the cursor (via crossings) if
  // a cell is selected; null ("no scope", the whole grid) if not -- see
  // connectedFillScope's own doc comment for exactly what "connected"
  // means here and why blocking everything outside it in the REQUEST is
  // safe (never changes any in-scope slot's own shape).
  const scopeCells = connectedFillScope();

  filling = true;
  clearFillFailedHighlight();
  clearForcedCells();
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
  // Everything outside the scope is blocked in THIS request only -- the
  // real puzzle (and beforeFill, captured above) is untouched. Every
  // in-scope slot's own cells are unaffected: scopeCells is closed under
  // crossings by construction, so no in-scope slot shares a cell with
  // anything now being blocked.
  let requestPuzzle = puzzle;
  if (scopeCells) {
    requestPuzzle = JSON.parse(JSON.stringify(puzzle));
    for (let r = 0; r < requestPuzzle.height; r++) {
      for (let c = 0; c < requestPuzzle.width; c++) {
        if (!scopeCells.has(`${r},${c}`)) requestPuzzle.blocks[r][c] = true;
      }
    }
  }
  try {
    const resp = await fetch("/api/fill", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        puzzle: requestPuzzle,
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
          applyScopedResultLetters(event.puzzle.letters, scopeCells);
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
        applyScopedResultLetters(finalEvent.puzzle.letters, scopeCells);
        // Absent (defaults to []) in maximize mode -- see app.py's /api/fill
        // docstring -- where "forced" isn't a meaningful concept. Also
        // excludes any cell that already had a letter before this Fill ran
        // (beforeFill, captured pre-request): "forced" is only meaningful
        // for a letter Fill itself just determined -- a cell the user typed
        // in beforehand isn't newly forced by this solve just because its
        // slot's pattern happens to admit only that one dictionary word.
        forcedCells = new Set(
          (finalEvent.forced_cells || [])
            .filter(([r, c]) => beforeFill.letters[r][c] === EMPTY)
            .map(([r, c]) => `${r},${c}`)
        );
        await refreshSlotsAndStats();
        renderGrid();
        const scopeNote = scopeCells ? " (connected region only)" : "";
        setStatus(
          (maximize
            ? `Proven optimal — score ${st.score.toLocaleString()} (${st.time_seconds.toFixed(2)}s, ${st.nodes.toLocaleString()} nodes)`
            : `Solved in ${st.time_seconds.toFixed(2)}s (${st.nodes} nodes, ${st.restarts} restarts)`) + scopeNote,
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
        min_score: effectiveMinScore(sel, s.pattern.length), // s.pattern.length, not s.length (cell count) -- a rebus cell can make the real word longer than the slot's physical cells, and min-score overrides are keyed by word length
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

// ---------------------------------------------------------------------------
// Side panel resize -- drag the thin strip between the grid and the side
// panel to widen/narrow it. Tracked purely through the panel's own inline
// style.width (set here, read back here), never getBoundingClientRect():
// that needs a real layout pass to return anything meaningful, which a
// headless test environment doesn't do, where reading back exactly what
// this code itself just wrote works identically everywhere.
// ---------------------------------------------------------------------------

const SIDE_PANEL_DEFAULT_WIDTH = 380; // matches #side-panel's CSS default
const SIDE_PANEL_MIN_WIDTH = 260;
const SIDE_PANEL_MAX_WIDTH = 700;

function wireSidePanelResizer() {
  const resizer = document.getElementById("side-panel-resizer");
  const sidePanel = document.getElementById("side-panel");

  let savedWidth = null;
  try {
    savedWidth = parseInt(localStorage.getItem("xfill-sidepanel-width"), 10);
  } catch (_) {
    // No persisted width -- falls through to the CSS default below.
  }
  if (savedWidth && !Number.isNaN(savedWidth)) {
    sidePanel.style.width = `${Math.max(SIDE_PANEL_MIN_WIDTH, Math.min(SIDE_PANEL_MAX_WIDTH, savedWidth))}px`;
  }

  let dragging = false;
  let startX = 0;
  let startWidth = 0;

  resizer.addEventListener("mousedown", (e) => {
    dragging = true;
    startX = e.clientX;
    startWidth = parseInt(sidePanel.style.width, 10) || SIDE_PANEL_DEFAULT_WIDTH;
    resizer.classList.add("resizing");
    document.body.style.userSelect = "none"; // keeps a fast drag from selecting page text
    e.preventDefault();
  });

  document.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    // The panel sits on the right edge of the screen -- dragging the
    // handle LEFT (negative delta) widens it, so this is subtraction.
    const delta = e.clientX - startX;
    const newWidth = Math.max(SIDE_PANEL_MIN_WIDTH, Math.min(SIDE_PANEL_MAX_WIDTH, startWidth - delta));
    sidePanel.style.width = `${newWidth}px`;
  });

  document.addEventListener("mouseup", () => {
    if (!dragging) return;
    dragging = false;
    resizer.classList.remove("resizing");
    document.body.style.userSelect = "";
    try {
      localStorage.setItem("xfill-sidepanel-width", String(parseInt(sidePanel.style.width, 10)));
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
  renderRebusTab();
  renderGridTitle();
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
  wireRebusTab();
  wireGridTitle();
  wireStyleControls();
  wireThemeToggle();
  wireOptionsSort();
  wireSidePanelResizer();
  wireSubstringHighlight();
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
