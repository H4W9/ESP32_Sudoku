// board_theme.h
// Sudoku grid appearance.
//
// theme.h owns the shell palette (bg/fg/accent + 21 themes). This file owns the
// *grid* palette, exposed through the same small interface theme.h already
// expects (BoardPal, BOARD_CLASSIC, boardPalFromTheme, BOARD_PAL_* ), so theme.h
// is reused unmodified. Two palettes ship:
//
//   Classic     — a fixed light "newspaper" board that reads the same under any
//                 UI theme: white cells, dark box rules, blue entries, red
//                 conflicts. This is the default.
//   Match Theme — neutrals (paper, lines) follow the active UI theme; the
//                 semantic colours (selection blue, conflict red, hint green)
//                 stay put because they carry meaning.
#pragma once
#include <Arduino.h>

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// One colour per role used by the board renderer.
struct BoardPal {
  uint16_t paper;       // empty cell fill
  uint16_t givenCell;   // fixed-clue cell fill
  uint16_t lineThin;    // rules between cells
  uint16_t lineThick;   // rules between 3x3 boxes + outer border
  uint16_t sel;         // selected cell
  uint16_t peer;        // cells sharing the selection's row / col / box
  uint16_t same;        // cells holding the same digit as the selection
  uint16_t givenText;   // clue digit
  uint16_t entryText;   // player-entered digit
  uint16_t noteText;    // pencil marks
  uint16_t hintText;    // a digit revealed by Hint
  uint16_t badFill;     // conflicting cell fill
  uint16_t badText;     // conflicting digit
  const char *name;
};

// Classic — the recognisable printed-sudoku look.
static const BoardPal BOARD_CLASSIC = {
    /* paper     */ rgb565(0xFF, 0xFF, 0xFF),
    /* givenCell */ rgb565(0xE7, 0xEC, 0xF2),
    /* lineThin  */ rgb565(0xB6, 0xBE, 0xC8),
    /* lineThick */ rgb565(0x27, 0x2E, 0x38),
    /* sel       */ rgb565(0xB6, 0xD4, 0xFF),
    /* peer      */ rgb565(0xE3, 0xEC, 0xF7),
    /* same      */ rgb565(0xA8, 0xC6, 0xF0),
    /* givenText */ rgb565(0x18, 0x1E, 0x26),
    /* entryText */ rgb565(0x14, 0x5A, 0xD6),
    /* noteText  */ rgb565(0x76, 0x80, 0x8C),
    /* hintText  */ rgb565(0x11, 0x94, 0x3E),
    /* badFill   */ rgb565(0xFB, 0xD4, 0xD4),
    /* badText   */ rgb565(0xCE, 0x1B, 0x1B),
    /* name      */ "Classic",
};

// Match Theme — neutrals from the shell theme, semantics preserved.
static inline BoardPal boardPalFromTheme(uint16_t bg, uint16_t fg, uint16_t dim, bool dark) {
  BoardPal p = BOARD_CLASSIC;
  p.paper     = bg;
  p.givenCell = dim;
  p.lineThin  = dark ? rgb565(0x3A, 0x40, 0x4A) : rgb565(0xB6, 0xBE, 0xC8);
  p.lineThick = fg;
  p.givenText = fg;
  p.entryText = dark ? rgb565(0x5A, 0xA8, 0xFF) : rgb565(0x14, 0x5A, 0xD6);
  p.noteText  = dim;
  p.sel       = dark ? rgb565(0x2A, 0x50, 0x86) : rgb565(0xB6, 0xD4, 0xFF);
  p.peer      = dark ? rgb565(0x1E, 0x2A, 0x3A) : rgb565(0xE3, 0xEC, 0xF7);
  p.same      = dark ? rgb565(0x38, 0x5E, 0x8E) : rgb565(0xA8, 0xC6, 0xF0);
  p.name      = "Match Theme";
  return p;
}

static const uint8_t BOARD_PAL_CLASSIC = 0;
static const uint8_t BOARD_PAL_THEME   = 1;
static const uint8_t BOARD_PAL_COUNT   = 2;
static const char *const BOARD_PAL_NAMES[BOARD_PAL_COUNT] = { "Classic", "Match Theme" };
