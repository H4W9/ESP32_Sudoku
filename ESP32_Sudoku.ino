/* ============================================================================
   ESP32 Sudoku — Pancake (ESP32-C5, ST7796 320x480, FT6336 capacitive touch)
                  Marauder V8 (ESP32-C5, ILI9341 240x320, XPT2046 resistive)
   ============================================================================
   A touch Sudoku that wears the H4W9 UI shell shared with ESP32_Scrabble
   (header with back button + battery/RAM status corner, momentum-scrolling list
   menus, chip settings rows, 21 colour themes). The Scrabble word engine is
   replaced by a Sudoku engine (sudoku.*) and a board view; the puzzle bank
   (puzzles.h) is embedded, so no SD card is required to play — the card, if
   present, only stores the in-progress game.

   Arduino IDE settings:
     Board            : ESP32C5 Dev Module
     Flash Size       : 8MB
     Partition Scheme : Custom  ->  partitions.csv in this folder
     Flash Frequency  : 80 MHz
     PSRAM            : Enabled     <-- required, the board view composites into
                                        a full-screen PSRAM sprite

   Requires the patched TFT_eSPI-ESP32-C5 library with User_Setup_Select.h set to
   the matching board's User_Setup_marauder_*.h.
   ============================================================================ */

#include "configs.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <ArduinoJson.h>

#include "ft6336.h"
#include "theme.h"
#include "board_theme.h"
#include "vlw.h"
#include "sudoku.h"
#include "puzzles.h"

// Picoware core (panel init, touch, view manager).
#include "src/Picoware/internal/boards.hpp"
#include "src/Picoware/internal/gui/draw.hpp"
#include "src/Picoware/internal/system/input.hpp"
#include "src/Picoware/internal/system/view.hpp"
#include "src/Picoware/internal/system/view_manager.hpp"
using namespace Picoware;

// ── Globals ────────────────────────────────────────────────────────────────
#ifdef HAS_C5_SD
SPIClass sharedSPI(SPI);
#endif

static ViewManager *vm    = nullptr;   // owns Draw (panel) + InputManager (touch)
static TFT_eSPI    *tft   = nullptr;   // raw panel (from Draw) for the shell screens
static TouchInput  *touch = nullptr;   // touch source (from InputManager)
static Theme        theme;             // colour theme + accent + font + brightness

// Theme-driven colours (macros so every use follows the current theme).
#define COL_BG     (theme.bg())
#define COL_FG     (theme.fg())
#define COL_ACCENT (theme.hdr())
#define COL_DIM    (theme.dim())
#define COL_SEL    (theme.sel())
static const uint16_t COL_OK = 0x07E0;   // status green (theme-independent)

static const int SCRW = TFT_WIDTH;
static const int SCRH = TFT_HEIGHT;

// Shell layout — matches H4W9 (header 28, nav 28, list rows 34/26).
static const int HDRH     = 28;
static const int NAVH     = 28;
#ifdef MARAUDER_V8
static const int ITEMH    = 26;
#else
static const int ITEMH    = 34;
#endif
static const int CONTENTY = HDRH;

static bool g_sdOk = false;            // SD mounted (auto-save target; else SPIFFS)

// ── Sudoku game state ───────────────────────────────────────────────────────
static Sudoku  g_sud;
static bool    g_gameActive = false;   // a game exists (Continue is meaningful)
static int     g_sel        = -1;      // selected cell 0..80, or -1
static bool    g_notesMode  = false;   // number taps write pencil marks
static bool    g_hintCell[81];         // which cells were filled by Hint (for colour)
static uint16_t g_mistakes  = 0;
static uint16_t g_hintsUsed = 0;
static uint32_t g_elapsedMs = 0;       // accumulated play time (paused segments)
static uint32_t g_startMs   = 0;       // start of the current running segment
static bool    g_running    = false;

// ── Sudoku settings (persisted /sudoku_cfg.json) ────────────────────────────
static bool    g_hlPeers    = true;    // shade the selection's row/col/box
static bool    g_hlSame     = true;    // shade cells holding the same digit
static bool    g_autoNotes  = true;    // placing a digit erases that pencil mark from peers
static bool    g_showTimer  = true;
static bool    g_hlMistakes = true;    // flag wrong entries in red (and count them)
static uint8_t g_mistakeLim = 0;       // 0 = off, else 3 or 5 strikes then game over

// ── Statistics (persisted /sudoku_stats.json) ───────────────────────────────
static uint16_t g_played[4] = {0,0,0,0};
static uint16_t g_won[4]    = {0,0,0,0};
static uint32_t g_best[4]   = {0,0,0,0};   // best solve time (seconds), 0 = none

// Kept below the type declarations above: Arduino inserts its auto-generated
// prototypes ahead of the FIRST function, so any function defined above these
// globals would push the prototypes above them and break the build.

// ════════════════════════════════════════════════════════════════════════════
//  Shell — transplanted from the H4W9 UI (Scrabble/FlipSocial), WiFi removed.
// ════════════════════════════════════════════════════════════════════════════

#ifndef HAS_CAP_TOUCH
// Resistive touch calibration (V8). The 5 uint16 blob is TFT_eSPI's own format.
static const char *TOUCH_CAL_FILE = "/sud_touch.dat";
static bool touchCalLoad(uint16_t *cal) {
  File f = SPIFFS.open(TOUCH_CAL_FILE, "r");
  if (!f) return false;
  bool ok = (f.read((uint8_t *)cal, sizeof(uint16_t) * 5) == sizeof(uint16_t) * 5);
  f.close();
  return ok;
}
static void touchCalSave(const uint16_t *cal) {
  File f = SPIFFS.open(TOUCH_CAL_FILE, "w");
  if (!f) return;
  f.write((const uint8_t *)cal, sizeof(uint16_t) * 5);
  f.close();
}
static void touchCalRun() {
  uint16_t cal[5];
  tft->fillScreen(TFT_BLACK);
  tft->setTextColor(TFT_WHITE, TFT_BLACK);
  tft->setTextDatum(MC_DATUM);
  tft->drawString("Touch Calibration", SCRW / 2, SCRH / 2 - 24, 4);
  tft->drawString("Tap each corner arrow", SCRW / 2, SCRH / 2 + 6, 2);
  tft->setTextDatum(TL_DATUM);
  delay(1500);
  tft->fillScreen(TFT_BLACK);
  tft->calibrateTouch(cal, TFT_MAGENTA, TFT_BLACK, 15);
  tft->setTouch(cal);
  touchCalSave(cal);
}
static void touchCalInit() {
  uint16_t cal[5];
  if (touchCalLoad(cal)) tft->setTouch(cal);
  else                   touchCalRun();
}
#endif // !HAS_CAP_TOUCH

static bool waitTap(uint16_t &x, uint16_t &y, uint32_t timeoutMs = 0) {
  uint32_t start = millis();
  bool wasDown = touch->isPressed();
  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    if (down && !wasDown) { x = touch->x(); y = touch->y(); return true; }
    wasDown = down;
    if (timeoutMs && (millis() - start) > timeoutMs) return false;
    delay(8);
    yield();
  }
}
static bool inRect(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh) {
  return (int)x >= rx && (int)x < rx + rw && (int)y >= ry && (int)y < ry + rh;
}

// Theme / brightness plumbing
static void applyBrightness() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, theme.duty());
#else
  ledcWrite(0, theme.duty());
#endif
}
static void applyThemeToViewManager() {
  if (!vm) return;
  vm->setBackgroundColor(theme.bg());
  vm->setForegroundColor(theme.fg());
  vm->setSelectedColor(theme.sel());
}

// Smooth-font drawing (VLW). Font numbers 1/2/4/6 map to 10/14/22/28 px.
static void drawStr(const String &s, int32_t x, int32_t y, uint8_t fontNum) {
  vlwLoad(*tft, fontNum, g_vlw_cur_tft);
  char buf[160];
  vlwPrivToUtf8(s.c_str(), buf, sizeof(buf));
  tft->drawString(buf, x, y);
}
static int16_t strWidth(const String &s, uint8_t fontNum) {
  return vlwTextWidth(vlwData(fontNum), s.c_str());
}
static void sprStr(TFT_eSprite &g, const uint8_t *&track,
                   const String &s, int32_t x, int32_t y, uint8_t fontNum) {
  vlwLoad(g, fontNum, track);
  char buf[160];
  vlwPrivToUtf8(s.c_str(), buf, sizeof(buf));
  g.drawString(buf, x, y);
}

// Status LED.
#ifdef HAS_ACT_LED
static bool g_actLedReady = false;
static void ledActArm() {
  if (g_actLedReady) return;
  pinMode(ACT_LED_PIN, OUTPUT);
  digitalWrite(ACT_LED_PIN, LOW);
  g_actLedReady = true;
}
static void ledActSet(bool on) {
  ledActArm();
  digitalWrite(ACT_LED_PIN, (on && theme.led_bright > 0) ? HIGH : LOW);
}
static inline void ledOff() { ledActSet(false); }
static inline void ledOn()  { ledActSet(true); }
static inline void ledBlinkOk(uint16_t ms = 150) { ledActSet(true); delay(ms); ledActSet(false); }
static inline void ledPreview() { ledActSet(true); }
#else
#ifdef RGB_BUILTIN
  #define PW_RGB_PIN RGB_BUILTIN
#else
  #define PW_RGB_PIN LED_BUILTIN
#endif
static inline void ledGap() { delayMicroseconds(300); }
static void ledRGB(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t s = theme.led_bright;
  ledGap();
  rgbLedWrite(PW_RGB_PIN, (uint8_t)((uint16_t)r * s / 20),
                          (uint8_t)((uint16_t)g * s / 20),
                          (uint8_t)((uint16_t)b * s / 20));
}
static inline void ledOff() { ledGap(); rgbLedWrite(PW_RGB_PIN, 0, 0, 0); }
static inline void ledOn()  { ledRGB(0, 80, 255); }
static inline void ledBlinkOk(uint16_t ms = 150) { ledRGB(0, 255, 0); delay(ms); ledOff(); }
static inline void ledPreview() { ledRGB(0, 160, 255); }
#endif // HAS_ACT_LED

// Battery fuel gauge (MAX17048, I2C 0x36, shared bus)
static int      g_battPct = -1;
static bool     g_battOk  = false;
static uint32_t g_battMs  = 0;
static void battInit() {
  Wire.beginTransmission(0x36);
  g_battOk = (Wire.endTransmission() == 0);
  Serial.println(g_battOk ? F("[Battery] MAX17048 OK") : F("[Battery] MAX17048 not found"));
}
static void battUpdate() {
  if (!g_battOk) return;
  Wire.beginTransmission(0x36);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0) { g_battOk = false; return; }
  Wire.requestFrom((uint8_t)0x36, (uint8_t)2);
  if (Wire.available() < 2) return;
  uint8_t hi = Wire.read();
  Wire.read();
  g_battPct = (hi > 100) ? 100 : hi;
  g_battMs  = millis();
}

// Rendering helpers
static String memShort(size_t bytes) {
  if (bytes >= 1024UL * 1024UL) {
    uint32_t tenths = (uint32_t)((bytes * 10ULL) / (1024ULL * 1024ULL));
    return String(tenths / 10) + "." + String(tenths % 10) + "M";
  }
  return String((uint32_t)(bytes / 1024)) + "k";
}
static const int MEM_W = 46;
static int memX(bool showBack) { return showBack ? 46 : 6; }
static void drawHeaderMem(bool showBack) {
  int x = memX(showBack);
  tft->fillRect(x, 0, MEM_W, HDRH, COL_ACCENT);
  tft->setTextColor(COL_FG, COL_ACCENT);
  tft->setTextDatum(ML_DATUM);
  drawStr(String("D:") + memShort(ESP.getFreeHeap()), x, HDRH / 2 - 6, 1);
  size_t ps = ESP.getFreePsram();
  if (ps) drawStr(String("P:") + memShort(ps), x, HDRH / 2 + 6, 1);
  tft->setTextDatum(TL_DATUM);
}
// Battery % painted into the header's top-right (WiFi icon removed).
static void drawHeaderStatus() {
  if (g_battOk && (g_battMs == 0 || millis() - g_battMs > 10000)) battUpdate();
  const int clearW = 62;
  tft->fillRect(SCRW - clearW, 0, clearW, HDRH, COL_ACCENT);
  if (g_battPct >= 0) {
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", g_battPct);
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MR_DATUM);
    drawStr(pct, SCRW - 6, HDRH / 2, 2);
  }
  tft->setTextDatum(TL_DATUM);
}

static void drawChevron(int bx, int by, int bw, int bh, bool right, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2;
  if (right) tft->fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, col);
  else       tft->fillTriangle(cx + 3, cy - 5, cx + 3, cy + 5, cx - 4, cy, col);
}
static void drawPlusMinus(int bx, int by, int bw, int bh, bool plus, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2, r = 6;
  tft->fillRect(cx - r, cy - 1, 2 * r, 2, col);
  if (plus) tft->fillRect(cx - 1, cy - r, 2, 2 * r, col);
}

static void drawHeader(const String &title, bool showBack) {
  tft->fillRect(0, 0, SCRW, HDRH, COL_ACCENT);
  if (showBack) {
    tft->fillRoundRect(2, 3, 40, 22, 4, COL_ACCENT);
    tft->drawRoundRect(2, 3, 40, 22, 4, theme.neon(3, COL_DIM));
    drawChevron(2, 3, 40, 22, false, COL_FG);
  }
  tft->setTextColor(COL_FG, COL_ACCENT);
  tft->setTextDatum(MC_DATUM);
  {
    int lo = memX(showBack) + MEM_W + 4, hi = SCRW - 62;
    drawStr(title, (lo + hi) / 2, HDRH / 2, 2);
  }
  drawHeaderMem(showBack);
  drawHeaderStatus();
  tft->setTextDatum(TL_DATUM);
}
static bool backTapped(uint16_t x, uint16_t y) {
  return (int)y < HDRH && (int)x < 48;
}

static void drawNav(const char *l, const char *m, const char *r) {
  int y = SCRH - NAVH, third = SCRW / 3, bh = NAVH - 10, by = y + 5, bw = third - 10;
  tft->fillRect(0, y, SCRW, NAVH, COL_BG);
  tft->drawFastHLine(0, y, SCRW, theme.edge());
  const char *L[3] = { l, m, r };
  for (int i = 0; i < 3; i++) {
    if (!L[i] || !L[i][0]) continue;
    int cx = i * third + third / 2, bx = cx - bw / 2;
    tft->fillRoundRect(bx, by, bw, bh, 5, COL_ACCENT);
    tft->drawRoundRect(bx, by, bw, bh, 5, theme.neon(i, COL_DIM));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
    drawStr(L[i], cx, by + bh / 2, 2);
  }
  tft->setTextDatum(TL_DATUM);
}
static int navHit(uint16_t x, uint16_t y) {
  if ((int)y < SCRH - NAVH) return -1;
  int c = (int)x / (SCRW / 3);
  return c > 2 ? 2 : c;
}

static void drawListRow(int y, const String &text, bool sel, bool arrow) {
  uint16_t bgc = sel ? COL_SEL : COL_BG;
  int seed = y / ITEMH;
  tft->fillRect(0, y, SCRW, ITEMH, bgc);
  tft->setTextColor(COL_FG, bgc);
  tft->setTextDatum(ML_DATUM);
  drawStr(text, 12, y + ITEMH / 2, 2);
  if (arrow) drawChevron(SCRW - 26, y, 16, ITEMH, true, theme.neon(seed, COL_DIM));
  tft->drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  tft->setTextDatum(TL_DATUM);
}

static const uint8_t *sprFont = nullptr;
static void drawRowSprite(TFT_eSprite &spr, int y, const String &text, bool arrow, int seed) {
  spr.fillRect(0, y, SCRW, ITEMH, COL_BG);
  spr.setTextColor(COL_FG, COL_BG);
  spr.setTextDatum(ML_DATUM);
  sprStr(spr, sprFont, text, 12, y + ITEMH / 2, 2);
  if (arrow) {
    int cx = SCRW - 26 + 8, cy = y + ITEMH / 2;
    spr.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, theme.neon(seed, COL_DIM));
  }
  spr.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  spr.setTextDatum(TL_DATUM);
}
static void sprScrollBar(TFT_eSprite &spr, int viewH, int total, float scroll) {
  if (total <= viewH) return;
  const int bw = 4, bx = SCRW - bw - 1;
  spr.fillRect(bx, 0, bw, viewH, theme.edge());
  int thumbH = viewH * viewH / total; if (thumbH < 14) thumbH = 14;
  int maxS = total - viewH;
  int thumbY = (maxS > 0) ? (int)((scroll / (float)maxS) * (viewH - thumbH)) : 0;
  spr.fillRect(bx, thumbY, bw, thumbH, theme.neon(thumbY / 12, COL_DIM));
}

static const int SL_BACK = -1, SL_F0 = -2, SL_F1 = -3, SL_F2 = -4;
static int scrollList(const String &title, String *rows, int n, bool arrow,
                      const char *fL = nullptr, const char *fM = nullptr, const char *fR = nullptr) {
  bool hasFooter = (fL && fL[0]) || (fM && fM[0]) || (fR && fR[0]);
  const int CY = CONTENTY;
  const int CH = SCRH - CONTENTY - (hasFooter ? NAVH : 0);
  int total = n * ITEMH;
  tft->fillScreen(COL_BG);
  drawHeader(title, true);
  if (hasFooter) drawNav(fL ? fL : "", fM ? fM : "", fR ? fR : "");

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);
  sprFont = nullptr;

  float scroll = 0, fling = 0;
  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;

  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (haveSpr) {
      spr.fillSprite(COL_BG);
      for (int i = 0; i < n; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawRowSprite(spr, y, rows[i], arrow, i);
      }
      sprScrollBar(spr, CH, total, scroll);
      spr.pushSprite(0, CY);
    } else {
      tft->fillRect(0, CY, SCRW, CH, COL_BG);
      for (int i = 0; i < n; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawListRow(CY + y, rows[i], false, arrow);
      }
    }
  };
  render();

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t ty = touch->y(), tx = touch->x();
    uint32_t now = millis();
    bool need = false;

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0; lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      scroll = pScroll + dy;
      uint32_t dt = now - lastT;
      if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
      need = true;
    } else if (!down && wasDown) {
      if (!moved) {
        if (backTapped(pX, pY)) { if (haveSpr) spr.deleteSprite(); return SL_BACK; }
        if (hasFooter && (int)pY >= SCRH - NAVH) {
          int nh = navHit(pX, pY);
          if (haveSpr) spr.deleteSprite();
          return nh == 0 ? SL_F0 : nh == 2 ? SL_F2 : SL_F1;
        }
        if ((int)pY >= CY && (int)pY < CY + CH) {
          int idx = ((int)pY - CY + (int)scroll) / ITEMH;
          if (idx >= 0 && idx < n) { if (haveSpr) spr.deleteSprite(); return idx; }
        }
      } else {
        fling = vel;
      }
      need = true;
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      need = true;
    } else {
      fling = 0;
    }
    wasDown = down;
    if (need) render();
    delay(12);
  }
}

static uint16_t contrastOn(uint16_t c) {
  int r = ((c >> 11) & 0x1F) * 255 / 31;
  int g = ((c >> 5) & 0x3F) * 255 / 63;
  int b = (c & 0x1F) * 255 / 31;
  return ((r * 299 + g * 587 + b * 114) / 1000 > 140) ? TFT_BLACK : TFT_WHITE;
}
static void statusLine(const char *msg, uint16_t col = 0xFFFF) {
  tft->fillRect(0, SCRH - 26, SCRW, 26, COL_BG);
  tft->setTextColor(col == 0xFFFF ? COL_FG : col, COL_BG);
  tft->setTextDatum(ML_DATUM);
  drawStr(msg, 8, SCRH - 13, 2);
  tft->setTextDatum(TL_DATUM);
}

// Settings chip rows: label + [<] value [>] (or [-] value [+])
static const int CHIP_W = 28, CHIP_H = 22;
static void chipGeom(const String &val, int &fwd_bx, int &bwd_bx, int &vx) {
  fwd_bx = SCRW - 8 - CHIP_W;
  int vw = strWidth(val.c_str(), 2);
  vx     = fwd_bx - 4 - vw;
  bwd_bx = vx - 4 - CHIP_W;
}
static int chipHit(int y, const String &val, uint16_t x, uint16_t ty) {
  int by = y + (ITEMH - CHIP_H) / 2, fwd_bx, bwd_bx, vx;
  chipGeom(val, fwd_bx, bwd_bx, vx);
  if ((int)ty < by || (int)ty >= by + CHIP_H) return -1;
  if ((int)x >= fwd_bx && (int)x < fwd_bx + CHIP_W) return 1;
  if ((int)x >= bwd_bx && (int)x < bwd_bx + CHIP_W) return 0;
  return -1;
}
static void sprChipRow(TFT_eSprite &spr, const uint8_t *&track, int y,
                       const String &label, const String &val, bool pm,
                       bool sel, uint16_t valcol) {
  uint16_t rbg = sel ? COL_SEL : COL_BG;
  int seed = y / ITEMH;
  spr.fillRect(0, y, SCRW, ITEMH, rbg);
  spr.setTextColor(COL_FG, rbg);
  spr.setTextDatum(ML_DATUM);
  sprStr(spr, track, label, 12, y + ITEMH / 2, 2);
  int by = y + (ITEMH - CHIP_H) / 2, fwd_bx, bwd_bx, vx;
  chipGeom(val, fwd_bx, bwd_bx, vx);
  spr.fillRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  spr.drawRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed, COL_DIM));
  spr.fillRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  spr.drawRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed + 4, COL_DIM));
  int fcx = fwd_bx + CHIP_W / 2, bcx = bwd_bx + CHIP_W / 2, cy = by + CHIP_H / 2;
  if (pm) {
    const int r = 6;
    spr.fillRect(bcx - r, cy - 1, 2 * r, 2, COL_FG);
    spr.fillRect(fcx - r, cy - 1, 2 * r, 2, COL_FG);
    spr.fillRect(fcx - 1, cy - r, 2, 2 * r, COL_FG);
  } else {
    spr.fillTriangle(bcx + 3, cy - 5, bcx + 3, cy + 5, bcx - 4, cy, COL_FG);
    spr.fillTriangle(fcx - 3, cy - 5, fcx - 3, cy + 5, fcx + 4, cy, COL_FG);
  }
  spr.setTextColor(valcol ? valcol : COL_FG, rbg);
  spr.setTextDatum(ML_DATUM);
  sprStr(spr, track, val, vx, y + ITEMH / 2, 2);
  spr.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  spr.setTextDatum(TL_DATUM);
}
static void sprInfoRow(TFT_eSprite &spr, const uint8_t *&track, int y,
                       const String &label, const String &val, bool sel) {
  uint16_t rbg = sel ? COL_SEL : COL_BG;
  int seed = y / ITEMH;
  spr.fillRect(0, y, SCRW, ITEMH, rbg);
  spr.setTextColor(COL_FG, rbg);
  spr.setTextDatum(ML_DATUM);
  sprStr(spr, track, label, 12, y + ITEMH / 2, 2);
  if (val.length()) {
    spr.setTextColor(COL_DIM, rbg);
    spr.setTextDatum(MR_DATUM);
    sprStr(spr, track, val, SCRW - 32, y + ITEMH / 2, 2);
    spr.setTextDatum(ML_DATUM);
  }
  int cx = SCRW - 26 + 8, cy = y + ITEMH / 2;
  spr.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, theme.neon(seed, COL_DIM));
  spr.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  spr.setTextDatum(TL_DATUM);
}

// Centred message screen with a Back header: headline + optional wrapped detail.
static void msgScreen(const char *title, const String &a, const String &b, uint16_t col) {
  tft->fillScreen(COL_BG);
  drawHeader(title, true);
  tft->setTextColor(col, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr(a, SCRW / 2, SCRH / 2 - 20, 4);
  if (b.length()) {
    tft->setTextColor(COL_DIM, COL_BG);
    int y = SCRH / 2 + 10, maxW = SCRW - 24;
    String line = "", rest = b;
    while (rest.length() && y < SCRH - 20) {
      int sp = rest.indexOf(' ');
      String word = (sp < 0) ? rest : rest.substring(0, sp);
      String cand = line.length() ? line + " " + word : word;
      if (strWidth(cand.c_str(), 2) <= maxW) { line = cand; }
      else { drawStr(line, SCRW / 2, y, 2); y += 20; line = word; }
      rest = (sp < 0) ? "" : rest.substring(sp + 1);
    }
    if (line.length() && y < SCRH - 20) drawStr(line, SCRW / 2, y, 2);
  }
  tft->setTextDatum(TL_DATUM);
  uint16_t x, y2; waitTap(x, y2);
}

// ── Config + stats persistence ──────────────────────────────────────────────
static void cfgLoad() {
  File f = SPIFFS.open("/sudoku_cfg.json", FILE_READ);
  if (!f) return;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return;
  if (!d["peers"].isNull())   g_hlPeers   = d["peers"].as<bool>();
  if (!d["same"].isNull())    g_hlSame    = d["same"].as<bool>();
  if (!d["autonote"].isNull())g_autoNotes = d["autonote"].as<bool>();
  if (!d["timer"].isNull())   g_showTimer = d["timer"].as<bool>();
  if (!d["mistakes"].isNull())g_hlMistakes= d["mistakes"].as<bool>();
  if (!d["mlimit"].isNull())  g_mistakeLim= d["mlimit"].as<uint8_t>();
  if (g_mistakeLim != 0 && g_mistakeLim != 3 && g_mistakeLim != 5) g_mistakeLim = 0;
}
static void cfgSave() {
  JsonDocument d;
  d["peers"]    = g_hlPeers;
  d["same"]     = g_hlSame;
  d["autonote"] = g_autoNotes;
  d["timer"]    = g_showTimer;
  d["mistakes"] = g_hlMistakes;
  d["mlimit"]   = g_mistakeLim;
  File w = SPIFFS.open("/sudoku_cfg.json", FILE_WRITE);
  if (!w) return;
  serializeJson(d, w);
  w.close();
}
static void statsLoad() {
  File f = SPIFFS.open("/sudoku_stats.json", FILE_READ);
  if (!f) return;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return;
  for (int i = 0; i < 4; i++) {
    g_played[i] = d["p"][i] | 0;
    g_won[i]    = d["w"][i] | 0;
    g_best[i]   = d["b"][i] | 0;
  }
}
static void statsSave() {
  JsonDocument d;
  JsonArray p = d["p"].to<JsonArray>();
  JsonArray w = d["w"].to<JsonArray>();
  JsonArray b = d["b"].to<JsonArray>();
  for (int i = 0; i < 4; i++) { p.add(g_played[i]); w.add(g_won[i]); b.add(g_best[i]); }
  File wf = SPIFFS.open("/sudoku_stats.json", FILE_WRITE);
  if (!wf) return;
  serializeJson(d, wf);
  wf.close();
}

// ── Game save/load (SD if present, else SPIFFS) ─────────────────────────────
// Binary layout, little-endian:
//   magic "SUD1"(4) ver(1) diff(1) idx(2) elapsedSec(4) mistakes(2) hints(2)
//   sel(2, 0xFFFF = none) given(41) cur(41) sol(41) notes(81*2) hintFlags(81)
static const char *SAVE_SPIFFS = "/sud_auto.sav";
static String savePath() { return g_sdOk ? String(SAVE_DIR "/auto.sav") : String(SAVE_SPIFFS); }

static void gameSave() {
  if (!g_gameActive) return;
  File f;
  if (g_sdOk) { SD.mkdir(SUDOKU_DIR); SD.mkdir(SAVE_DIR); f = SD.open(SAVE_DIR "/auto.sav", FILE_WRITE); }
  else        f = SPIFFS.open(SAVE_SPIFFS, FILE_WRITE);
  if (!f) return;
  uint32_t elapsed = g_elapsedMs / 1000 + (g_running ? (millis() - g_startMs) / 1000 : 0);
  uint8_t g41[41], c41[41], s41[41]; uint16_t notes[81];
  g_sud.packGiven(g41); g_sud.packValues(c41); g_sud.packSolution(s41); g_sud.copyNotes(notes);
  uint8_t hdr[19];
  memcpy(hdr, "SUD1", 4); hdr[4] = 1;
  hdr[5] = g_sud.difficulty();
  uint16_t idx = g_sud.bankIndex();      hdr[6] = idx & 0xFF; hdr[7] = idx >> 8;
  hdr[8]  = elapsed & 0xFF; hdr[9] = (elapsed >> 8) & 0xFF; hdr[10] = (elapsed >> 16) & 0xFF; hdr[11] = (elapsed >> 24) & 0xFF;
  hdr[12] = g_mistakes & 0xFF; hdr[13] = g_mistakes >> 8;
  hdr[14] = g_hintsUsed & 0xFF; hdr[15] = g_hintsUsed >> 8;
  uint16_t sel = (g_sel < 0) ? 0xFFFF : (uint16_t)g_sel; hdr[16] = sel & 0xFF; hdr[17] = sel >> 8;
  hdr[18] = g_notesMode ? 1 : 0;
  f.write(hdr, 19);
  f.write(g41, 41); f.write(c41, 41); f.write(s41, 41);
  f.write((uint8_t *)notes, 81 * 2);
  uint8_t hf[81]; for (int i = 0; i < 81; i++) hf[i] = g_hintCell[i] ? 1 : 0;
  f.write(hf, 81);
  f.close();
}
static bool gameLoad() {
  File f = g_sdOk ? SD.open(SAVE_DIR "/auto.sav", FILE_READ) : SPIFFS.open(SAVE_SPIFFS, FILE_READ);
  if (!f) return false;
  uint8_t hdr[19];
  if (f.read(hdr, 19) != 19 || memcmp(hdr, "SUD1", 4) != 0) { f.close(); return false; }
  uint8_t g41[41], c41[41], s41[41]; uint16_t notes[81]; uint8_t hf[81];
  if (f.read(g41, 41) != 41 || f.read(c41, 41) != 41 || f.read(s41, 41) != 41 ||
      f.read((uint8_t *)notes, 81 * 2) != 81 * 2 || f.read(hf, 81) != 81) { f.close(); return false; }
  f.close();
  uint8_t diff = hdr[5];
  uint16_t idx = hdr[6] | (hdr[7] << 8);
  g_sud.loadState(g41, c41, s41, notes, diff, idx);
  uint32_t elapsed = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
  g_elapsedMs = elapsed * 1000UL;
  g_mistakes  = hdr[12] | (hdr[13] << 8);
  g_hintsUsed = hdr[14] | (hdr[15] << 8);
  uint16_t sel = hdr[16] | (hdr[17] << 8);
  g_sel = (sel == 0xFFFF) ? -1 : (int)sel;
  g_notesMode = hdr[18] != 0;
  for (int i = 0; i < 81; i++) g_hintCell[i] = hf[i] != 0;
  g_running = false;
  g_gameActive = true;
  return true;
}
static void gameDeleteSave() {
  if (g_sdOk) SD.remove(SAVE_DIR "/auto.sav");
  else        SPIFFS.remove(SAVE_SPIFFS);
}

// ════════════════════════════════════════════════════════════════════════════
//  Settings + About + Statistics
// ════════════════════════════════════════════════════════════════════════════
enum {
  SET_THEME = 0, SET_ACCENT, SET_FONT, SET_GRID,
  SET_PEERS, SET_SAME, SET_AUTONOTE, SET_TIMER, SET_MISTAKES, SET_MLIMIT,
  SET_BRIGHT, SET_LED, SET_ABOUT,
#ifndef HAS_CAP_TOUCH
  SET_CAL,
#endif
  SET_N
};
static const int SET_CHIP_MAX = SET_LED;   // rows 0..SET_LED draw chips

static String mlimitVal() { return g_mistakeLim == 0 ? String("Off") : String(g_mistakeLim); }
static String onOff(bool b) { return b ? String("On") : String("Off"); }

static String setChipVal(int row) {
  switch (row) {
    case SET_THEME:    return theme.themeName();
    case SET_ACCENT:   return theme.accentName();
    case SET_FONT:     return theme.fontColName();
    case SET_GRID:     return theme.boardPalName();
    case SET_PEERS:    return onOff(g_hlPeers);
    case SET_SAME:     return onOff(g_hlSame);
    case SET_AUTONOTE: return onOff(g_autoNotes);
    case SET_TIMER:    return onOff(g_showTimer);
    case SET_MISTAKES: return onOff(g_hlMistakes);
    case SET_MLIMIT:   return mlimitVal();
    case SET_BRIGHT:   return String(theme.bright + 1) + "/20";
    case SET_LED:      return String(theme.led_bright) + "/20";
  }
  return "";
}
static void sprSettingRow(TFT_eSprite &spr, const uint8_t *&sf, int row, int sel, int y) {
  bool s = (row == sel);
  switch (row) {
    case SET_THEME:    sprChipRow(spr, sf, y, "Theme",       setChipVal(row), false, s, 0); break;
    case SET_ACCENT:   sprChipRow(spr, sf, y, "Accent",      setChipVal(row), false, s, 0); break;
    case SET_FONT:     sprChipRow(spr, sf, y, "Font Color",  setChipVal(row), false, s, theme.fontColPreview()); break;
    case SET_GRID:     sprInfoRow(spr, sf, y, "Grid Look",   setChipVal(row), s); break;
    case SET_PEERS:    sprChipRow(spr, sf, y, "Highlight Peers", setChipVal(row), false, s, 0); break;
    case SET_SAME:     sprChipRow(spr, sf, y, "Highlight Same",  setChipVal(row), false, s, 0); break;
    case SET_AUTONOTE: sprChipRow(spr, sf, y, "Auto-Erase Notes", setChipVal(row), false, s, 0); break;
    case SET_TIMER:    sprChipRow(spr, sf, y, "Show Timer",   setChipVal(row), false, s, 0); break;
    case SET_MISTAKES: sprChipRow(spr, sf, y, "Show Mistakes", setChipVal(row), false, s, 0); break;
    case SET_MLIMIT:   sprChipRow(spr, sf, y, "Mistake Limit", setChipVal(row), true, s, 0); break;
    case SET_BRIGHT:   sprChipRow(spr, sf, y, "Brightness",   setChipVal(row), true,  s, 0); break;
    case SET_LED:      sprChipRow(spr, sf, y, "LED",          setChipVal(row), true,  s, 0); break;
    case SET_ABOUT:    sprInfoRow(spr, sf, y, "About",        "", s); break;
#ifndef HAS_CAP_TOUCH
    case SET_CAL:      sprInfoRow(spr, sf, y, "Calibrate Touch", "", s); break;
#endif
  }
}

static void aboutScreen() {
  tft->fillScreen(COL_BG);
  drawHeader("About", true);
#ifdef MARAUDER_V8
  const int dName = 26, dSub = 17, dAuth = 18, dRule = 6, dRow = 17, dGap = 2, dRule2 = 6, dCred = 16;
  const int valX = 92;
#else
  const int dName = 32, dSub = 22, dAuth = 24, dRule = 10, dRow = 21, dGap = 4, dRule2 = 8, dCred = 20;
  const int valX = 120;
#endif
  int cx = SCRW / 2, y = CONTENTY + 12;
  tft->setTextColor(COL_FG, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr(FW_NAME, cx, y, 4); y += dName;
  drawStr(String("Version ") + FW_VERSION, cx, y, 2); y += dSub;
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr("UI by " FW_AUTHOR, cx, y, 2); y += dAuth;
  tft->drawFastHLine(16, y, SCRW - 32, theme.neon(1, theme.edge())); y += dRule;

  tft->setTextDatum(TL_DATUM);
  auto row = [&](const char *label, const String &value) {
    tft->setTextColor(COL_DIM, COL_BG); drawStr(label, 16, y, 2);
    tft->setTextColor(COL_FG, COL_BG);  drawStr(value, valX, y, 2);
    y += dRow;
  };
  row("Board",   BOARD_NAME);
  row("MCU",     BOARD_MCU);
  row("Display", BOARD_DISPLAY);
  row("Touch",   BOARD_TOUCH);
  {
    size_t ps = ESP.getPsramSize();
    if (ps >= 1024 * 1024)  row("PSRAM", String((unsigned)((ps + 512 * 1024) / (1024 * 1024))) + " MB");
    else if (ps > 0)        row("PSRAM", String((unsigned)(ps / 1024)) + " KB");
    else                    row("PSRAM", "None");
  }
  {
    uint16_t total = 0;
    for (int i = 0; i < 4; i++) total += PUZ_BANK_COUNT[i];
    row("Puzzles", String(total));
  }
  row("Built",   __DATE__);
  row("Commit",  FW_COMMIT);

  y += dGap;
  tft->drawFastHLine(16, y, SCRW - 32, theme.neon(2, theme.edge())); y += dRule2;
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr("Classic 9x9 Sudoku", 16, y, 2); y += dCred;
  drawStr("Puzzles: bundled dataset", 16, y, 2);

  statusLine("Tap to go back.", COL_DIM);
  uint16_t x, ty; waitTap(x, ty);
}

// ── Grid Look picker — a full-screen gallery of palette swatches ────────────
// Resolve a palette by cycle index (the last index is the theme-derived one).
static BoardPal palByIndex(uint8_t i) {
  if (i >= BOARD_PAL_THEME) return boardPalFromTheme(theme.bg(), theme.fg(), theme.dim(), theme.dark());
  return BOARD_PALS[i % BOARD_PAL_FIXED];
}
// One swatch: a mini 3x3 board in the palette (given, entry, conflict digits +
// selection/peer/same tints) with its name below and a ring when it is current.
static void drawSwatchSpr(TFT_eSprite &g, const uint8_t *&tk, int sx, int sy,
                          int slotW, int slotH, uint8_t idx, bool selected,
                          int labelH, uint8_t labelF) {
  BoardPal bp = palByIndex(idx);
  const int pad = 5;
  int avail = min(slotW - 2 * pad, slotH - labelH - 2 * pad);
  if (avail < 12) avail = 12;
  int cell = avail / 3, S = cell * 3;
  int ssx = sx + (slotW - S) / 2, ssy = sy + pad;

  g.fillRect(ssx, ssy, S, S, bp.paper);
  g.fillRect(ssx + cell,     ssy + cell,     cell, cell, bp.sel);      // centre = selected
  g.fillRect(ssx + 2 * cell, ssy,            cell, cell, bp.same);     // top-right = same digit
  g.fillRect(ssx,            ssy + 2 * cell, cell, cell, bp.peer);     // bottom-left = peer
  g.fillRect(ssx + 2 * cell, ssy + 2 * cell, cell, cell, bp.badFill);  // bottom-right = conflict
  for (int k = 1; k < 3; k++) {
    g.drawFastVLine(ssx + k * cell, ssy, S, bp.lineThin);
    g.drawFastHLine(ssx, ssy + k * cell, S, bp.lineThin);
  }
  g.drawRect(ssx, ssy, S, S, bp.lineThick);
  g.drawRect(ssx - 1, ssy - 1, S + 2, S + 2, bp.lineThick);

  uint8_t df = (cell >= 18) ? 2 : 1;
  g.setTextDatum(MC_DATUM);
  g.setTextColor(bp.givenText); sprStr(g, tk, "5", ssx + cell / 2,           ssy + cell / 2,           df);
  g.setTextColor(bp.entryText); sprStr(g, tk, "3", ssx + cell + cell / 2,     ssy + cell + cell / 2,     df);
  g.setTextColor(bp.badText);   sprStr(g, tk, "7", ssx + 2 * cell + cell / 2, ssy + 2 * cell + cell / 2, df);

  g.setTextColor(selected ? theme.sel() : COL_FG, COL_BG);
  sprStr(g, tk, BOARD_PAL_NAMES[idx], sx + slotW / 2, ssy + S + labelH / 2 + 1, labelF);
  g.setTextDatum(TL_DATUM);
  if (selected) {
    g.drawRoundRect(sx + 2, sy + 1, slotW - 4, slotH - 2, 6, theme.sel());
    g.drawRoundRect(sx + 3, sy + 2, slotW - 6, slotH - 4, 6, theme.sel());
  }
}
static void gridPickerScreen() {
#ifdef MARAUDER_V8
  const int COLS = 2, SLOTH = 94, LABELH = 13; const uint8_t LABELF = 1;
#else
  const int COLS = 3, SLOTH = 86, LABELH = 16; const uint8_t LABELF = 2;
#endif
  const int NPAL  = BOARD_PAL_COUNT;
  const int slotW = SCRW / COLS;
  const int nrows = (NPAL + COLS - 1) / COLS;
  const int CY = CONTENTY, CH = SCRH - CONTENTY;
  const int total = nrows * SLOTH;

  tft->fillScreen(COL_BG);
  drawHeader("Grid Look", true);

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  if (!spr.createSprite(SCRW, CH)) {
    msgScreen("Grid Look", "Out of memory", "The palette gallery needs PSRAM.", TFT_RED);
    return;
  }
  const uint8_t *tk = nullptr;
  float scroll = 0, fling = 0;

  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    tk = nullptr;
    spr.fillSprite(COL_BG);
    for (int i = 0; i < NPAL; i++) {
      int y = (i / COLS) * SLOTH - (int)scroll, x = (i % COLS) * slotW;
      if (y + SLOTH < 0 || y > CH) continue;
      drawSwatchSpr(spr, tk, x, y, slotW, SLOTH, (uint8_t)i, i == theme.board_pal, LABELH, LABELF);
    }
    sprScrollBar(spr, CH, total, scroll);
    spr.pushSprite(0, CY);
  };
  render();

  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;
  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t tx = touch->x(), ty = touch->y();
    uint32_t now = millis();
    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0; lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      scroll = pScroll + dy;
      uint32_t dt = now - lastT;
      if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
      render();
    } else if (!down && wasDown) {
      if (!moved) {
        if (backTapped(pX, pY)) { spr.deleteSprite(); return; }
        if ((int)pY >= CY) {
          int r = ((int)pY - CY + (int)scroll) / SLOTH, c = (int)pX / slotW;
          int idx = r * COLS + c;
          if (c >= 0 && c < COLS && idx >= 0 && idx < NPAL && idx != theme.board_pal) {
            theme.board_pal = (uint8_t)idx; theme.save();
            render();
          }
        }
      } else {
        fling = vel;
      }
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f; fling *= 0.95f; render();
    } else {
      fling = 0;
    }
    wasDown = down;
    delay(10);
  }
}

static void settingsFlow() {
  int sel = -1;
  const int CY = CONTENTY;
  const int CH = SCRH - CONTENTY;
  const int total = SET_N * ITEMH;

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);
  const uint8_t *sf = nullptr;
  float scroll = 0, fling = 0;

  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (!haveSpr) {
      tft->setViewport(0, CY, SCRW, CH);
      tft->fillRect(0, 0, SCRW, CH, COL_BG);
      tft->resetViewport();
      return;
    }
    sf = nullptr;
    spr.fillSprite(COL_BG);
    for (int i = 0; i < SET_N; i++) {
      int y = i * ITEMH - (int)scroll;
      if (y + ITEMH < 0 || y > CH) continue;
      sprSettingRow(spr, sf, i, sel, y);
    }
    sprScrollBar(spr, CH, total, scroll);
    spr.pushSprite(0, CY);
  };
  auto full    = [&]() { tft->fillScreen(COL_BG); drawHeader("Settings", true); render(); };
  auto recolor = [&]() { drawHeader("Settings", true); render(); };
  full();

  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t tx = touch->x(), ty = touch->y();
    uint32_t now = millis();

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0; lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      if (moved) {
        scroll = pScroll + dy;
        uint32_t dt = now - lastT;
        if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
        render();
      }
    } else if (!down && wasDown) {
      if (moved) { fling = vel; wasDown = down; delay(10); continue; }
      if (backTapped(pX, pY)) { if (haveSpr) spr.deleteSprite(); ledOff(); cfgSave(); return; }
      if ((int)pY < CY) { wasDown = down; delay(10); continue; }

      int absY = (int)pY - CY + (int)scroll;
      int row = absY / ITEMH;
      if (row < 0 || row >= SET_N) { wasDown = down; delay(10); continue; }
      int old = sel; sel = row;
      if (old != row) render();
      if (row != SET_LED) ledOff();

      int rowTop = CY + row * ITEMH - (int)scroll;
      int h = (row <= SET_CHIP_MAX) ? chipHit(rowTop, setChipVal(row), pX, pY) : -1;

      switch (row) {
        case SET_THEME:  if (h >= 0) { theme.cycleTheme(h); theme.save(); applyThemeToViewManager(); full(); } break;
        case SET_ACCENT: if (h >= 0) { theme.cycleAccent(h); theme.save(); applyThemeToViewManager(); render(); } break;
        case SET_FONT:   if (h >= 0) { theme.cycleFontCol(h); theme.save(); applyThemeToViewManager(); recolor(); } break;
        case SET_GRID:   gridPickerScreen(); full(); break;
        case SET_PEERS:  if (h >= 0) { g_hlPeers   = !g_hlPeers;   cfgSave(); render(); } break;
        case SET_SAME:   if (h >= 0) { g_hlSame    = !g_hlSame;    cfgSave(); render(); } break;
        case SET_AUTONOTE:if (h >= 0){ g_autoNotes = !g_autoNotes; cfgSave(); render(); } break;
        case SET_TIMER:  if (h >= 0) { g_showTimer = !g_showTimer; cfgSave(); render(); } break;
        case SET_MISTAKES:if (h >= 0){ g_hlMistakes = !g_hlMistakes; cfgSave(); render(); } break;
        case SET_MLIMIT:
          if (h == 1)      g_mistakeLim = (g_mistakeLim == 0) ? 3 : (g_mistakeLim == 3) ? 5 : 0;
          else if (h == 0) g_mistakeLim = (g_mistakeLim == 0) ? 5 : (g_mistakeLim == 5) ? 3 : 0;
          if (h >= 0) { cfgSave(); render(); } break;
        case SET_BRIGHT: if (h == 0 && theme.bright > 0) theme.bright--;
                         else if (h == 1 && theme.bright < 19) theme.bright++;
                         if (h >= 0) { theme.save(); applyBrightness(); render(); } break;
        case SET_LED:    if (h == 0 && theme.led_bright > 0) theme.led_bright--;
                         else if (h == 1 && theme.led_bright < 20) theme.led_bright++;
                         if (h >= 0) { theme.save(); render(); }
                         ledPreview(); break;
        case SET_ABOUT:  aboutScreen(); full(); break;
#ifndef HAS_CAP_TOUCH
        case SET_CAL:    touchCalRun(); full(); break;
#endif
        default: break;
      }
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      render();
    } else {
      fling = 0;
    }
    wasDown = down;
    delay(10);
  }
}

static String fmtTime(uint32_t sec) {
  char b[12];
  if (sec >= 3600) snprintf(b, sizeof(b), "%lu:%02lu:%02lu", (unsigned long)(sec/3600), (unsigned long)((sec/60)%60), (unsigned long)(sec%60));
  else             snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(sec/60), (unsigned long)(sec%60));
  return String(b);
}

static void statsScreen() {
  tft->fillScreen(COL_BG);
  drawHeader("Statistics", true);
#ifdef MARAUDER_V8
  int y = CONTENTY + 8, rowH = 22; uint8_t hf = 2;
#else
  int y = CONTENTY + 14, rowH = 30; uint8_t hf = 4;
#endif
  tft->setTextDatum(TL_DATUM);
  // Column header
  tft->setTextColor(COL_DIM, COL_BG);
  int cW = SCRW / 4;
  drawStr("Level",  10,          y, 2);
  drawStr("Played", cW + 4,      y, 1);
  drawStr("Won",    2*cW + 8,    y, 1);
  drawStr("Best",   3*cW,        y, 1);
  y += (hf == 4 ? 24 : 18);
  tft->drawFastHLine(8, y - 4, SCRW - 16, theme.edge());
  for (int i = 0; i < 4; i++) {
    tft->setTextColor(COL_FG, COL_BG);
    drawStr(PUZ_DIFF_NAME[i], 10, y, 2);
    tft->setTextColor(COL_DIM, COL_BG);
    drawStr(String(g_played[i]), cW + 4,   y, 2);
    drawStr(String(g_won[i]),    2*cW + 8, y, 2);
    drawStr(g_best[i] ? fmtTime(g_best[i]) : String("--"), 3*cW, y, 2);
    y += rowH;
  }
  y += 6;
  uint32_t tp = 0, tw = 0;
  for (int i = 0; i < 4; i++) { tp += g_played[i]; tw += g_won[i]; }
  tft->setTextColor(COL_FG, COL_BG);
  drawStr(String("Total solved: ") + tw + " / " + tp, 10, y, 2);

  statusLine("Tap to go back.", COL_DIM);
  uint16_t x, ty; waitTap(x, ty);
}

// ════════════════════════════════════════════════════════════════════════════
//  Sudoku board view — PORTRAIT, composited into one PSRAM sprite per frame.
// ════════════════════════════════════════════════════════════════════════════
#ifdef MARAUDER_V8
static const int IH       = 22;   // info bar
static const int GTOPGAP  = 3;
static const int GCELL    = 22;   // 9*22 = 198
static const int ACTH     = 28;
static const int NUMH     = 30;
static const int STRIPGAP = 3;
static const uint8_t CELLFONT = 2;   // 14 px firm digits
static const uint8_t NUMFONT  = 2;
static const uint8_t NOTEFONT = 1;   // 10 px pencil marks
#else
static const int IH       = 28;
static const int GTOPGAP  = 6;
static const int GCELL    = 34;   // 9*34 = 306
static const int ACTH     = 42;
static const int NUMH     = 46;
static const int STRIPGAP = 6;
static const uint8_t CELLFONT = 4;   // 22 px
static const uint8_t NUMFONT  = 4;
static const uint8_t NOTEFONT = 1;
#endif
static const int GRIDSZ = GCELL * 9;
static const int GX     = (SCRW - GRIDSZ) / 2;
static const int GY     = HDRH + IH + GTOPGAP;
static const int NUMY   = SCRH - NUMH - 4;
static const int ACTY   = NUMY - ACTH - STRIPGAP;

static TFT_eSprite *g_cs = nullptr;      // content sprite, SCRW x (SCRH-HDRH), pushed at (0,HDRH)
static const uint8_t *g_csFont = nullptr;
static const int CSH = SCRH - HDRH;
#define SY(v) ((v) - HDRH)               // screen-y -> content-sprite-y

static bool gSpriteBegin() {
  g_cs = new TFT_eSprite(tft);
  g_cs->setColorDepth(16);
  if (!g_cs->createSprite(SCRW, CSH)) { delete g_cs; g_cs = nullptr; return false; }
  return true;
}
static void gSpriteEnd() {
  if (g_cs) { g_cs->deleteSprite(); delete g_cs; g_cs = nullptr; }
}

static uint32_t elapsedSec() {
  return g_elapsedMs / 1000 + (g_running ? (millis() - g_startMs) / 1000 : 0);
}
static bool isPeer(int a, int b) {
  if (a == b) return false;
  return (a / 9 == b / 9) || (a % 9 == b % 9) ||
         ((a / 9) / 3 == (b / 9) / 3 && (a % 9) / 3 == (b % 9) / 3);
}
// A cell shows red when its value disagrees with the unique solution — but only
// when the Show Mistakes hint is on; off, wrong entries look like any other.
static bool cellWrong(int i) {
  if (!g_hlMistakes) return false;
  uint8_t v = g_sud.value(i);
  return v != 0 && !g_sud.isGiven(i) && v != g_sud.solution(i);
}

// Number-pad geometry: 9 keys across the grid width.
static int numKeyW() { return GRIDSZ / 9; }
static int numKeyX(int d /*1..9*/) { return GX + (d - 1) * numKeyW(); }
// Action row: 4 buttons across the grid width.
static const char *ACT_LABEL[4] = { "Undo", "Erase", "Notes", "Hint" };
static int actKeyW() { return GRIDSZ / 4; }
static int actKeyX(int i) { return GX + i * actKeyW(); }

static void gDrawInto(TFT_eSprite &cs) {
  cs.fillSprite(COL_BG);
  BoardPal bp = theme.board();

  // ── info bar ──
  int iy = SY(HDRH);
  cs.setTextDatum(ML_DATUM);
  cs.setTextColor(COL_FG, COL_BG);
  sprStr(cs, g_csFont, PUZ_DIFF_NAME[g_sud.difficulty()], 10, iy + IH / 2, 2);
  if (g_showTimer) {
    cs.setTextDatum(MC_DATUM);
    sprStr(cs, g_csFont, fmtTime(elapsedSec()), SCRW / 2, iy + IH / 2, 2);
  }
  cs.setTextDatum(MR_DATUM);
  String rstat;
  if (g_hlMistakes && g_mistakeLim) rstat = String("X ") + g_mistakes + "/" + g_mistakeLim;
  else if (g_hlMistakes && g_mistakes) rstat = String("X ") + g_mistakes;
  else rstat = String(g_sud.filledCount()) + "/81";
  cs.setTextColor(g_hlMistakes && g_mistakeLim && g_mistakes >= g_mistakeLim ? bp.badText : COL_DIM, COL_BG);
  sprStr(cs, g_csFont, rstat, SCRW - 10, iy + IH / 2, 2);
  cs.setTextDatum(TL_DATUM);

  // ── grid cells (fills) ──
  int gy = SY(GY);
  for (int i = 0; i < 81; i++) {
    int r = i / 9, c = i % 9;
    int x = GX + c * GCELL, y = gy + r * GCELL;
    uint16_t fill;
    if (cellWrong(i))                                             fill = bp.badFill;
    else if (i == g_sel)                                          fill = bp.sel;
    else if (g_hlSame && g_sel >= 0 && g_sud.value(i) != 0 &&
             g_sud.value(i) == g_sud.value(g_sel))                fill = bp.same;
    else if (g_hlPeers && g_sel >= 0 && isPeer(i, g_sel))         fill = bp.peer;
    else                                                          fill = g_sud.isGiven(i) ? bp.givenCell : bp.paper;
    cs.fillRect(x, y, GCELL, GCELL, fill);
  }
  // ── grid lines ──
  for (int k = 0; k <= 9; k++) {
    bool thick = (k % 3 == 0);
    uint16_t col = thick ? bp.lineThick : bp.lineThin;
    int gx = GX + k * GCELL;
    cs.drawFastVLine(gx, gy, GRIDSZ, col);
    cs.drawFastHLine(GX, gy + k * GCELL, GRIDSZ, col);
    if (thick) {   // 2 px for the box rules / border
      cs.drawFastVLine(gx - 1, gy, GRIDSZ, col);
      cs.drawFastHLine(GX, gy + k * GCELL - 1, GRIDSZ, col);
    }
  }
  // ── digits, then notes: two passes so the VLW font loader (vlw.h) never
  //    thrashes between CELLFONT and NOTEFONT mid-grid — each font loads once
  //    instead of potentially once per cell. ──
  cs.setTextDatum(MC_DATUM);
  for (int i = 0; i < 81; i++) {
    uint8_t v = g_sud.value(i);
    if (!v) continue;
    int r = i / 9, c = i % 9;
    int x = GX + c * GCELL, y = gy + r * GCELL;
    uint16_t tc = cellWrong(i) ? bp.badText
                : g_sud.isGiven(i) ? bp.givenText
                : g_hintCell[i] ? bp.hintText : bp.entryText;
    cs.setTextColor(tc);
    sprStr(cs, g_csFont, String((char)('0' + v)), x + GCELL / 2, y + GCELL / 2 + 1, CELLFONT);
  }
  cs.setTextColor(bp.noteText);
  for (int i = 0; i < 81; i++) {
    if (g_sud.value(i) || !g_sud.notes(i)) continue;
    int r = i / 9, c = i % 9;
    int x = GX + c * GCELL, y = gy + r * GCELL;
    int sub = GCELL / 3;
    for (uint8_t d = 1; d <= 9; d++) {
      if (!g_sud.hasNote(i, d)) continue;
      int nx = x + ((d - 1) % 3) * sub + sub / 2;
      int ny = y + ((d - 1) / 3) * sub + sub / 2;
      sprStr(cs, g_csFont, String((char)('0' + d)), nx, ny, NOTEFONT);
    }
  }
  cs.setTextDatum(TL_DATUM);

  // ── action row ──
  int aw = actKeyW(), ay = SY(ACTY);
  for (int i = 0; i < 4; i++) {
    int bx = actKeyX(i);
    bool on = (i == 2 && g_notesMode);
    uint16_t bg = on ? theme.sel() : COL_ACCENT;
    cs.fillRoundRect(bx + 2, ay, aw - 4, ACTH, 6, bg);
    cs.drawRoundRect(bx + 2, ay, aw - 4, ACTH, 6, theme.neon(i, COL_DIM));
    bool dim = (i == 0 && !g_sud.canUndo());
    cs.setTextColor(dim ? COL_DIM : COL_FG, bg);
    cs.setTextDatum(MC_DATUM);
    sprStr(cs, g_csFont, ACT_LABEL[i], bx + aw / 2, ay + ACTH / 2, 2);
  }
  cs.setTextDatum(TL_DATUM);

  // ── number row: all 9 digits, then all 9 remaining-count badges — this
  //    loop used to switch fonts up to 18x per render (guaranteed, on every
  //    tap and every 1Hz timer tick), which is why the number row is where
  //    the biggest chunk of the lag was hiding. ──
  int nw = numKeyW(), ny = SY(NUMY);
  cs.setTextDatum(MC_DATUM);
  for (int d = 1; d <= 9; d++) {
    int bx = numKeyX(d);
    bool done = g_sud.digitExhausted(d);
    cs.fillRoundRect(bx + 1, ny, nw - 2, NUMH, 5, COL_ACCENT);
    cs.drawRoundRect(bx + 1, ny, nw - 2, NUMH, 5, theme.neon(d, COL_DIM));
    cs.setTextColor(done ? COL_DIM : COL_FG, COL_ACCENT);
    sprStr(cs, g_csFont, String((char)('0' + d)), bx + nw / 2, ny + NUMH / 2 + 1, NUMFONT);
  }
  cs.setTextDatum(TL_DATUM);
  cs.setTextColor(COL_DIM, COL_ACCENT);
  for (int d = 1; d <= 9; d++) {
    int rem = 9 - g_sud.countOf((uint8_t)d);
    if (rem <= 0) continue;
    sprStr(cs, g_csFont, String(rem), numKeyX(d) + 4, ny + 2, 1);
  }
}

static void gRender() {
  if (!g_cs) return;
  g_csFont = nullptr;
  gDrawInto(*g_cs);
  g_cs->pushSprite(0, HDRH);
}

// ════════════════════════════════════════════════════════════════════════════
//  Partial redraw — each pair below draws exactly one widget into the
//  persistent content sprite (g_cs) and blits just that widget's rectangle to
//  the panel via TFT_eSprite::pushSprite(tx,ty,sx,sy,sw,sh) (a windowed push,
//  not a full-sprite one). A tap now repaints a handful of pixels instead of
//  the whole board.
// ════════════════════════════════════════════════════════════════════════════
static void drawCellFull(TFT_eSprite &cs, int i) {
  BoardPal bp = theme.board();
  int r = i / 9, c = i % 9;
  int x = GX + c * GCELL, y = SY(GY) + r * GCELL;
  uint16_t fill;
  if (cellWrong(i))                                             fill = bp.badFill;
  else if (i == g_sel)                                          fill = bp.sel;
  else if (g_hlSame && g_sel >= 0 && g_sud.value(i) != 0 &&
           g_sud.value(i) == g_sud.value(g_sel))                fill = bp.same;
  else if (g_hlPeers && g_sel >= 0 && isPeer(i, g_sel))         fill = bp.peer;
  else                                                          fill = g_sud.isGiven(i) ? bp.givenCell : bp.paper;
  cs.fillRect(x, y, GCELL, GCELL, fill);

  // Redraw this cell's own 4 border edges (thick at box boundaries, exactly
  // as the full-grid line pass draws them) so a lone-cell repaint still lines
  // up with its neighbours.
  bool thickL = (c % 3 == 0), thickR = ((c + 1) % 3 == 0);
  bool thickT = (r % 3 == 0), thickB = ((r + 1) % 3 == 0);
  cs.drawFastVLine(x, y, GCELL, thickL ? bp.lineThick : bp.lineThin);
  if (thickL) cs.drawFastVLine(x - 1, y, GCELL, bp.lineThick);
  cs.drawFastVLine(x + GCELL, y, GCELL, thickR ? bp.lineThick : bp.lineThin);
  if (thickR) cs.drawFastVLine(x + GCELL - 1, y, GCELL, bp.lineThick);
  cs.drawFastHLine(x, y, GCELL, thickT ? bp.lineThick : bp.lineThin);
  if (thickT) cs.drawFastHLine(x, y - 1, GCELL, bp.lineThick);
  cs.drawFastHLine(x, y + GCELL, GCELL, thickB ? bp.lineThick : bp.lineThin);
  if (thickB) cs.drawFastHLine(x, y + GCELL - 1, GCELL, bp.lineThick);

  uint8_t v = g_sud.value(i);
  cs.setTextDatum(MC_DATUM);
  if (v) {
    uint16_t tc = cellWrong(i) ? bp.badText
                : g_sud.isGiven(i) ? bp.givenText
                : g_hintCell[i] ? bp.hintText : bp.entryText;
    cs.setTextColor(tc);
    sprStr(cs, g_csFont, String((char)('0' + v)), x + GCELL / 2, y + GCELL / 2 + 1, CELLFONT);
  } else if (g_sud.notes(i)) {
    cs.setTextColor(bp.noteText);
    int sub = GCELL / 3;
    for (uint8_t d = 1; d <= 9; d++) {
      if (!g_sud.hasNote(i, d)) continue;
      int nx = x + ((d - 1) % 3) * sub + sub / 2;
      int ny = y + ((d - 1) / 3) * sub + sub / 2;
      sprStr(cs, g_csFont, String((char)('0' + d)), nx, ny, NOTEFONT);
    }
  }
  cs.setTextDatum(TL_DATUM);
}
static void pushCellRect(int i) {
  if (!g_cs) return;
  int r = i / 9, c = i % 9;
  int x = GX + c * GCELL - 2, ySpr = SY(GY) + r * GCELL - 2;
  int w = GCELL + 4, h = GCELL + 4;
  if (x < GX)                      { w -= (GX - x);              x = GX; }
  if (ySpr < SY(GY))               { h -= (SY(GY) - ySpr);       ySpr = SY(GY); }
  if (x + w > GX + GRIDSZ)         w = GX + GRIDSZ - x;
  if (ySpr + h > SY(GY) + GRIDSZ)  h = SY(GY) + GRIDSZ - ySpr;
  g_cs->pushSprite(x, ySpr + HDRH, x, ySpr, w, h);
}

static void drawNumKeyFull(TFT_eSprite &cs, int d) {
  int nw = numKeyW(), ny = SY(NUMY);
  int bx = numKeyX(d);
  bool done = g_sud.digitExhausted((uint8_t)d);
  cs.fillRoundRect(bx + 1, ny, nw - 2, NUMH, 5, COL_ACCENT);
  cs.drawRoundRect(bx + 1, ny, nw - 2, NUMH, 5, theme.neon(d, COL_DIM));
  cs.setTextColor(done ? COL_DIM : COL_FG, COL_ACCENT);
  cs.setTextDatum(MC_DATUM);
  sprStr(cs, g_csFont, String((char)('0' + d)), bx + nw / 2, ny + NUMH / 2 + 1, NUMFONT);
  int rem = 9 - g_sud.countOf((uint8_t)d);
  if (rem > 0) {
    cs.setTextColor(COL_DIM, COL_ACCENT);
    cs.setTextDatum(TL_DATUM);
    sprStr(cs, g_csFont, String(rem), bx + 4, ny + 2, 1);
  }
  cs.setTextDatum(TL_DATUM);
}
static void pushNumKeyRect(int d) {
  if (!g_cs) return;
  int nw = numKeyW(), x = numKeyX(d), ySpr = SY(NUMY);
  g_cs->pushSprite(x, NUMY, x, ySpr, nw, NUMH);
}

static void drawActKeyFull(TFT_eSprite &cs, int i) {
  int aw = actKeyW(), ay = SY(ACTY);
  int bx = actKeyX(i);
  bool on = (i == 2 && g_notesMode);
  uint16_t bg = on ? theme.sel() : COL_ACCENT;
  cs.fillRoundRect(bx + 2, ay, aw - 4, ACTH, 6, bg);
  cs.drawRoundRect(bx + 2, ay, aw - 4, ACTH, 6, theme.neon(i, COL_DIM));
  bool dim = (i == 0 && !g_sud.canUndo());
  cs.setTextColor(dim ? COL_DIM : COL_FG, bg);
  cs.setTextDatum(MC_DATUM);
  sprStr(cs, g_csFont, ACT_LABEL[i], bx + aw / 2, ay + ACTH / 2, 2);
  cs.setTextDatum(TL_DATUM);
}
static void pushActKeyRect(int i) {
  if (!g_cs) return;
  int aw = actKeyW(), x = actKeyX(i), ySpr = SY(ACTY);
  g_cs->pushSprite(x, ACTY, x, ySpr, aw, ACTH);
}

static const int INFO_STATUS_W = 140;   // right-aligned mistake/filled readout
static void drawInfoStatusFull(TFT_eSprite &cs) {
  BoardPal bp = theme.board();
  int iy = SY(HDRH), rx = SCRW - INFO_STATUS_W;
  cs.fillRect(rx, iy, INFO_STATUS_W, IH, COL_BG);
  cs.setTextDatum(MR_DATUM);
  String rstat;
  if (g_hlMistakes && g_mistakeLim) rstat = String("X ") + g_mistakes + "/" + g_mistakeLim;
  else if (g_hlMistakes && g_mistakes) rstat = String("X ") + g_mistakes;
  else rstat = String(g_sud.filledCount()) + "/81";
  cs.setTextColor(g_hlMistakes && g_mistakeLim && g_mistakes >= g_mistakeLim ? bp.badText : COL_DIM, COL_BG);
  sprStr(cs, g_csFont, rstat, SCRW - 10, iy + IH / 2, 2);
  cs.setTextDatum(TL_DATUM);
}
static void pushInfoStatusRect() {
  if (!g_cs) return;
  int rx = SCRW - INFO_STATUS_W, ySpr = SY(HDRH);
  g_cs->pushSprite(rx, HDRH, rx, ySpr, INFO_STATUS_W, IH);
}

static const int INFO_TIMER_W = 110;    // centred clock readout
static void drawInfoTimerFull(TFT_eSprite &cs) {
  int iy = SY(HDRH), tx = SCRW / 2 - INFO_TIMER_W / 2;
  cs.fillRect(tx, iy, INFO_TIMER_W, IH, COL_BG);
  cs.setTextDatum(MC_DATUM);
  cs.setTextColor(COL_FG, COL_BG);
  sprStr(cs, g_csFont, fmtTime(elapsedSec()), SCRW / 2, iy + IH / 2, 2);
  cs.setTextDatum(TL_DATUM);
}
static void pushInfoTimerRect() {
  if (!g_cs) return;
  int tx = SCRW / 2 - INFO_TIMER_W / 2, ySpr = SY(HDRH);
  g_cs->pushSprite(tx, HDRH, tx, ySpr, INFO_TIMER_W, IH);
}

// Selection changes can shade more than just the old/new cell (peers, same-
// digit highlighting), so redraw the full affected set, not just the pair.
static void redrawSelectionHighlights(int oldSel, int newSel) {
  bool touched[81] = { false };
  auto mark = [&](int s) {
    if (s < 0) return;
    touched[s] = true;
    if (!g_hlPeers && !g_hlSame) return;
    uint8_t sv = g_sud.value(s);
    for (int j = 0; j < 81; j++) {
      if (g_hlPeers && isPeer(j, s)) touched[j] = true;
      if (g_hlSame && sv != 0 && g_sud.value(j) == sv) touched[j] = true;
    }
  };
  mark(oldSel);
  mark(newSel);
  for (int i = 0; i < 81; i++) {
    if (!touched[i]) continue;
    drawCellFull(*g_cs, i);
    pushCellRect(i);
  }
}

// Momentary "key pressed" feedback. Drawn into the sprite and blitted like
// any other partial update, then restored by serviceFlash() once FLASH_MS has
// elapsed — a timestamp check each loop pass, not a blocking delay(), so
// touch polling never stalls.
static const uint16_t KEY_FLASH = 0xBDF7;   // light gray (same as the shell keyboard)
static const uint32_t FLASH_MS  = 70;
static int      g_flashNum   = -1;   // 1..9, or -1
static int      g_flashAct   = -1;   // 0..3, or -1
static uint32_t g_flashUntil = 0;

static void drawNumKeyFlash(int d) {
  if (!g_cs) return;
  int nw = numKeyW(), ny = SY(NUMY), bx = numKeyX(d);
  bool dim = g_sud.digitExhausted((uint8_t)d);
  g_cs->fillRoundRect(bx + 1, ny, nw - 2, NUMH, 5, KEY_FLASH);
  g_cs->drawRoundRect(bx + 1, ny, nw - 2, NUMH, 5, theme.neon(0, COL_DIM));
  g_cs->setTextColor(dim ? COL_DIM : contrastOn(KEY_FLASH), KEY_FLASH);
  g_cs->setTextDatum(MC_DATUM);
  sprStr(*g_cs, g_csFont, String((char)('0' + d)), bx + nw / 2, ny + NUMH / 2 + 1, NUMFONT);
  g_cs->setTextDatum(TL_DATUM);
  pushNumKeyRect(d);
  g_flashNum = d; g_flashUntil = millis() + FLASH_MS;
}
static void drawActKeyFlash(int i) {
  if (!g_cs) return;
  int aw = actKeyW(), ay = SY(ACTY), bx = actKeyX(i);
  bool dim = (i == 0 && !g_sud.canUndo());
  g_cs->fillRoundRect(bx + 2, ay, aw - 4, ACTH, 6, KEY_FLASH);
  g_cs->drawRoundRect(bx + 2, ay, aw - 4, ACTH, 6, theme.neon(0, COL_DIM));
  g_cs->setTextColor(dim ? COL_DIM : contrastOn(KEY_FLASH), KEY_FLASH);
  g_cs->setTextDatum(MC_DATUM);
  sprStr(*g_cs, g_csFont, ACT_LABEL[i], bx + aw / 2, ay + ACTH / 2 + 1, 2);
  g_cs->setTextDatum(TL_DATUM);
  pushActKeyRect(i);
  g_flashAct = i; g_flashUntil = millis() + FLASH_MS;
}
static void serviceFlash() {
  if (g_flashNum < 0 && g_flashAct < 0) return;
  if (millis() < g_flashUntil) return;
  if (g_flashNum >= 0) { drawNumKeyFull(*g_cs, g_flashNum); pushNumKeyRect(g_flashNum); g_flashNum = -1; }
  if (g_flashAct >= 0) { drawActKeyFull(*g_cs, g_flashAct); pushActKeyRect(g_flashAct); g_flashAct = -1; }
}

// Screen point -> cell index, or -1.
static int cellAt(int x, int y) {
  if (x < GX || x >= GX + GRIDSZ || y < GY || y >= GY + GRIDSZ) return -1;
  int c = (x - GX) / GCELL, r = (y - GY) / GCELL;
  if (c < 0 || c > 8 || r < 0 || r > 8) return -1;
  return r * 9 + c;
}
static int numAt(int x, int y) {
  if (y < NUMY || y >= NUMY + NUMH) return -1;
  int nw = numKeyW();
  int d = (x - GX) / nw + 1;
  return (d >= 1 && d <= 9 && x >= GX && x < GX + GRIDSZ) ? d : -1;
}
static int actAt(int x, int y) {
  if (y < ACTY || y >= ACTY + ACTH) return -1;
  int aw = actKeyW();
  int i = (x - GX) / aw;
  return (i >= 0 && i <= 3 && x >= GX && x < GX + GRIDSZ) ? i : -1;
}

static void recordWin() {
  uint8_t d = g_sud.difficulty();
  g_played[d]++; g_won[d]++;
  uint32_t t = elapsedSec();
  if (g_best[d] == 0 || t < g_best[d]) g_best[d] = t;
  statsSave();
}

// Enter a digit at the selected cell (or toggle a note). Returns true if changed.
static bool enterDigit(uint8_t d) {
  if (g_sel < 0 || g_sud.isGiven(g_sel)) return false;
  if (g_notesMode) {
    return g_sud.toggleNote(g_sel, d);
  }
  if (g_sud.value(g_sel) == d) return g_sud.clearCell(g_sel);   // tap-again clears
  g_hintCell[g_sel] = false;
  bool changed = g_sud.setValue(g_sel, d, g_autoNotes);
  if (changed && g_hlMistakes && d != g_sud.solution(g_sel)) g_mistakes++;
  return changed;
}

static void gameScreen() {
  if (!gSpriteBegin()) {
    msgScreen("Sudoku", "Out of memory", "The board needs PSRAM. Enable it in Tools.", TFT_RED);
    return;
  }
  tft->fillScreen(COL_BG);
  drawHeader("Sudoku", true);
  g_startMs = millis();
  g_running = true;
  gRender();   // one full composite + full push; every update after this is partial

  bool wasDown = false;
  uint16_t pX = 0, pY = 0;
  bool moved = false;
  uint32_t lastTick = millis();
  g_flashNum = -1; g_flashAct = -1;

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t x = touch->x(), y = touch->y();

    serviceFlash();   // non-blocking: restores a pressed key once its flash window elapses

    if (down && !wasDown) { pX = x; pY = y; moved = false; }
    else if (down && wasDown) { if (abs((int)x - pX) > 8 || abs((int)y - pY) > 8) moved = true; }
    else if (!down && wasDown && !moved) {
      // ── a tap ──
      if (backTapped(pX, pY)) {
        g_elapsedMs += (millis() - g_startMs); g_running = false;
        gameSave();
        gSpriteEnd();
        return;
      }
      bool changed = false;
      int cell = cellAt(pX, pY);
      int num  = numAt(pX, pY);
      int act  = actAt(pX, pY);

      if (cell >= 0) {
        if (g_sel != cell) { int old = g_sel; g_sel = cell; redrawSelectionHighlights(old, cell); }
      } else if (num >= 1) {
        drawNumKeyFlash(num);
        uint8_t prevVal = (g_sel >= 0) ? g_sud.value(g_sel) : 0;
        changed = enterDigit((uint8_t)num);
        if (changed) {
          drawCellFull(*g_cs, g_sel); pushCellRect(g_sel);
          drawInfoStatusFull(*g_cs); pushInfoStatusRect();
          if (prevVal && prevVal != num) { drawNumKeyFull(*g_cs, prevVal); pushNumKeyRect(prevVal); }
        }
      } else if (act >= 0) {
        drawActKeyFlash(act);
        switch (act) {
          case 0: {   // Undo — the undo ring is single-cell snapshots, so diff
                      // the grid before/after to find exactly which cell moved.
            uint8_t beforeVal[81]; uint16_t beforeNotes[81];
            for (int k = 0; k < 81; k++) { beforeVal[k] = g_sud.value(k); beforeNotes[k] = g_sud.notes(k); }
            changed = g_sud.undo();
            if (changed) {
              int ci = -1;
              for (int k = 0; k < 81; k++)
                if (beforeVal[k] != g_sud.value(k) || beforeNotes[k] != g_sud.notes(k)) { ci = k; break; }
              if (ci >= 0) {
                uint8_t oldV = beforeVal[ci], newV = g_sud.value(ci);
                drawCellFull(*g_cs, ci); pushCellRect(ci);
                if (oldV)                 { drawNumKeyFull(*g_cs, oldV); pushNumKeyRect(oldV); }
                if (newV && newV != oldV) { drawNumKeyFull(*g_cs, newV); pushNumKeyRect(newV); }
                drawInfoStatusFull(*g_cs); pushInfoStatusRect();
              }
            }
            break;
          }
          case 1:   // Erase
            if (g_sel >= 0) {
              uint8_t prevVal = g_sud.value(g_sel);
              g_hintCell[g_sel] = false;
              changed = g_sud.clearCell(g_sel);
              if (changed) {
                drawCellFull(*g_cs, g_sel); pushCellRect(g_sel);
                drawInfoStatusFull(*g_cs); pushInfoStatusRect();
                if (prevVal) { drawNumKeyFull(*g_cs, prevVal); pushNumKeyRect(prevVal); }
              }
            }
            break;
          case 2:   // Notes toggle — no cells change; serviceFlash() repaints
                    // this key itself (pressed/unpressed) once the flash ends.
            g_notesMode = !g_notesMode;
            changed = true;
            break;
          case 3:   // Hint
            if (g_sel >= 0 && !g_sud.isGiven(g_sel) && g_sud.value(g_sel) != g_sud.solution(g_sel)) {
              uint8_t prevVal = g_sud.value(g_sel);
              g_hintCell[g_sel] = true; g_hintsUsed++;
              changed = g_sud.applyHint(g_sel);
              if (changed) {
                uint8_t newVal = g_sud.value(g_sel);
                drawCellFull(*g_cs, g_sel); pushCellRect(g_sel);
                drawInfoStatusFull(*g_cs); pushInfoStatusRect();
                if (prevVal)                      { drawNumKeyFull(*g_cs, prevVal); pushNumKeyRect(prevVal); }
                if (newVal && newVal != prevVal)  { drawNumKeyFull(*g_cs, newVal); pushNumKeyRect(newVal); }
              }
            }
            break;
        }
      }

      if (changed) {
        // ── win? ──
        if (g_sud.isSolved()) {
          g_elapsedMs += (millis() - g_startMs); g_running = false;
          recordWin();
          gameDeleteSave();
          g_gameActive = false;
          gSpriteEnd();
          ledBlinkOk(400);
          msgScreen("Solved!", fmtTime(elapsedSec()),
                    String(PUZ_DIFF_NAME[g_sud.difficulty()]) + " completed" +
                    (g_hintsUsed ? String(" - ") + g_hintsUsed + " hint" + (g_hintsUsed > 1 ? "s" : "") : String("")),
                    COL_OK);
          return;
        }
        // ── mistake limit reached? ──
        if (g_hlMistakes && g_mistakeLim && g_mistakes >= g_mistakeLim) {
          g_elapsedMs += (millis() - g_startMs); g_running = false;
          g_played[g_sud.difficulty()]++; statsSave();
          gameDeleteSave();
          g_gameActive = false;
          gSpriteEnd();
          msgScreen("Game Over", "Too many mistakes",
                    String("You reached ") + g_mistakeLim + " mistakes.", TFT_RED);
          return;
        }
        // ── board full but not correct? (the only end-signal when hints are off) ──
        if (g_sud.isComplete()) {
          g_elapsedMs += (millis() - g_startMs); g_running = false;
          msgScreen("Almost!", "Grid full, not solved",
                    g_hlMistakes ? "Fix the cells marked in red."
                                 : "Show Mistakes is off - recheck your entries.", COL_FG);
          tft->fillScreen(COL_BG); drawHeader("Sudoku", true); gRender();
          g_startMs = millis(); g_running = true;
          wasDown = true; moved = true;   // swallow the dismiss tap's release
        }
      }
    }
    wasDown = down;

    // ── 1 Hz timer refresh — just the clock digits, not the whole board ──
    if (g_showTimer && millis() - lastTick >= 1000) {
      lastTick = millis();
      drawInfoTimerFull(*g_cs);
      pushInfoTimerRect();
    }
    delay(10);
  }
}

// New game — difficulty picker, then load a random puzzle from that tier.
static void resetMeta() {
  g_sel = -1; g_notesMode = false; g_mistakes = 0; g_hintsUsed = 0;
  g_elapsedMs = 0; g_running = false;
  for (int i = 0; i < 81; i++) g_hintCell[i] = false;
}
static void newGameFlow() {
  String rows[4];
  for (int i = 0; i < 4; i++)
    rows[i] = String(PUZ_DIFF_NAME[i]) + "  (" + PUZ_BANK_COUNT[i] + ")";
  int pick = scrollList("New Game", rows, 4, true);
  if (pick < 0) return;
  uint16_t idx = (uint16_t)random(PUZ_BANK_COUNT[pick]);
  g_sud.loadFromBank((uint8_t)pick, idx);
  resetMeta();
  g_gameActive = true;
  gameScreen();
}

// ════════════════════════════════════════════════════════════════════════════
//  Main menu
// ════════════════════════════════════════════════════════════════════════════
static const char *MENU_ITEMS[] = { "New Game", "Continue", "Statistics", "Settings", "About" };
static const int    MENU_COUNT  = 5;
static const int    MENU_MARGIN = 16;
static const int    MENU_TOP    = CONTENTY + 12;
static const int    MENU_GAP    = 12;
static int menuBtnH() {
  int avail = SCRH - MENU_TOP - 12;
  return (avail - (MENU_COUNT - 1) * MENU_GAP) / MENU_COUNT;
}
static int menuBtnY(int i) { return MENU_TOP + i * (menuBtnH() + MENU_GAP); }
static int menuButtonAt(uint16_t x, uint16_t y) {
  if ((int)x < MENU_MARGIN || (int)x >= SCRW - MENU_MARGIN) return -1;
  int bh = menuBtnH();
  for (int i = 0; i < MENU_COUNT; i++) {
    int by = menuBtnY(i);
    if ((int)y >= by && (int)y < by + bh) return i;
  }
  return -1;
}
static void drawMenu() {
  tft->fillScreen(COL_BG);
  drawHeader(FW_NAME, false);
  int bh = menuBtnH();
  for (int i = 0; i < MENU_COUNT; i++) {
    int y = menuBtnY(i);
    tft->fillRoundRect(MENU_MARGIN, y, SCRW - 2 * MENU_MARGIN, bh, 12, COL_ACCENT);
    tft->drawRoundRect(MENU_MARGIN, y, SCRW - 2 * MENU_MARGIN, bh, 12, theme.neon(i * 3, COL_DIM));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
#ifdef MARAUDER_V8
    drawStr(MENU_ITEMS[i], SCRW / 2, y + bh / 2, 2);
#else
    drawStr(MENU_ITEMS[i], SCRW / 2, y + bh / 2, 4);
#endif
  }
  tft->setTextDatum(TL_DATUM);
}
static void openMenuItem(int i) {
  switch (i) {
    case 0: newGameFlow(); break;
    case 1:
      if (g_gameActive) { gameScreen(); break; }
      if (gameLoad()) { gameScreen(); }
      else msgScreen("Continue", "No saved game",
                     "Start a new game - it saves automatically when you leave the board.", COL_DIM);
      break;
    case 2: statsScreen();  break;
    case 3: settingsFlow(); break;
    case 4: aboutScreen();  break;
    default: break;
  }
  drawMenu();
}

static bool mainMenuStart(ViewManager *viewManager) {
  drawMenu();
  return true;
}
static void mainMenuRun(ViewManager *viewManager) {
  static bool wasDown = false;
  TouchInput *t = viewManager->getInputManager()->getTouch();
  bool down = t->isPressed();
  if (down && !wasDown) {
    uint16_t x = t->x(), y = t->y();
    int btn = menuButtonAt(x, y);
    if (btn >= 0) openMenuItem(btn);
  }
  wasDown = down;

  static uint32_t lastRefresh = 0;
  if (millis() - lastRefresh > 5000) {
    lastRefresh = millis();
    drawHeaderStatus();
    drawHeaderMem(false);
  }
}
static const PROGMEM View mainMenuView = View("MainMenu", mainMenuRun, mainMenuStart, nullptr);

// ════════════════════════════════════════════════════════════════════════════
//  Arduino entry points
// ════════════════════════════════════════════════════════════════════════════
void setup() {
  randomSeed(esp_random());
#ifndef DEVELOPER
  esp_log_level_set("*", ESP_LOG_NONE);
#endif
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);
  Serial.println(F("[" BOARD_NAME "] Sudoku starting..."));

  pinMode(TFT_BL, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 0);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, 0);
#endif

#ifdef HAS_C5_SD
  sharedSPI.begin(SD_SCK, SD_MISO, SD_MOSI);
  delay(100);
  g_sdOk = SD.begin(SD_CS, sharedSPI);
#else
  g_sdOk = SD.begin(SD_CS);
#endif
  Serial.println(g_sdOk ? F("[" BOARD_NAME "] SD OK") : F("[" BOARD_NAME "] SD absent (SPIFFS saves)"));

  if (!SPIFFS.begin(true)) Serial.println(F("[" BOARD_NAME "] SPIFFS mount failed"));

#ifdef HAS_PSRAM
  if (!psramInit()) Serial.println(F("[" BOARD_NAME "] PSRAM unavailable"));
#endif

#ifdef HAS_CAP_TOUCH
  ft6336_init();
#else
  Wire.begin(I2C_SDA, I2C_SCL, 400000U);
#endif
  battInit();

  theme.load();
  cfgLoad();
  statsLoad();
  ledOff();

#ifdef MARAUDER_V8
  vm = new ViewManager(MarauderV8Config);
#else
  vm = new ViewManager(PancakeConfig);
#endif
  tft   = vm->getDraw()->display->getTFT();
  touch = vm->getInputManager()->getTouch();
  applyThemeToViewManager();
  applyBrightness();

#ifndef HAS_CAP_TOUCH
  if (touch) touch->attachTFT(tft);
  touchCalInit();
#endif

  vm->add(&mainMenuView);
  vm->set("MainMenu");
  Serial.println(F("[" BOARD_NAME "] Ready."));
}

void loop() {
  vm->run();
}
