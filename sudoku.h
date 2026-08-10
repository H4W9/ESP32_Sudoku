#pragma once
#ifndef sudoku_h
#define sudoku_h
#include <Arduino.h>

// ============================================================================
//  Sudoku model — the 9x9 grid, its rules, pencil marks, hints and an undo
//  history. No rendering and no persistence I/O live here; the sketch owns the
//  board view and the save file, and calls into this for every rule question.
//
//  Cells are indexed 0..80 in row-major order (i = row*9 + col). A value of 0
//  means empty. `given` marks the puzzle's fixed clues (locked, never editable).
//  Notes are a bitmask: bit d (1..9) set means pencil-mark d is shown; bit 0 is
//  unused.
// ============================================================================
class Sudoku {
 public:
  static const uint8_t N     = 9;
  static const uint8_t CELLS = 81;

  // --- loading -------------------------------------------------------------
  // Pull entry `index` of difficulty tier `diff` (0..3) out of the embedded
  // bank (puzzles.h). Resets the board to the puzzle's clues.
  void loadFromBank(uint8_t diff, uint16_t index);
  // Restore from a self-contained save: three 41-byte packed grids (givens,
  // current values, solution) plus 81 note masks.
  void loadState(const uint8_t *given41, const uint8_t *cur41,
                 const uint8_t *sol41, const uint16_t *notes81,
                 uint8_t diff, uint16_t index);

  // --- queries -------------------------------------------------------------
  uint8_t  value(uint8_t i)  const { return v_[i]; }         // current grid value
  uint8_t  solution(uint8_t i) const { return s_[i]; }
  bool     isGiven(uint8_t i) const { return g_[i] != 0; }
  uint16_t notes(uint8_t i)  const { return n_[i]; }
  bool     hasNote(uint8_t i, uint8_t d) const { return n_[i] & (1u << d); }

  bool     conflict(uint8_t i) const;   // v_[i] duplicates a row/col/box peer
  bool     isComplete() const;          // every cell filled
  bool     isSolved() const;            // filled AND matches the solution
  uint8_t  filledCount() const;
  uint8_t  countOf(uint8_t d) const;    // how many of digit d are on the board
  bool     digitExhausted(uint8_t d) const { return countOf(d) >= 9; }
  int      firstEmpty() const;          // index of first empty cell, or -1

  uint8_t  difficulty() const { return diff_; }
  uint16_t bankIndex()  const { return idx_; }
  uint8_t  givenValue(uint8_t i) const { return g_[i]; }

  // --- mutation (all no-ops on given cells) --------------------------------
  // Place digit d (1..9) at cell i. If autoErase, remove pencil-mark d from the
  // cell's peers. Returns true if the board changed. Records undo.
  bool setValue(uint8_t i, uint8_t d, bool autoErase);
  bool clearCell(uint8_t i);            // erase value and notes. Records undo.
  bool toggleNote(uint8_t i, uint8_t d);// flip pencil-mark d. Records undo.
  bool applyHint(uint8_t i);            // fill cell i from the solution. Records undo.

  bool canUndo() const { return uhead_ != utail_; }
  bool undo();                          // revert the last change. Returns true if one was undone.

  // --- persistence helpers -------------------------------------------------
  void packGiven(uint8_t *out41)   const { pack(g_, out41); }
  void packValues(uint8_t *out41)  const { pack(v_, out41); }
  void packSolution(uint8_t *out41)const { pack(s_, out41); }
  void copyNotes(uint16_t *out81)  const { memcpy(out81, n_, sizeof(n_)); }

 private:
  uint8_t  g_[81];   // givens (0 or 1..9) — which cells are locked
  uint8_t  v_[81];   // current values (starts equal to givens)
  uint8_t  s_[81];   // the unique solution
  uint16_t n_[81];   // pencil-mark bitmasks
  uint8_t  diff_ = 0;
  uint16_t idx_  = 0;

  // Undo ring of single-cell snapshots taken just before each change.
  static const uint8_t UMAX = 96;
  struct Snap { uint8_t idx; uint8_t val; uint16_t note; };
  Snap    undo_[UMAX];
  uint8_t uhead_ = 0, utail_ = 0;   // empty when equal; drops oldest when full
  void    pushUndo(uint8_t i);

  static void unpack(const uint8_t *b41, uint8_t *out81);
  static void pack(const uint8_t *in81, uint8_t *b41);
};

#endif // sudoku_h
