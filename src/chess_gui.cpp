#include <Arduino.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include "board_config.h"

// Keep the Arduino loop stack large enough for Engine setup and UI work.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Arduino GPIO macros conflict with identifiers in Stockfish headers.
#ifdef HIGH
  #undef HIGH
#endif
#ifdef LOW
  #undef LOW
#endif
#ifdef cli
  #undef cli
#endif

#include "stockfish/engine.h"
#include "stockfish/misc.h"
#include "stockfish/attacks.h"
#include "stockfish/position.h"
#include "gui/pieces.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

namespace {

constexpr char StartFen[] =
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

constexpr int BoardX = 4;
constexpr int BoardY = 16;  // Center the 208px board vertically on the 240px display.
constexpr int SquareSize = 26;
constexpr int BoardSize = SquareSize * 8;
static_assert(PIECE_ICON_SIZE == SquareSize, "Piece icon size must match a board square");
static uint16_t pieceTile[SquareSize * SquareSize];

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Deep navy background, closer to the TeensyFish reference palette.
constexpr uint16_t ColorBackground = rgb(16, 18, 28);
constexpr uint16_t ColorPanel      = rgb(22, 26, 39);
constexpr uint16_t ColorLight      = rgb(194, 193, 201);
constexpr uint16_t ColorDark       = rgb(54, 99, 197);
constexpr uint16_t ColorOrange     = rgb(255, 169, 0);
constexpr uint16_t ColorText       = rgb(239, 240, 245);
constexpr uint16_t ColorMuted      = rgb(149, 157, 177);
constexpr uint16_t ColorButton     = rgb(51, 77, 128);
constexpr uint16_t ColorDisabled   = rgb(48, 54, 68);
constexpr uint16_t ColorSelected   = rgb(235, 220, 111);
constexpr uint16_t ColorLastMove   = rgb(224, 204, 91);
constexpr uint16_t ColorHint       = rgb(69, 190, 184);
constexpr uint16_t ColorCheck      = rgb(217, 65, 65);

class BoardDisplay : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ILI9341 _panel;

public:
  BoardDisplay() {
    auto b = _bus.config();
    b.spi_host = SPI2_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.spi_3wire = false;
    b.use_lock = true;
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = LCD_SCLK;
    b.pin_mosi = LCD_MOSI;
    b.pin_miso = LCD_MISO;
    b.pin_dc = LCD_DC;
    _bus.config(b);
    _panel.setBus(&_bus);

    auto p = _panel.config();
    p.pin_cs = LCD_CS;
    p.pin_rst = LCD_RST;
    p.pin_busy = -1;
    p.memory_width = LCD_WIDTH;
    p.memory_height = LCD_HEIGHT;
    p.panel_width = LCD_WIDTH;
    p.panel_height = LCD_HEIGHT;
    p.offset_x = 0;
    p.offset_y = 0;
    p.offset_rotation = 0;
    p.dummy_read_pixel = 8;
    p.dummy_read_bits = 1;
    p.readable = true;
    p.invert = false;
    p.rgb_order = false;
    p.dlen_16bit = false;
    p.bus_shared = true;
    _panel.config(p);
    setPanel(&_panel);
  }
};

BoardDisplay tft;
Stockfish::Engine *engine = nullptr;

enum class Screen : uint8_t { Calibration, Menu, Game };
enum class SearchMode : uint8_t { None, EngineMove, Hint };

Screen screen = Screen::Menu;
SearchMode searchMode = SearchMode::None;

std::vector<std::string> gameMoves;
std::string currentFen = StartFen;
char board[64] = {};
char playerColor = 'w';
int skillLevel = 10;
int selectedSquare = -1;
int lastFrom = -1;
int lastTo = -1;
int hintFrom = -1;
int hintTo = -1;
bool hasGame = false;
bool searchInFlight = false;
char statusText[48] = "Ready";
char hintText[16] = "";

struct SearchMailbox {
  int depth;
  uint32_t nps;
  uint64_t nodes;
  bool infoDirty;
  bool bestMoveReady;
  char bestMove[16];
};

SearchMailbox searchMailbox{};
portMUX_TYPE searchMux = portMUX_INITIALIZER_UNLOCKED;

struct TouchCalibration {
  uint32_t magic;
  float ax, bx, cx;
  float ay, by, cy;
};

constexpr uint32_t TouchCalibrationMagic = 0x53465443;
TouchCalibration touchCalibration{};
bool touchCalibrated = false;
bool previousTouchDown = false;

struct CalibrationSample {
  uint16_t rawX;
  uint16_t rawY;
  int16_t screenX;
  int16_t screenY;
};

CalibrationSample calibrationSamples[3];
constexpr int16_t CalibrationTargets[3][2] = {
  { 18, 18 }, { 301, 18 }, { 301, 221 }
};
int calibrationStep = 0;

uint32_t lastPanelRefresh = 0;

static void setStatus(const char *message) {
  snprintf(statusText, sizeof(statusText), "%s", message ? message : "");
}

static bool touchReadRaw(uint16_t &x, uint16_t &y, uint8_t &points) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom(TOUCH_I2C_ADDR, (uint8_t)5) != 5)
    return false;

  const uint8_t count = Wire.read() & 0x0F;
  const uint8_t xh = Wire.read();
  const uint8_t xl = Wire.read();
  const uint8_t yh = Wire.read();
  const uint8_t yl = Wire.read();

  points = count;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return count != 0;
}

static bool solveTouchCalibration() {
  const double x0 = calibrationSamples[0].rawX;
  const double y0 = calibrationSamples[0].rawY;
  const double x1 = calibrationSamples[1].rawX;
  const double y1 = calibrationSamples[1].rawY;
  const double x2 = calibrationSamples[2].rawX;
  const double y2 = calibrationSamples[2].rawY;

  const double denominator =
    x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1);

  if (fabs(denominator) < 100.0)
    return false;

  const double sx0 = calibrationSamples[0].screenX;
  const double sx1 = calibrationSamples[1].screenX;
  const double sx2 = calibrationSamples[2].screenX;
  const double sy0 = calibrationSamples[0].screenY;
  const double sy1 = calibrationSamples[1].screenY;
  const double sy2 = calibrationSamples[2].screenY;

  touchCalibration.ax =
    (float)((sx0 * (y1 - y2) + sx1 * (y2 - y0) + sx2 * (y0 - y1)) / denominator);
  touchCalibration.bx =
    (float)((sx0 * (x2 - x1) + sx1 * (x0 - x2) + sx2 * (x1 - x0)) / denominator);
  touchCalibration.cx =
    (float)((sx0 * (x1 * y2 - x2 * y1) + sx1 * (x2 * y0 - x0 * y2)
             + sx2 * (x0 * y1 - x1 * y0)) / denominator);

  touchCalibration.ay =
    (float)((sy0 * (y1 - y2) + sy1 * (y2 - y0) + sy2 * (y0 - y1)) / denominator);
  touchCalibration.by =
    (float)((sy0 * (x2 - x1) + sy1 * (x0 - x2) + sy2 * (x1 - x0)) / denominator);
  touchCalibration.cy =
    (float)((sy0 * (x1 * y2 - x2 * y1) + sy1 * (x2 * y0 - x0 * y2)
             + sy2 * (x0 * y1 - x1 * y0)) / denominator);

  touchCalibration.magic = TouchCalibrationMagic;
  return true;
}

static void loadTouchCalibration() {
  Preferences preferences;
  if (!preferences.begin("sf-gui", true))
    return;
  const size_t bytes = preferences.getBytes("touch", &touchCalibration,
                                             sizeof(touchCalibration));
  preferences.end();
  touchCalibrated = bytes == sizeof(touchCalibration)
                 && touchCalibration.magic == TouchCalibrationMagic;
}

static bool saveTouchCalibration() {
  Preferences preferences;
  if (!preferences.begin("sf-gui", false)) {
    Serial.println("[GUI] ERROR: could not open NVS namespace sf-gui");
    return false;
  }

  const size_t written =
    preferences.putBytes("touch", &touchCalibration, sizeof(touchCalibration));
  preferences.end();

  const bool saved = written == sizeof(touchCalibration);
  Serial.printf("[GUI] Touch calibration %s (%u/%u bytes in NVS)\n",
                saved ? "saved" : "SAVE FAILED",
                (unsigned)written, (unsigned)sizeof(touchCalibration));
  return saved;
}

static void mapTouch(uint16_t rawX, uint16_t rawY, int &x, int &y) {
  const float mappedX = touchCalibration.ax * rawX
                      + touchCalibration.bx * rawY + touchCalibration.cx;
  const float mappedY = touchCalibration.ay * rawX
                      + touchCalibration.by * rawY + touchCalibration.cy;
  x = constrain((int)lroundf(mappedX), 0, tft.width() - 1);
  y = constrain((int)lroundf(mappedY), 0, tft.height() - 1);
}

static void drawCenteredText(const char *text, int x, int y, int w, int h,
                             uint16_t color, const lgfx::IFont *font) {
  tft.setFont(font);
  tft.setTextSize(1);
  tft.setTextColor(color);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(text, x + w / 2, y + h / 2);
}

static void drawTextAt(const char *text, int x, int y, uint16_t color,
                       const lgfx::IFont *font) {
  tft.setFont(font);
  tft.setTextSize(1);
  tft.setTextColor(color);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(text, x, y);
}

static void drawButton(int x, int y, int w, int h, const char *label,
                       bool active = false, bool enabled = true,
                       const lgfx::IFont *font = &fonts::FreeSansBold9pt7b) {
  const uint16_t fill = !enabled ? ColorDisabled
                          : active ? ColorOrange : ColorButton;
  const uint16_t text = active ? ColorBackground : ColorText;
  tft.fillRoundRect(x, y, w, h, 7, fill);
  tft.drawRoundRect(x, y, w, h, 7, active ? ColorOrange : ColorButton);
  tft.setFont(font);
  tft.setTextSize(1);
  tft.setTextColor(enabled ? text : ColorMuted);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(label, x + w / 2, y + h / 2);
}

static void drawCrosshair(int x, int y) {
  tft.drawLine(x - 12, y, x + 12, y, ColorText);
  tft.drawLine(x, y - 12, x, y + 12, ColorText);
  tft.drawCircle(x, y, 5, ColorOrange);
  tft.fillCircle(x, y, 2, ColorCheck);
}

static void renderCalibration() {
  tft.fillScreen(ColorBackground);
  drawCenteredText("Touch calibration", 20, 79, 280, 38,
                   ColorOrange, &fonts::FreeSansBold12pt7b);
  tft.drawFastHLine(48, 119, 224, ColorButton);
  drawCenteredText("Tap the crosshair", 20, 130, 280, 36,
                   ColorText, &fonts::FreeSans9pt7b);

  char stepText[24];
  snprintf(stepText, sizeof(stepText), "Step %d of 3", calibrationStep + 1);
  drawCenteredText(stepText, 20, 177, 280, 28,
                   ColorMuted, &fonts::FreeSans9pt7b);

  drawCrosshair(CalibrationTargets[calibrationStep][0],
                CalibrationTargets[calibrationStep][1]);
}

static void beginTouchCalibration() {
  calibrationStep = 0;
  touchCalibrated = false;
  screen = Screen::Calibration;
  renderCalibration();
}

static bool loadFenBoard(const std::string &fen) {
  memset(board, 0, sizeof(board));
  int rank = 7;
  int file = 0;

  for (char ch : fen) {
    if (ch == ' ')
      break;
    if (ch == '/') {
      --rank;
      file = 0;
    } else if (ch >= '1' && ch <= '8') {
      file += ch - '0';
    } else if (isalpha((unsigned char)ch)) {
      if (rank < 0 || rank > 7 || file < 0 || file > 7)
        return false;
      board[rank * 8 + file] = ch;
      ++file;
    }
  }
  return true;
}

static char sideToMove() {
  const size_t space = currentFen.find(' ');
  return space != std::string::npos && space + 1 < currentFen.size()
       ? currentFen[space + 1] : 'w';
}

static int squareFromUci(const char *text) {
  if (!text || text[0] < 'a' || text[0] > 'h'
      || text[1] < '1' || text[1] > '8')
    return -1;
  return (text[1] - '1') * 8 + (text[0] - 'a');
}

static void updateLastMove() {
  lastFrom = lastTo = -1;
  if (gameMoves.empty())
    return;
  lastFrom = squareFromUci(gameMoves.back().c_str());
  lastTo = squareFromUci(gameMoves.back().c_str() + 2);
}

static bool syncEnginePosition() {
  if (!engine)
    return false;
  const auto error = engine->set_position(StartFen, gameMoves);
  if (error.has_value()) {
    Serial.printf("[GUI] Position rejected: %s\n", error->what());
    return false;
  }
  currentFen = engine->fen();
  if (!loadFenBoard(currentFen)) {
    setStatus("FEN parse error");
    return false;
  }
  updateLastMove();
  return true;
}

static int materialValue(char piece) {
  switch (tolower((unsigned char)piece)) {
    case 'p': return 1;
    case 'n': case 'b': return 3;
    case 'r': return 5;
    case 'q': return 9;
    default: return 0;
  }
}

static int materialBalanceForPlayer() {
  int white = 0;
  int black = 0;
  for (char piece : board) {
    if (!piece)
      continue;
    if (isupper((unsigned char)piece))
      white += materialValue(piece);
    else
      black += materialValue(piece);
  }
  return playerColor == 'w' ? white - black : black - white;
}

static int pieceIconIndex(char piece) {
  switch (tolower((unsigned char)piece)) {
    case 'p': return 0;
    case 'n': return 1;
    case 'b': return 2;
    case 'r': return 3;
    case 'q': return 4;
    case 'k': return 5;
    default: return -1;
  }
}

static void drawPiece(char piece, int x, int y, uint16_t squareColor) {
  const int iconIndex = pieceIconIndex(piece);
  if (iconIndex < 0) {
    tft.fillRect(x, y, SquareSize, SquareSize, squareColor);
    return;
  }

  const bool white = isupper((unsigned char)piece);
  const uint16_t body = white ? rgb(255, 255, 255) : rgb(0, 0, 0);
  const uint16_t outline = white ? rgb(0, 0, 0) : rgb(150, 150, 150);

  // The 2-bit source icon uses 0=transparent, 1=body and 2=outline.
  // Compose one 26x26 RGB565 tile and send it as a single display transfer.
  // This uses 1.3 KiB of static internal RAM, not a full-screen framebuffer.
  for (int i = 0; i < SquareSize * SquareSize; ++i)
    pieceTile[i] = squareColor;

  constexpr int PixelCount = PIECE_ICON_SIZE * PIECE_ICON_SIZE;
  for (int i = 0; i < PixelCount; ++i) {
    const uint8_t packed = pgm_read_byte(&PIECE_ICONS[iconIndex][i >> 2]);
    const uint8_t pixel = (packed >> (6 - 2 * (i & 3))) & 0x03;
    if (pixel == 1)
      pieceTile[i] = body;
    else if (pixel == 2)
      pieceTile[i] = outline;
  }

  tft.pushImage(x, y, SquareSize, SquareSize, pieceTile);
}

static void drawBoard() {
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      const int file = playerColor == 'w' ? col : 7 - col;
      const int rank = playerColor == 'w' ? 7 - row : row;
      const int square = rank * 8 + file;
      const int x = BoardX + col * SquareSize;
      const int y = BoardY + row * SquareSize;

      uint16_t squareColor = ((file + rank) & 1) ? ColorLight : ColorDark;
      if (square == lastFrom || square == lastTo)
        squareColor = ColorLastMove;
      if (square == hintFrom || square == hintTo)
        squareColor = ColorHint;
      if (square == selectedSquare)
        squareColor = ColorSelected;

      if (board[square])
        drawPiece(board[square], x, y, squareColor);
      else
        tft.fillRect(x, y, SquareSize, SquareSize, squareColor);
    }
  }
}

static void drawGamePanel() {
  const int x = BoardX + BoardSize + 6;
  const int w = 320 - x - 4;
  tft.fillRect(x, BoardY, w, 224, ColorPanel);

  drawCenteredText("Stockfish", x, 25, w, 18,
                   ColorText, &fonts::FreeSansBold9pt7b);
  char levelText[20];
  snprintf(levelText, sizeof(levelText), "Level %d", skillLevel);
  drawCenteredText(levelText, x, 44, w, 14,
                   ColorOrange, &fonts::FreeSansBold9pt7b);

  const char *turnText = searchInFlight ? "Thinking"
                       : sideToMove() == playerColor ? "Your move" : "Engine";
  drawCenteredText(turnText, x + 1, 62, w - 2, 16,
                   searchInFlight ? ColorOrange : ColorText,
                   &fonts::FreeSansBold9pt7b);

  int uiDepth;
  uint32_t uiNps;
  portENTER_CRITICAL(&searchMux);
  uiDepth = searchMailbox.depth;
  uiNps = searchMailbox.nps;
  portEXIT_CRITICAL(&searchMux);

  char depthText[24];
  snprintf(depthText, sizeof(depthText), "Depth %d", uiDepth);
  drawCenteredText(depthText, x, 82, w, 14,
                   ColorMuted, &fonts::FreeSans9pt7b);

  char npsText[24];
  if (uiNps >= 1000)
    snprintf(npsText, sizeof(npsText), "%.1fk N/s", uiNps / 1000.0f);
  else
    snprintf(npsText, sizeof(npsText), "%lu N/s", (unsigned long)uiNps);
  drawCenteredText(npsText, x, 97, w, 14,
                   ColorMuted, &fonts::FreeSans9pt7b);

  const int material = materialBalanceForPlayer();
  char materialText[20];
  snprintf(materialText, sizeof(materialText), "Material %+d", material);
  drawCenteredText(materialText, x, 113, w, 14,
                   material > 0 ? ColorOrange : ColorText,
                   &fonts::FreeSansBold9pt7b);

  drawCenteredText(statusText, x + 1, 129, w - 2, 14,
                   ColorMuted, &fonts::FreeSans9pt7b);
  drawButton(x + 2, 147, w - 4, 25, "HINT", false,
             !searchInFlight && sideToMove() == playerColor,
             &fonts::FreeSansBold9pt7b);
  drawButton(x + 2, 177, w - 4, 25, "UNDO", false, !gameMoves.empty(),
             &fonts::FreeSansBold9pt7b);
  drawButton(x + 2, 207, w - 4, 25, "NEW", false, true,
             &fonts::FreeSansBold9pt7b);
}

static void renderGame() {
  screen = Screen::Game;
  tft.fillScreen(ColorBackground);
  drawBoard();
  drawGamePanel();
}

static void renderMenu() {
  screen = Screen::Menu;
  tft.fillScreen(ColorBackground);

  drawCenteredText("New Game", 20, 12, 280, 40,
                   ColorOrange, &fonts::FreeSansBold12pt7b);
  drawTextAt("Play as", 31, 59, ColorMuted, &fonts::FreeSans9pt7b);

  drawButton(28, 87, 128, 42, "White", playerColor == 'w');
  drawButton(164, 87, 128, 42, "Black", playerColor == 'b');

  drawTextAt("Stockfish level", 31, 142,
             ColorMuted, &fonts::FreeSans9pt7b);

  drawButton(30, 164, 50, 38, "-");
  char levelText[12];
  snprintf(levelText, sizeof(levelText), "%d", skillLevel);
  drawCenteredText(levelText, 94, 164, 132, 38,
                   ColorText, &fonts::FreeSansBold12pt7b);
  drawButton(240, 164, 50, 38, "+");

  drawButton(28, 207, 128, 30, "Start");
  drawButton(164, 207, 128, 30, "Cancel");
  drawButton(230, 5, 86, 26, "CALIBRATE", false, true,
             &fonts::FreeSansBold9pt7b);
}

static void renderLoading() {
  tft.fillScreen(ColorBackground);
  drawCenteredText("Stockfish ESP32", 20, 76, 280, 32,
                   ColorOrange, &fonts::FreeSansBold12pt7b);
  drawCenteredText("Loading engine and NNUE...", 20, 122, 280, 24,
                   ColorText, &fonts::FreeSans9pt7b);
}

static bool setSkillOption() {
  if (!engine)
    return false;
  std::istringstream option("name Skill Level value " + std::to_string(skillLevel));
  engine->get_options().setoption(option);
  return true;
}

static void renderCalibrationComplete(bool saved) {
  tft.fillScreen(ColorBackground);
  drawCenteredText("Touch ready", 20, 88, 280, 34,
                   ColorOrange, &fonts::FreeSansBold12pt7b);
  drawCenteredText(saved ? "Calibration saved to flash" : "Flash save failed",
                   20, 132, 280, 24, ColorText, &fonts::FreeSans9pt7b);
  delay(500);
}

static void startSearch(SearchMode mode) {
  if (!engine || searchInFlight)
    return;

  if (mode == SearchMode::Hint && sideToMove() != playerColor)
    return;

  hintFrom = hintTo = -1;
  hintText[0] = '\0';
  searchMode = mode;

  portENTER_CRITICAL(&searchMux);
  searchMailbox.depth = 0;
  searchMailbox.nps = 0;
  searchMailbox.nodes = 0;
  searchMailbox.infoDirty = true;
  searchMailbox.bestMoveReady = false;
  searchMailbox.bestMove[0] = '\0';
  portEXIT_CRITICAL(&searchMux);

  Stockfish::Search::LimitsType limits;
  limits.movetime = mode == SearchMode::Hint ? 1400 : 2400;
  limits.startTime = Stockfish::now();

  searchInFlight = true;
  setStatus(mode == SearchMode::Hint ? "Finding hint" : "Thinking");
  engine->go(limits);
  drawGamePanel();
}

static void stopSearch() {
  if (!engine || !searchInFlight)
    return;
  engine->stop();
  engine->wait_for_search_finished();
  searchInFlight = false;
  searchMode = SearchMode::None;

  portENTER_CRITICAL(&searchMux);
  searchMailbox.bestMoveReady = false;
  searchMailbox.bestMove[0] = '\0';
  portEXIT_CRITICAL(&searchMux);
}

static void handleSearchResult(const char *bestMove) {
  if (!bestMove || !bestMove[0] || strcmp(bestMove, "0000") == 0) {
    searchMode = SearchMode::None;
    setStatus("Game over");
    renderGame();
    return;
  }

  if (searchMode == SearchMode::Hint) {
    snprintf(hintText, sizeof(hintText), "%s", bestMove);
    hintFrom = squareFromUci(bestMove);
    hintTo = squareFromUci(bestMove + 2);
    char text[32];
    snprintf(text, sizeof(text), "Hint: %.5s", bestMove);
    setStatus(text);
    searchMode = SearchMode::None;
    renderGame();
    return;
  }

  if (searchMode == SearchMode::EngineMove) {
    gameMoves.emplace_back(bestMove);
    if (!syncEnginePosition()) {
      gameMoves.pop_back();
      syncEnginePosition();
      setStatus("Engine move error");
    } else {
      setStatus("Your move");
    }
    searchMode = SearchMode::None;
    renderGame();
  }
}

static void pollSearchMailbox() {
  bool infoDirty = false;
  bool bestMoveReady = false;
  char bestMove[16] = {};

  portENTER_CRITICAL(&searchMux);
  if (searchMailbox.infoDirty) {
    infoDirty = true;
    searchMailbox.infoDirty = false;
  }
  if (searchMailbox.bestMoveReady) {
    bestMoveReady = true;
    memcpy(bestMove, searchMailbox.bestMove, sizeof(bestMove));
    searchMailbox.bestMoveReady = false;
  }
  portEXIT_CRITICAL(&searchMux);

  if (infoDirty && screen == Screen::Game) {
    drawGamePanel();
  }

  if (bestMoveReady) {
    engine->wait_for_search_finished();
    searchInFlight = false;
    handleSearchResult(bestMove);
  }
}

static void startNewGame() {
  stopSearch();
  engine->search_clear();
  gameMoves.clear();
  selectedSquare = -1;
  hintFrom = hintTo = -1;
  hintText[0] = '\0';
  searchMode = SearchMode::None;

  setSkillOption();
  if (!syncEnginePosition()) {
    setStatus("Position error");
    renderMenu();
    return;
  }

  hasGame = true;
  setStatus("Ready");
  renderGame();

  // When the human chooses Black, Stockfish makes the opening move.
  if (playerColor == 'b')
    startSearch(SearchMode::EngineMove);
}

static void undoMove() {
  if (searchInFlight)
    stopSearch();

  if (gameMoves.empty()) {
    setStatus("Nothing to undo");
    drawGamePanel();
    return;
  }

  const size_t removeCount = gameMoves.size() >= 2 ? 2 : 1;
  for (size_t i = 0; i < removeCount; ++i)
    gameMoves.pop_back();

  selectedSquare = -1;
  hintFrom = hintTo = -1;
  if (!syncEnginePosition()) {
    setStatus("Undo error");
    renderGame();
    return;
  }

  setStatus("Move undone");
  renderGame();

  if (sideToMove() != playerColor)
    startSearch(SearchMode::EngineMove);
}

static void uciSquareName(int square, char *out) {
  if (square < 0 || square >= 64) {
    out[0] = '\0';
    return;
  }
  out[0] = (char)('a' + square % 8);
  out[1] = (char)('1' + square / 8);
  out[2] = '\0';
}

static void attemptMove(int from, int to) {
  if (from < 0 || to < 0 || from == to)
    return;

  char move[8];
  uciSquareName(from, move);
  uciSquareName(to, move + 2);
  int length = 4;

  // First GUI milestone: promotion defaults to Queen. A dedicated chooser
  // can be added without changing the underlying Stockfish move validation.
  const char movingPiece = board[from];
  if (tolower((unsigned char)movingPiece) == 'p'
      && ((playerColor == 'w' && to / 8 == 7)
          || (playerColor == 'b' && to / 8 == 0))) {
    move[length++] = 'q';
  }
  move[length] = '\0';

  gameMoves.emplace_back(move);
  const auto error = engine->set_position(StartFen, gameMoves);
  if (error.has_value()) {
    gameMoves.pop_back();
    engine->set_position(StartFen, gameMoves);
    selectedSquare = -1;
    setStatus("Illegal move");
    renderGame();
    return;
  }

  currentFen = engine->fen();
  loadFenBoard(currentFen);
  updateLastMove();
  selectedSquare = -1;
  hintFrom = hintTo = -1;
  hintText[0] = '\0';
  setStatus("Thinking");
  renderGame();

  if (sideToMove() != playerColor)
    startSearch(SearchMode::EngineMove);
}

static int screenToSquare(int x, int y) {
  if (x < BoardX || x >= BoardX + BoardSize
      || y < BoardY || y >= BoardY + BoardSize)
    return -1;

  const int col = (x - BoardX) / SquareSize;
  const int row = (y - BoardY) / SquareSize;
  const int file = playerColor == 'w' ? col : 7 - col;
  const int rank = playerColor == 'w' ? 7 - row : row;
  return rank * 8 + file;
}

static void handleBoardTouch(int x, int y) {
  if (searchInFlight)
    return;

  const int panelX = BoardX + BoardSize + 6;

  if (x >= panelX && y >= 147 && y < 173) {
    if (sideToMove() == playerColor)
      startSearch(SearchMode::Hint);
    return;
  }
  if (x >= panelX && y >= 177 && y < 203) {
    undoMove();
    return;
  }
  if (x >= panelX && y >= 207 && y < 235) {
    renderMenu();
    return;
  }

  const int square = screenToSquare(x, y);
  if (square < 0 || sideToMove() != playerColor)
    return;

  if (selectedSquare < 0) {
    const char piece = board[square];
    if (piece && ((playerColor == 'w' && isupper((unsigned char)piece))
                  || (playerColor == 'b' && islower((unsigned char)piece)))) {
      selectedSquare = square;
      setStatus("Select destination");
      renderGame();
    }
    return;
  }

  if (square == selectedSquare) {
    selectedSquare = -1;
    setStatus("Your move");
    renderGame();
    return;
  }

  const char targetPiece = board[square];
  const bool ownTarget = targetPiece
    && ((playerColor == 'w' && isupper((unsigned char)targetPiece))
        || (playerColor == 'b' && islower((unsigned char)targetPiece)));

  if (ownTarget) {
    selectedSquare = square;
    setStatus("Select destination");
    renderGame();
    return;
  }

  const int from = selectedSquare;
  attemptMove(from, square);
}

static void handleMenuTouch(int x, int y) {
  if (y >= 87 && y < 129) {
    if (x >= 28 && x < 156) {
      playerColor = 'w';
      renderMenu();
    } else if (x >= 164 && x < 292) {
      playerColor = 'b';
      renderMenu();
    }
    return;
  }

  if (y >= 164 && y < 202) {
    if (x >= 30 && x < 80 && skillLevel > 1) {
      --skillLevel;
      renderMenu();
    } else if (x >= 240 && x < 290 && skillLevel < 20) {
      ++skillLevel;
      renderMenu();
    }
    return;
  }

  if (x >= 230 && x < 316 && y >= 5 && y < 31) {
    beginTouchCalibration();
    return;
  }

  if (y >= 207 && y < 239) {
    if (x >= 28 && x < 156) {
      startNewGame();
    } else if (x >= 164 && x < 292) {
      if (hasGame)
        renderGame();
      else
        renderMenu();
    }
    return;
  }
}

static void handleCalibrationTouch(uint16_t rawX, uint16_t rawY) {
  calibrationSamples[calibrationStep].rawX = rawX;
  calibrationSamples[calibrationStep].rawY = rawY;
  calibrationSamples[calibrationStep].screenX = CalibrationTargets[calibrationStep][0];
  calibrationSamples[calibrationStep].screenY = CalibrationTargets[calibrationStep][1];

  ++calibrationStep;
  if (calibrationStep < 3) {
    renderCalibration();
    return;
  }

  if (!solveTouchCalibration()) {
    calibrationStep = 0;
    renderCalibration();
    return;
  }

  const bool saved = saveTouchCalibration();
  touchCalibrated = true;
  renderCalibrationComplete(saved);
  renderMenu();
}

static void processTouch(int x, int y, uint16_t rawX, uint16_t rawY) {
  switch (screen) {
    case Screen::Calibration:
      handleCalibrationTouch(rawX, rawY);
      break;
    case Screen::Menu:
      handleMenuTouch(x, y);
      break;
    case Screen::Game:
      handleBoardTouch(x, y);
      break;
  }
}

static void initializeDisplayAndTouch() {
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, 1);

  tft.init();
  tft.setRotation(1);
  Serial.printf("[GUI] Display initialized: %d x %d\n", tft.width(), tft.height());

  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, 1);
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
  delay(50);

  Wire.beginTransmission(TOUCH_I2C_ADDR);
  const bool touchPresent = Wire.endTransmission() == 0;
  Serial.printf("[GUI] FT6336U I2C @0x%02X: %s\n",
                TOUCH_I2C_ADDR, touchPresent ? "ACK" : "NOT FOUND");
}

static void initializeEngine() {
  // Stockfish's standalone Engine constructor expects the global attack and
  // position tables to have been initialized by the application first.
  // The diagnostic probe performs these same calls before constructing Engine.
  Serial.println("[GUI] Initializing Stockfish attack tables...");
  Stockfish::Attacks::init();
  Serial.println("[GUI] Initializing Stockfish position tables...");
  Stockfish::Position::init();
  Serial.println("[GUI] Attack and position initialization complete.");

  static Stockfish::Engine stockfish;
  engine = &stockfish;

  engine->set_on_verify_network([](std::string_view message) {
    Serial.printf("[NNUE] %.*s\n", (int)message.size(), message.data());
  });

  // The standalone Engine API requires all five search callbacks.
  // The UCI frontend normally supplies them; this GUI does not use all events,
  // so explicitly install no-op handlers for the events it does not display.
  engine->set_on_start([]() {
    // Search started. The UI already changed its state in startSearch().
  });

  engine->set_on_update_no_moves([](const Stockfish::Engine::InfoShort &) {
    // No additional event handling is needed for a terminal position yet.
  });

  engine->set_on_iter([](const Stockfish::Engine::InfoIter &) {
    // Per-iteration details are not currently displayed.
  });

  engine->set_on_update_full([](const Stockfish::Engine::InfoFull &info) {
    portENTER_CRITICAL(&searchMux);
    searchMailbox.depth = info.depth;
    searchMailbox.nps = (uint32_t)info.nps;
    searchMailbox.nodes = (uint64_t)info.nodes;
    searchMailbox.infoDirty = true;
    portEXIT_CRITICAL(&searchMux);
  });

  engine->set_on_bestmove([](std::string_view bestMove, std::string_view) {
    portENTER_CRITICAL(&searchMux);
    const size_t count = std::min(bestMove.size(), sizeof(searchMailbox.bestMove) - 1);
    if (count)
      memcpy(searchMailbox.bestMove, bestMove.data(), count);
    searchMailbox.bestMove[count] = '\0';
    searchMailbox.bestMoveReady = true;
    portEXIT_CRITICAL(&searchMux);
  });

  Serial.println("[GUI] Verifying embedded NNUE network...");
  engine->verify_network();
  Serial.println("[GUI] Engine ready");
  Serial.printf("[GUI] PSRAM free after Engine construction: %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println();
  Serial.println("Stockfish ESP32-S3 touchscreen GUI");

  initializeDisplayAndTouch();
  renderLoading();
  initializeEngine();

  loadTouchCalibration();
  Serial.printf("[GUI] Touch calibration: %s\n",
                touchCalibrated ? "loaded from NVS" : "not found");
  if (touchCalibrated) {
    renderMenu();
  } else {
    beginTouchCalibration();
  }
}

void loop() {
  pollSearchMailbox();

  uint16_t rawX = 0;
  uint16_t rawY = 0;
  uint8_t points = 0;
  const bool down = touchReadRaw(rawX, rawY, points);

  if (down && !previousTouchDown) {
    int x = rawX;
    int y = rawY;
    if (screen != Screen::Calibration && touchCalibrated)
      mapTouch(rawX, rawY, x, y);
    processTouch(x, y, rawX, rawY);
  }
  previousTouchDown = down;

  delay(12);
}
