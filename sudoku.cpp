#include "sudoku.h"
#include "puzzles.h"

// Low-nibble-first unpack: byte i holds cell 2i in bits 0-3, cell 2i+1 in 4-7.
void Sudoku::unpack(const uint8_t *b41, uint8_t *out81) {
  for (uint8_t i = 0; i < CELLS; i++) {
    uint8_t byte = b41[i >> 1];
    out81[i] = (i & 1) ? (byte >> 4) : (byte & 0x0F);
  }
}
void Sudoku::pack(const uint8_t *in81, uint8_t *b41) {
  memset(b41, 0, 41);
  for (uint8_t i = 0; i < CELLS; i++) {
    if (i & 1) b41[i >> 1] |= (in81[i] & 0x0F) << 4;
    else       b41[i >> 1] |= (in81[i] & 0x0F);
  }
}

void Sudoku::loadFromBank(uint8_t diff, uint16_t index) {
  if (diff > 3) diff = 0;
  uint16_t cnt = PUZ_BANK_COUNT[diff];
  if (cnt == 0) { diff = 0; cnt = PUZ_BANK_COUNT[0]; }
  index %= cnt;
  diff_ = diff; idx_ = index;

  // Copy the 82-byte entry out of PROGMEM, then split into puzzle + solution.
  uint8_t entry[SUDOKU_ENTRY_BYTES];
  memcpy_P(entry, PUZ_BANK[diff] + (uint32_t)index * SUDOKU_ENTRY_BYTES, SUDOKU_ENTRY_BYTES);
  unpack(entry,      g_);      // givens
  unpack(entry + 41, s_);      // solution
  memcpy(v_, g_, sizeof(v_));  // current grid starts at the clues
  memset(n_, 0, sizeof(n_));
  uhead_ = utail_ = 0;
}

void Sudoku::loadState(const uint8_t *given41, const uint8_t *cur41,
                       const uint8_t *sol41, const uint16_t *notes81,
                       uint8_t diff, uint16_t index) {
  unpack(given41, g_);
  unpack(cur41,   v_);
  unpack(sol41,   s_);
  memcpy(n_, notes81, sizeof(n_));
  diff_ = (diff > 3) ? 0 : diff;
  idx_  = index;
  uhead_ = utail_ = 0;
}

// --- undo ------------------------------------------------------------------
void Sudoku::pushUndo(uint8_t i) {
  undo_[uhead_] = { i, v_[i], n_[i] };
  uhead_ = (uhead_ + 1) % UMAX;
  if (uhead_ == utail_) utail_ = (utail_ + 1) % UMAX;   // ring full: drop oldest
}
bool Sudoku::undo() {
  if (uhead_ == utail_) return false;
  uhead_ = (uhead_ + UMAX - 1) % UMAX;
  const Snap &s = undo_[uhead_];
  v_[s.idx] = s.val;
  n_[s.idx] = s.note;
  return true;
}

// --- mutation --------------------------------------------------------------
bool Sudoku::setValue(uint8_t i, uint8_t d, bool autoErase) {
  if (isGiven(i) || d < 1 || d > 9) return false;
  if (v_[i] == d && n_[i] == 0) return false;
  pushUndo(i);
  v_[i] = d;
  n_[i] = 0;                                   // a firm value clears this cell's notes
  if (autoErase) {                             // and pencil-mark d from its peers
    uint8_t r = i / 9, c = i % 9;
    uint8_t br = (r / 3) * 3, bc = (c / 3) * 3;
    for (uint8_t k = 0; k < 9; k++) {
      n_[r * 9 + k]       &= ~(1u << d);
      n_[k * 9 + c]       &= ~(1u << d);
      n_[(br + k / 3) * 9 + (bc + k % 3)] &= ~(1u << d);
    }
  }
  return true;
}
bool Sudoku::clearCell(uint8_t i) {
  if (isGiven(i) || (v_[i] == 0 && n_[i] == 0)) return false;
  pushUndo(i);
  v_[i] = 0;
  n_[i] = 0;
  return true;
}
bool Sudoku::toggleNote(uint8_t i, uint8_t d) {
  if (isGiven(i) || d < 1 || d > 9) return false;
  pushUndo(i);
  v_[i] = 0;                                    // notes and a firm value are exclusive
  n_[i] ^= (1u << d);
  return true;
}
bool Sudoku::applyHint(uint8_t i) {
  if (isGiven(i) || v_[i] == s_[i]) return false;
  pushUndo(i);
  v_[i] = s_[i];
  n_[i] = 0;
  return true;
}

// --- queries ---------------------------------------------------------------
bool Sudoku::conflict(uint8_t i) const {
  uint8_t d = v_[i];
  if (d == 0) return false;
  uint8_t r = i / 9, c = i % 9;
  for (uint8_t k = 0; k < 9; k++) {
    uint8_t rc = r * 9 + k; if (rc != i && v_[rc] == d) return true;
    uint8_t cc = k * 9 + c; if (cc != i && v_[cc] == d) return true;
  }
  uint8_t br = (r / 3) * 3, bc = (c / 3) * 3;
  for (uint8_t rr = 0; rr < 3; rr++)
    for (uint8_t cc = 0; cc < 3; cc++) {
      uint8_t j = (br + rr) * 9 + (bc + cc);
      if (j != i && v_[j] == d) return true;
    }
  return false;
}
bool Sudoku::isComplete() const {
  for (uint8_t i = 0; i < CELLS; i++) if (v_[i] == 0) return false;
  return true;
}
bool Sudoku::isSolved() const {
  for (uint8_t i = 0; i < CELLS; i++) if (v_[i] != s_[i]) return false;
  return true;
}
uint8_t Sudoku::filledCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < CELLS; i++) if (v_[i]) n++;
  return n;
}
uint8_t Sudoku::countOf(uint8_t d) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < CELLS; i++) if (v_[i] == d) n++;
  return n;
}
int Sudoku::firstEmpty() const {
  for (uint8_t i = 0; i < CELLS; i++) if (v_[i] == 0) return i;
  return -1;
}
