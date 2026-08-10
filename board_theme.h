// board_theme.h
// Sudoku grid appearance.
//
// theme.h owns the shell palette (bg/fg/accent + 21 themes). This file owns the
// *grid* palette, exposed through the same small interface theme.h already
// expects (BoardPal, BOARD_PALS, boardPalFromTheme, BOARD_PAL_* ), so theme.h is
// reused almost unmodified — only Theme::board() indexes the table below.
//
// The grid palette is a separate setting from the UI theme (Settings → Grid
// Look), so a dark grid can sit under a light shell and vice-versa. The final
// entry, "Match Theme", is computed from the active UI theme instead of being a
// fixed table row — that is the one that harmonises the grid with the shell.
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

// ── Classic — the recognisable printed-sudoku look (light). ─────────────────
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

// ── Dark — dark cells, light digits (comfortable at night). ─────────────────
static const BoardPal BOARD_DARK = {
    /* paper     */ rgb565(0x12, 0x15, 0x1A),
    /* givenCell */ rgb565(0x1C, 0x21, 0x2A),
    /* lineThin  */ rgb565(0x2E, 0x35, 0x40),
    /* lineThick */ rgb565(0x8A, 0x95, 0xA4),
    /* sel       */ rgb565(0x24, 0x46, 0x70),
    /* peer      */ rgb565(0x1A, 0x22, 0x30),
    /* same      */ rgb565(0x2E, 0x5A, 0x86),
    /* givenText */ rgb565(0xE6, 0xEA, 0xF0),
    /* entryText */ rgb565(0x6A, 0xB0, 0xFF),
    /* noteText  */ rgb565(0x7A, 0x84, 0x94),
    /* hintText  */ rgb565(0x4E, 0xD0, 0x8A),
    /* badFill   */ rgb565(0x4A, 0x1E, 0x24),
    /* badText   */ rgb565(0xFF, 0x6B, 0x6B),
    /* name      */ "Dark",
};

// ── Grayscale — pure monochrome. ────────────────────────────────────────────
static const BoardPal BOARD_GRAY = {
    /* paper     */ rgb565(0xFF, 0xFF, 0xFF),
    /* givenCell */ rgb565(0xE2, 0xE2, 0xE2),
    /* lineThin  */ rgb565(0xBC, 0xBC, 0xBC),
    /* lineThick */ rgb565(0x2A, 0x2A, 0x2A),
    /* sel       */ rgb565(0xC4, 0xC4, 0xC4),
    /* peer      */ rgb565(0xEE, 0xEE, 0xEE),
    /* same      */ rgb565(0xAC, 0xAC, 0xAC),
    /* givenText */ rgb565(0x14, 0x14, 0x14),
    /* entryText */ rgb565(0x54, 0x54, 0x54),
    /* noteText  */ rgb565(0x94, 0x94, 0x94),
    /* hintText  */ rgb565(0x6E, 0x6E, 0x6E),
    /* badFill   */ rgb565(0xB0, 0xB0, 0xB0),
    /* badText   */ rgb565(0x00, 0x00, 0x00),
    /* name      */ "Grayscale",
};

// ── Midnight — deep indigo, jewel-tone accents. ─────────────────────────────
static const BoardPal BOARD_MIDNIGHT = {
    /* paper     */ rgb565(0x0E, 0x16, 0x30),
    /* givenCell */ rgb565(0x17, 0x21, 0x42),
    /* lineThin  */ rgb565(0x2A, 0x3A, 0x66),
    /* lineThick */ rgb565(0x7C, 0x8A, 0xC0),
    /* sel       */ rgb565(0x2C, 0x4A, 0x9E),
    /* peer      */ rgb565(0x18, 0x24, 0x48),
    /* same      */ rgb565(0x3A, 0x5A, 0xB8),
    /* givenText */ rgb565(0xDD, 0xE4, 0xFF),
    /* entryText */ rgb565(0x8F, 0xB4, 0xFF),
    /* noteText  */ rgb565(0x6E, 0x7C, 0xA8),
    /* hintText  */ rgb565(0x58, 0xD0, 0xA0),
    /* badFill   */ rgb565(0x4A, 0x1E, 0x3A),
    /* badText   */ rgb565(0xFF, 0x7B, 0xAE),
    /* name      */ "Midnight",
};

// ── Sepia — warm newsprint. ─────────────────────────────────────────────────
static const BoardPal BOARD_SEPIA = {
    /* paper     */ rgb565(0xF3, 0xE9, 0xD6),
    /* givenCell */ rgb565(0xE7, 0xD8, 0xBE),
    /* lineThin  */ rgb565(0xC9, 0xB4, 0x8F),
    /* lineThick */ rgb565(0x5A, 0x46, 0x32),
    /* sel       */ rgb565(0xE7, 0xC6, 0x8A),
    /* peer      */ rgb565(0xEE, 0xE2, 0xC9),
    /* same      */ rgb565(0xD8, 0xB4, 0x72),
    /* givenText */ rgb565(0x3A, 0x2E, 0x1E),
    /* entryText */ rgb565(0x9A, 0x5A, 0x1E),
    /* noteText  */ rgb565(0x8A, 0x76, 0x56),
    /* hintText  */ rgb565(0x4E, 0x7A, 0x3A),
    /* badFill   */ rgb565(0xE8, 0xB7, 0x9A),
    /* badText   */ rgb565(0xB0, 0x2A, 0x16),
    /* name      */ "Sepia",
};

// ── Forest — soft green paper. ──────────────────────────────────────────────
static const BoardPal BOARD_FOREST = {
    /* paper     */ rgb565(0xF1, 0xF7, 0xEE),
    /* givenCell */ rgb565(0xDD, 0xE9, 0xD4),
    /* lineThin  */ rgb565(0xB4, 0xC9, 0xA6),
    /* lineThick */ rgb565(0x2E, 0x4A, 0x28),
    /* sel       */ rgb565(0xB7, 0xE0, 0xA0),
    /* peer      */ rgb565(0xE6, 0xF1, 0xDE),
    /* same      */ rgb565(0x8F, 0xC9, 0x7A),
    /* givenText */ rgb565(0x1E, 0x2E, 0x19),
    /* entryText */ rgb565(0x2E, 0x7D, 0x32),
    /* noteText  */ rgb565(0x6E, 0x84, 0x6A),
    /* hintText  */ rgb565(0x1B, 0x7A, 0x46),
    /* badFill   */ rgb565(0xF6, 0xCD, 0xCD),
    /* badText   */ rgb565(0xC6, 0x28, 0x28),
    /* name      */ "Forest",
};

// ── Ocean — cool teal paper. ────────────────────────────────────────────────
static const BoardPal BOARD_OCEAN = {
    /* paper     */ rgb565(0xF0, 0xFA, 0xFB),
    /* givenCell */ rgb565(0xD3, 0xEC, 0xEF),
    /* lineThin  */ rgb565(0xA6, 0xCD, 0xD4),
    /* lineThick */ rgb565(0x0F, 0x3D, 0x47),
    /* sel       */ rgb565(0x9F, 0xE0, 0xEC),
    /* peer      */ rgb565(0xE0, 0xF4, 0xF6),
    /* same      */ rgb565(0x62, 0xC3, 0xD6),
    /* givenText */ rgb565(0x0E, 0x2A, 0x30),
    /* entryText */ rgb565(0x0E, 0x7C, 0x90),
    /* noteText  */ rgb565(0x5E, 0x80, 0x88),
    /* hintText  */ rgb565(0x0E, 0x8F, 0x6E),
    /* badFill   */ rgb565(0xF7, 0xCF, 0xCF),
    /* badText   */ rgb565(0xC6, 0x28, 0x28),
    /* name      */ "Ocean",
};

// ── Rose — warm pink paper. ─────────────────────────────────────────────────
static const BoardPal BOARD_ROSE = {
    /* paper     */ rgb565(0xFE, 0xF3, 0xF6),
    /* givenCell */ rgb565(0xF4, 0xD9, 0xE2),
    /* lineThin  */ rgb565(0xE0, 0xAE, 0xC0),
    /* lineThick */ rgb565(0x5A, 0x24, 0x36),
    /* sel       */ rgb565(0xF7, 0xC0, 0xD3),
    /* peer      */ rgb565(0xFB, 0xE4, 0xEC),
    /* same      */ rgb565(0xE8, 0x8C, 0xAD),
    /* givenText */ rgb565(0x3A, 0x1E, 0x28),
    /* entryText */ rgb565(0xC2, 0x18, 0x5B),
    /* noteText  */ rgb565(0x9A, 0x76, 0x84),
    /* hintText  */ rgb565(0x2E, 0x8B, 0x57),
    /* badFill   */ rgb565(0xF6, 0xC7, 0xC7),
    /* badText   */ rgb565(0xB0, 0x14, 0x2A),
    /* name      */ "Rose",
};

// ── Sunset — cream paper, amber accents. ────────────────────────────────────
static const BoardPal BOARD_SUNSET = {
    /* paper     */ rgb565(0xFF, 0xF6, 0xE9),
    /* givenCell */ rgb565(0xFB, 0xE6, 0xC8),
    /* lineThin  */ rgb565(0xE6, 0xC7, 0x9A),
    /* lineThick */ rgb565(0x6A, 0x3E, 0x1E),
    /* sel       */ rgb565(0xFF, 0xCF, 0x9A),
    /* peer      */ rgb565(0xFC, 0xEB, 0xD3),
    /* same      */ rgb565(0xF0, 0xA6, 0x5C),
    /* givenText */ rgb565(0x3A, 0x2A, 0x18),
    /* entryText */ rgb565(0xD2, 0x69, 0x1E),
    /* noteText  */ rgb565(0x9A, 0x82, 0x58),
    /* hintText  */ rgb565(0x4E, 0x7A, 0x3A),
    /* badFill   */ rgb565(0xF7, 0xC9, 0xB0),
    /* badText   */ rgb565(0xB8, 0x32, 0x20),
    /* name      */ "Sunset",
};

// ── Lavender — pale purple paper. ───────────────────────────────────────────
static const BoardPal BOARD_LAVENDER = {
    /* paper     */ rgb565(0xF6, 0xF2, 0xFC),
    /* givenCell */ rgb565(0xE4, 0xDA, 0xF3),
    /* lineThin  */ rgb565(0xC4, 0xB4, 0xE0),
    /* lineThick */ rgb565(0x3E, 0x2A, 0x5A),
    /* sel       */ rgb565(0xD0, 0xBC, 0xF2),
    /* peer      */ rgb565(0xED, 0xE5, 0xF8),
    /* same      */ rgb565(0xA9, 0x8C, 0xE0),
    /* givenText */ rgb565(0x28, 0x1E, 0x3A),
    /* entryText */ rgb565(0x6A, 0x3A, 0xD0),
    /* noteText  */ rgb565(0x86, 0x78, 0x9A),
    /* hintText  */ rgb565(0x2E, 0x8B, 0x57),
    /* badFill   */ rgb565(0xF3, 0xC9, 0xC9),
    /* badText   */ rgb565(0xB0, 0x14, 0x2A),
    /* name      */ "Lavender",
};

// ── Mint — fresh green-cyan paper. ──────────────────────────────────────────
static const BoardPal BOARD_MINT = {
    /* paper     */ rgb565(0xF0, 0xFB, 0xF6),
    /* givenCell */ rgb565(0xD2, 0xEE, 0xE0),
    /* lineThin  */ rgb565(0xA6, 0xD6, 0xC2),
    /* lineThick */ rgb565(0x17, 0x40, 0x2E),
    /* sel       */ rgb565(0xA2, 0xE8, 0xC8),
    /* peer      */ rgb565(0xE2, 0xF6, 0xEE),
    /* same      */ rgb565(0x66, 0xCC, 0xA2),
    /* givenText */ rgb565(0x12, 0x30, 0x2A),
    /* entryText */ rgb565(0x0E, 0x9E, 0x6E),
    /* noteText  */ rgb565(0x5E, 0x84, 0x7A),
    /* hintText  */ rgb565(0x0E, 0x8F, 0x5A),
    /* badFill   */ rgb565(0xF7, 0xCF, 0xCF),
    /* badText   */ rgb565(0xC6, 0x28, 0x28),
    /* name      */ "Mint",
};

// ── Slate — muted blue-grey, dark. ──────────────────────────────────────────
static const BoardPal BOARD_SLATE = {
    /* paper     */ rgb565(0x1B, 0x24, 0x30),
    /* givenCell */ rgb565(0x26, 0x31, 0x3E),
    /* lineThin  */ rgb565(0x38, 0x46, 0x56),
    /* lineThick */ rgb565(0x8C, 0xA0, 0xB4),
    /* sel       */ rgb565(0x34, 0x50, 0x70),
    /* peer      */ rgb565(0x21, 0x2E, 0x3C),
    /* same      */ rgb565(0x46, 0x68, 0x8E),
    /* givenText */ rgb565(0xE4, 0xEA, 0xF0),
    /* entryText */ rgb565(0x7C, 0xC0, 0xFF),
    /* noteText  */ rgb565(0x7A, 0x8A, 0x9A),
    /* hintText  */ rgb565(0x4E, 0xD0, 0xA0),
    /* badFill   */ rgb565(0x4A, 0x20, 0x28),
    /* badText   */ rgb565(0xFF, 0x7B, 0x7B),
    /* name      */ "Slate",
};

// ── Carbon — near-black, high-contrast dark. ────────────────────────────────
static const BoardPal BOARD_CARBON = {
    /* paper     */ rgb565(0x0A, 0x0A, 0x0A),
    /* givenCell */ rgb565(0x1A, 0x1A, 0x1A),
    /* lineThin  */ rgb565(0x30, 0x30, 0x30),
    /* lineThick */ rgb565(0xA0, 0xA0, 0xA0),
    /* sel       */ rgb565(0x2A, 0x4A, 0x6A),
    /* peer      */ rgb565(0x16, 0x16, 0x16),
    /* same      */ rgb565(0x2E, 0x5A, 0x86),
    /* givenText */ rgb565(0xFF, 0xFF, 0xFF),
    /* entryText */ rgb565(0x59, 0xB0, 0xFF),
    /* noteText  */ rgb565(0x80, 0x80, 0x80),
    /* hintText  */ rgb565(0x43, 0xD0, 0x8A),
    /* badFill   */ rgb565(0x40, 0x14, 0x14),
    /* badText   */ rgb565(0xFF, 0x5A, 0x5A),
    /* name      */ "Carbon",
};

// ── Contrast — accessibility: bold rules, strong hues, yellow selection. ─────
static const BoardPal BOARD_CONTRAST = {
    /* paper     */ rgb565(0xFF, 0xFF, 0xFF),
    /* givenCell */ rgb565(0xEC, 0xEC, 0xEC),
    /* lineThin  */ rgb565(0x80, 0x80, 0x80),
    /* lineThick */ rgb565(0x00, 0x00, 0x00),
    /* sel       */ rgb565(0xFF, 0xE2, 0x4D),
    /* peer      */ rgb565(0xDC, 0xE8, 0xFF),
    /* same      */ rgb565(0xFF, 0xB0, 0x20),
    /* givenText */ rgb565(0x00, 0x00, 0x00),
    /* entryText */ rgb565(0x00, 0x33, 0xCC),
    /* noteText  */ rgb565(0x50, 0x50, 0x50),
    /* hintText  */ rgb565(0x00, 0x7A, 0x2E),
    /* badFill   */ rgb565(0xFF, 0xB3, 0xB3),
    /* badText   */ rgb565(0xCC, 0x00, 0x00),
    /* name      */ "Contrast",
};

// ── Terminal — retro green phosphor on black. ───────────────────────────────
static const BoardPal BOARD_TERMINAL = {
    /* paper     */ rgb565(0x04, 0x10, 0x08),
    /* givenCell */ rgb565(0x0A, 0x1C, 0x10),
    /* lineThin  */ rgb565(0x10, 0x3A, 0x20),
    /* lineThick */ rgb565(0x1E, 0x9A, 0x50),
    /* sel       */ rgb565(0x12, 0x50, 0x2A),
    /* peer      */ rgb565(0x0A, 0x26, 0x14),
    /* same      */ rgb565(0x1E, 0x7A, 0x40),
    /* givenText */ rgb565(0x8C, 0xFF, 0xC0),
    /* entryText */ rgb565(0x40, 0xE0, 0x80),
    /* noteText  */ rgb565(0x2E, 0x7A, 0x4E),
    /* hintText  */ rgb565(0xC8, 0xFF, 0x80),
    /* badFill   */ rgb565(0x40, 0x14, 0x14),
    /* badText   */ rgb565(0xFF, 0x60, 0x60),
    /* name      */ "Terminal",
};

// Fixed palettes, in cycle order. "Match Theme" is appended as the last option
// but is computed (below), not stored here.
static const BoardPal BOARD_PALS[] = {
    BOARD_CLASSIC, BOARD_DARK,     BOARD_GRAY,   BOARD_MIDNIGHT, BOARD_SEPIA,
    BOARD_FOREST,  BOARD_OCEAN,    BOARD_ROSE,   BOARD_SUNSET,   BOARD_LAVENDER,
    BOARD_MINT,    BOARD_SLATE,    BOARD_CARBON, BOARD_CONTRAST, BOARD_TERMINAL,
};
static const uint8_t BOARD_PAL_FIXED = sizeof(BOARD_PALS) / sizeof(BOARD_PALS[0]);

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
static const uint8_t BOARD_PAL_COUNT   = BOARD_PAL_FIXED + 1;   // + Match Theme
static const uint8_t BOARD_PAL_THEME   = BOARD_PAL_COUNT - 1;   // last = computed
static const char *const BOARD_PAL_NAMES[BOARD_PAL_COUNT] = {
    "Classic", "Dark",  "Grayscale", "Midnight", "Sepia",
    "Forest",  "Ocean", "Rose",      "Sunset",   "Lavender",
    "Mint",    "Slate", "Carbon",    "Contrast", "Terminal",
    "Match Theme",
};
static_assert(sizeof(BOARD_PAL_NAMES) / sizeof(BOARD_PAL_NAMES[0]) == BOARD_PAL_COUNT,
              "BOARD_PAL_NAMES must have one entry per palette (fixed + Match Theme)");
