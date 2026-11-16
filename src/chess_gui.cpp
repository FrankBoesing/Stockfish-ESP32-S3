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
#include "stockfish/movegen.h"
#include "stockfish/uci.h"
#include "stockfish/score.h"
#include "stockfish/esp_trace.h"
#include "gui/pieces.h"
#include "gui/game_history.h"
#include "gui/chess_rules.h"
#include "gui/flash_polyglot_book.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

namespace {

constexpr char StartFen[] =
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

constexpr uint32_t SavedGameMagic = 0x53464731; // "SFG1"
constexpr uint16_t SavedGameVersion = 2;
constexpr size_t SavedGameMaxFenBytes = 192;
struct SavedGameHeaderV1 {
  uint32_t magic;
  uint16_t version;
  uint8_t playerColor;
  uint8_t skillLevel;
  uint32_t moveCount;
  uint32_t checksum;
};
struct SavedGameHeader {
  uint32_t magic;
  uint16_t version;
  uint8_t playerColor;
  uint8_t skillLevel;
  uint32_t moveCount;
  uint16_t baseFenLength;
  uint16_t reserved;
  uint32_t checksum;
};
static_assert(sizeof(SavedGameHeaderV1) == 16, "Unexpected v1 saved-game header layout");
static_assert(sizeof(SavedGameHeader) == 20, "Unexpected v2 saved-game header layout");

constexpr int BoardX = 10;  // Reserve 10px for rank labels; board remains 208px wide.
constexpr int BoardY = 16;  // Center the 208px board vertically on the 240px display.
constexpr int SquareSize = 26;
constexpr int BoardSize = SquareSize * 8;
constexpr int PanelX = 218; // Keep the information panel at its existing screen position.
static_assert(PIECE_ICON_SIZE == SquareSize, "Piece icon size must match a board square");
static uint16_t pieceTile[SquareSize * SquareSize];

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Deep midnight navy, close to the dark TeensyFish look.
constexpr uint16_t ColorBackground = rgb(27, 32, 48);
constexpr uint16_t ColorPanel      = rgb(37, 45, 64);
constexpr uint16_t ColorLight      = rgb(194, 193, 201);
constexpr uint16_t ColorDark       = rgb(54, 99, 197);
constexpr uint16_t ColorOrange     = rgb(255, 169, 0);
constexpr uint16_t ColorText       = rgb(239, 240, 245);
constexpr uint16_t ColorMuted      = rgb(149, 157, 177);
constexpr uint16_t ColorButton     = rgb(51, 77, 128);
constexpr uint16_t ColorDisabled   = rgb(48, 54, 68);
constexpr uint16_t ColorSelected   = rgb(235, 220, 111);
constexpr uint16_t ColorTargetDot  = rgb(40, 40, 40);
constexpr uint16_t ColorEvalWhite    = rgb(230, 230, 235);
constexpr uint16_t ColorEvalBlack    = rgb(34, 39, 52);
constexpr uint16_t ColorEvalTextDark = rgb(43, 49, 64);
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
    // FNK0104B uses an IPS ILI9341 panel. LovyanGFX requires invert=true
    // for IPS panels; otherwise dark RGB565 colours can appear inverted.
    p.invert = true;
    // The ILI9341 panel uses BGR colour order on this board.
    p.rgb_order = false;
    p.dlen_16bit = false;
    p.bus_shared = true;
    _panel.config(p);
    setPanel(&_panel);
  }
};

BoardDisplay tft;
Stockfish::Engine *engine = nullptr;
PolyglotBook::FlashPolyglotBook openingBook;
bool openingBookEnabled = true;

enum class Screen : uint8_t { Calibration, Menu, Game, Promotion };
enum class SearchMode : uint8_t { None, EngineMove, Hint, Evaluation };

Screen screen = Screen::Menu;
SearchMode searchMode = SearchMode::None;

GameHistory gameHistory;
std::string gameBaseFen = StartFen;
std::string currentFen = StartFen;
char board[64] = {};
char playerColor = 'w';
int skillLevel = 10; // Strength for the current game
int selectedSkillLevel = 10; // Default for the next game
int selectedSquare = -1;
int lastFrom = -1;
int lastTo = -1;
int hintFrom = -1;
int hintTo = -1;
bool hasGame = false;
bool gameOver = false;
bool searchInFlight = false;
char statusText[48] = "Ready";
char hintText[16] = "";
ChessGuiRules::PromotionRequest pendingPromotion;
bool legalDestinations[64] = {};

enum class SearchScoreKind : uint8_t { None, Centipawns, Mate, Tablebase };

struct SearchMailbox {
  int depth;
  uint32_t nps;
  uint64_t nodes;
  char scoreSide;
  SearchScoreKind scoreKind;
  int scoreValue;
  bool tablebaseWin;
  bool scoreValid;
  int wdlWin;
  int wdlDraw;
  int wdlLoss;
  bool wdlValid;
  bool infoDirty;
  bool bestMoveReady;
  char bestMove[16];
};

SearchMailbox searchMailbox{};
portMUX_TYPE searchMux = portMUX_INITIALIZER_UNLOCKED;

static bool evalBarValid = false;
static SearchScoreKind evalBarScoreKind = SearchScoreKind::None;
static int evalBarWhiteCp = 0;
static bool evalBarWhiteWins = false;

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
static constexpr size_t SerialCommandCapacity = 2048;
static char serialCommand[SerialCommandCapacity] = {};
static size_t serialCommandLength = 0;
static bool serialCommandOverflow = false;

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
                       const lgfx::IFont *font = &fonts::FreeSansBold9pt7b,
                       int textYOffset = 0) {
  const uint16_t fill = !enabled ? ColorDisabled
                          : active ? ColorOrange : ColorButton;
  const uint16_t text = active ? ColorBackground : ColorText;
  tft.fillRoundRect(x, y, w, h, 7, fill);
  tft.drawRoundRect(x, y, w, h, 7, active ? ColorOrange : ColorButton);
  tft.setFont(font);
  tft.setTextSize(1);
  tft.setTextColor(enabled ? text : ColorMuted);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(label, x + w / 2, y + h / 2 + textYOffset);
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
  char move[6];
  if (!gameHistory.size() || !gameHistory.formatMoveAt(gameHistory.size() - 1, move))
    return;
  lastFrom = squareFromUci(move);
  lastTo = squareFromUci(move + 2);
}

static bool syncEnginePosition() {
  if (!engine)
    return false;
  const std::vector<std::string> moves = gameHistory.toUciMoves();
  const auto error = engine->set_position(gameBaseFen, moves);
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

static bool updateGameStatusFromPosition() {
  if (!engine) {
    gameOver = false;
    setStatus("Position error");
    return false;
  }

  const Stockfish::PositionStatus position = engine->game_status();
  const ChessGuiRules::GameOutcome outcome =
    ChessGuiRules::classifyGame(position, sideToMove(), playerColor);
  gameOver = ChessGuiRules::isTerminal(outcome);
  setStatus(ChessGuiRules::statusText(outcome));
  return gameOver;
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
    case 'k': return 5;    default: return -1;
  }
}

// Add the four small corner triangles used to identify capture destinations.
// They stay clear of the piece silhouette and are composed into the same
// RGB565 tile, so the square still needs only one display transfer.
static void addCaptureMarkerToTile() {
  constexpr int MarkerSize = 6; // Scaled from the 30px TeensyChess squares.
  for (int y = 0; y < MarkerSize; ++y) {
    for (int x = 0; x < MarkerSize - y; ++x) {
      pieceTile[y * SquareSize + x] = ColorTargetDot;
      pieceTile[y * SquareSize + (SquareSize - 1 - x)] = ColorTargetDot;
      pieceTile[(SquareSize - 1 - y) * SquareSize + x] = ColorTargetDot;
      pieceTile[(SquareSize - 1 - y) * SquareSize + (SquareSize - 1 - x)] = ColorTargetDot;
    }
  }
}

static void drawPiece(char piece, int x, int y, uint16_t squareColor,
                      bool captureMarker = false) {
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

  if (captureMarker)
    addCaptureMarkerToTile();

  tft.pushImage(x, y, SquareSize, SquareSize, pieceTile);
}

static uint16_t boardSquareColor(int square, int lastFromValue, int lastToValue,
                                     int hintFromValue, int hintToValue,
                                     int selectedSquareValue) {
  const int file = square % 8;
  const int rank = square / 8;
  uint16_t color = ((file + rank) & 1) ? ColorLight : ColorDark;

  if (square == lastFromValue || square == lastToValue)
    color = ColorLastMove;
  if (square == hintFromValue || square == hintToValue)
    color = ColorHint;
  if (square == selectedSquareValue)
    color = ColorSelected;
  return color;
}

static void drawBoardSquare(int square) {
  if (square < 0 || square >= 64)
    return;

  const int file = square % 8;
  const int rank = square / 8;
  const int col = playerColor == 'w' ? file : 7 - file;
  const int row = playerColor == 'w' ? 7 - rank : rank;
  const int x = BoardX + col * SquareSize;
  const int y = BoardY + row * SquareSize;
  const uint16_t color = boardSquareColor(square, lastFrom, lastTo,
                                          hintFrom, hintTo, selectedSquare);

  const bool legalTarget = legalDestinations[square];
  if (board[square]) {
    drawPiece(board[square], x, y, color, legalTarget);
  } else {
    tft.fillRect(x, y, SquareSize, SquareSize, color);
    if (legalTarget) {
      // Small Lichess-style destination dot on an empty square.
      tft.fillCircle(x + SquareSize / 2 - 1, y + SquareSize / 2 - 1,
                     4, ColorTargetDot);
    }
  }
}

static void clearLegalDestinations(bool redraw = true) {
  for (int square = 0; square < 64; ++square) {
    if (!legalDestinations[square])
      continue;
    legalDestinations[square] = false;
    if (redraw)
      drawBoardSquare(square);
  }
}

static void updateLegalDestinations(int fromSquare) {
  bool nextDestinations[64] = {};

  if (fromSquare >= 0 && fromSquare < 64) {
    Stockfish::Position position;
    Stockfish::StateInfo state;
    const auto error = position.set(currentFen, false, &state);
    if (!error.has_value()) {
      ChessGuiRules::legalDestinations(position, fromSquare, nextDestinations);
    } else {
      Serial.printf("[GUI] Cannot generate legal move highlights: %s\n",
                    error->what());
    }
  }

  for (int square = 0; square < 64; ++square) {
    if (legalDestinations[square] == nextDestinations[square])
      continue;
    legalDestinations[square] = nextDestinations[square];
    drawBoardSquare(square);
  }
}

static void drawChangedBoardSquares(const char *oldBoard,
                                    int oldLastFrom, int oldLastTo,
                                    int oldHintFrom, int oldHintTo,
                                    int oldSelectedSquare) {
  for (int square = 0; square < 64; ++square) {
    const uint16_t oldColor = boardSquareColor(
      square, oldLastFrom, oldLastTo, oldHintFrom, oldHintTo, oldSelectedSquare);
    const uint16_t newColor = boardSquareColor(
      square, lastFrom, lastTo, hintFrom, hintTo, selectedSquare);

    if (oldBoard[square] != board[square] || oldColor != newColor)
      drawBoardSquare(square);
  }
}

static void drawBoard() {
  for (int square = 0; square < 64; ++square)
    drawBoardSquare(square);
}

static int evaluationWhiteSharePermille() {
  if (!evalBarValid)
    return 0;

  if (evalBarScoreKind == SearchScoreKind::Centipawns) {
    // Match Lichess' published winning-chance model:
    // Win% = 100 / (1 + exp(-0.00368208 * centipawns)).
    // Stockfish's Score::InternalUnits already contains centipawns.
    const double exponent = -0.00368208 * (double)evalBarWhiteCp;
    const double whiteWinPercent = 100.0 / (1.0 + exp(exponent));
    return constrain((int)lround(whiteWinPercent * 10.0), 0, 1000);
  }

  // Mate and decisive tablebase scores are represented as a certain win
  // for the side indicated by the score, so the bar reaches the matching end.
  return evalBarWhiteWins ? 1000 : 0;
}

static int evaluationWhiteHeight() {
  return evalBarValid ? BoardSize * evaluationWhiteSharePermille() / 1000 : 0;
}

static void drawEvaluationBar() {
  // The bar occupies the existing rank-label strip and is drawn first;
  // coordinate labels are painted over it afterwards.
  tft.fillRect(0, BoardY, BoardX, BoardSize, ColorBackground);
  if (!evalBarValid)
    return;

  const int whiteHeight = evaluationWhiteHeight();
  if (playerColor == 'w') {
    const int whiteTop = BoardY + BoardSize - whiteHeight;
    if (whiteTop > BoardY)
      tft.fillRect(0, BoardY, BoardX, whiteTop - BoardY, ColorEvalBlack);
    if (whiteHeight > 0)
      tft.fillRect(0, whiteTop, BoardX, whiteHeight, ColorEvalWhite);
  } else {
    if (whiteHeight > 0)
      tft.fillRect(0, BoardY, BoardX, whiteHeight, ColorEvalWhite);
    const int blackTop = BoardY + whiteHeight;
    if (blackTop < BoardY + BoardSize)
      tft.fillRect(0, blackTop, BoardX, BoardY + BoardSize - blackTop, ColorEvalBlack);
  }
}

static void drawBoardCoordinates() {
  static constexpr char Files[] = "abcdefgh";
  static constexpr char Ranks[] = "87654321";

  // Font0 is deliberately small and unobtrusive. Labels occupy the existing
  // 16px top margin and the 10px strip to the left of the board.
  for (int col = 0; col < 8; ++col) {
    const int file = playerColor == 'w' ? col : 7 - col;
    char label[2] = { Files[file], '\0' };
    drawCenteredText(label, BoardX + col * SquareSize, 0, SquareSize, BoardY,
                     ColorMuted, &fonts::Font0);
  }

  const int whiteHeight = evaluationWhiteHeight();
  const int whiteBoundary = playerColor == 'w'
                          ? BoardY + BoardSize - whiteHeight
                          : BoardY + whiteHeight;
  for (int row = 0; row < 8; ++row) {
    const int rank = playerColor == 'w' ? 7 - row : row;
    const int centerY = BoardY + row * SquareSize + SquareSize / 2;
    uint16_t labelColor = ColorMuted;
    if (evalBarValid) {
      const bool whiteSegment = playerColor == 'w'
                              ? centerY >= whiteBoundary
                              : centerY < whiteBoundary;
      labelColor = whiteSegment ? ColorEvalTextDark : ColorText;
    }
    char label[2] = { Ranks[rank], '\0' };
    drawCenteredText(label, 0, BoardY + row * SquareSize, BoardX, SquareSize,
                     labelColor, &fonts::Font0);
  }
}

static void refreshEvaluationBar() {
  drawEvaluationBar();
  drawBoardCoordinates();
}

static void drawPanelText(const char *text, int x, int y, int w, int h,
                           uint16_t color, const lgfx::IFont *font) {
  tft.fillRect(x, y, w, h, ColorPanel);
  drawCenteredText(text, x, y, w, h, color, font);
}

static void drawGamePanelSearchStats();

static void drawGamePanelInfo() {
  const int x = PanelX;
  const int w = 320 - x - 4;

  // Clear the complete changing information area, including font overhang.
  // The depth/NPS strip is redrawn separately immediately afterwards.
  tft.fillRect(x, 60, w, 85, ColorPanel);

  const char *turnText = gameOver ? "Game over"
                       : searchInFlight
                           ? (searchMode == SearchMode::Evaluation ? "Evaluating" : "Thinking")
                       : sideToMove() == playerColor ? "Your move" : "Engine";
  drawPanelText(turnText, x + 1, 62, w - 2, 16,
                searchInFlight ? ColorOrange : ColorText,
                &fonts::FreeSansBold9pt7b);

  const int material = materialBalanceForPlayer();
  char materialText[20];
  snprintf(materialText, sizeof(materialText), "Material %+d", material);
  drawPanelText(materialText, x, 113, w, 14,
                material > 0 ? ColorOrange : ColorText,
                &fonts::FreeSansBold9pt7b);

  drawPanelText(statusText, x + 1, 129, w - 2, 14,
                ColorMuted, &fonts::FreeSans9pt7b);
  drawButton(x + 2, 147, w - 4, 25, "HINT", false,
              ChessGuiRules::hintEnabled(
                searchInFlight, gameOver, sideToMove(), playerColor),
              &fonts::FreeSansBold9pt7b, 2);
  drawButton(x + 2, 177, w - 4, 25, "UNDO", false,
              ChessGuiRules::undoEnabled(gameHistory.size()),
             &fonts::FreeSansBold9pt7b, 2);
  drawButton(x + 2, 207, w - 4, 25, "NEW", false, true,
             &fonts::FreeSansBold9pt7b, 2);

  drawGamePanelSearchStats();
  refreshEvaluationBar();
}

static bool parseWdl(std::string_view text, int &win, int &draw, int &loss) {
  size_t pos = 0;
  auto parseValue = [&text, &pos](int &value) {
    while (pos < text.size() && isspace((unsigned char)text[pos]))
      ++pos;
    if (pos >= text.size() || !isdigit((unsigned char)text[pos]))
      return false;
    value = 0;
    while (pos < text.size() && isdigit((unsigned char)text[pos])) {
      value = value * 10 + (text[pos] - '0');
      if (value > 1000)
        return false;
      ++pos;
    }
    return true;
  };

  if (!parseValue(win) || !parseValue(draw) || !parseValue(loss))
    return false;
  while (pos < text.size() && isspace((unsigned char)text[pos]))
    ++pos;
  return pos == text.size() && win + draw + loss == 1000;
}

static void drawGamePanelSearchStats() {
  const int x = PanelX;
  const int w = 320 - x - 4;

  // Clear both changing search rows together; FreeSans glyphs have vertical
  // overhang beyond the nominal row height.
  tft.fillRect(x, 80, w, 33, ColorPanel);

  int uiDepth;
  uint32_t uiNps;
  portENTER_CRITICAL(&searchMux);
  uiDepth = searchMailbox.depth;
  uiNps = searchMailbox.nps;
  portEXIT_CRITICAL(&searchMux);

  char depthText[24];
  snprintf(depthText, sizeof(depthText), "Depth %d", uiDepth);
  drawPanelText(depthText, x, 82, w, 14, ColorMuted, &fonts::FreeSans9pt7b);

  char npsText[24];
  if (uiNps >= 1000)
    snprintf(npsText, sizeof(npsText), "%.1fk N/s", uiNps / 1000.0f);
  else
    snprintf(npsText, sizeof(npsText), "%lu N/s", (unsigned long)uiNps);
  drawPanelText(npsText, x, 97, w, 14, ColorMuted, &fonts::FreeSans9pt7b);
}

static void drawGamePanel() {
  const int x = PanelX;
  const int w = 320 - x - 4;
  tft.fillRect(x, BoardY, w, 224, ColorPanel);

  drawCenteredText("Stockfish", x, 25, w, 18,
                   ColorText, &fonts::FreeSansBold9pt7b);
  char levelText[28];
  snprintf(levelText, sizeof(levelText), "L%d  Book %s", skillLevel,
           openingBookEnabled ? "ON" : "OFF");
  drawCenteredText(levelText, x, 44, w, 14,
                   ColorOrange, &fonts::FreeSansBold9pt7b);
  drawGamePanelInfo();
}

static void renderGame() {
  screen = Screen::Game;
  tft.fillScreen(ColorBackground);
  drawBoard();
  drawBoardCoordinates();
  drawGamePanel();
}

static void renderPromotionChooser() {
  screen = Screen::Promotion;

  // Modal dialog over the current board. The board is fully redrawn after a
  // choice or cancellation, so no framebuffer or partial overlay bookkeeping
  // is required.
  constexpr int DialogX = 14;
  constexpr int DialogY = 66;
  constexpr int DialogW = 292;
  constexpr int DialogH = 112;
  tft.fillRoundRect(DialogX, DialogY, DialogW, DialogH, 10, ColorPanel);
  tft.drawRoundRect(DialogX, DialogY, DialogW, DialogH, 10, ColorOrange);
  drawCenteredText("Promote pawn", DialogX + 6, DialogY + 5,
                   DialogW - 12, 25, ColorOrange,
                   &fonts::FreeSansBold9pt7b);

  static const char *labels[] = { "QUEEN", "ROOK", "BISHOP", "KNIGHT" };
  static constexpr int buttonX[] = { 22, 91, 160, 229 };
  for (int i = 0; i < 4; ++i)
    drawButton(buttonX[i], 102, 64, 50, labels[i], false, true,
               &fonts::FreeSansBold9pt7b);

  drawButton(112, 157, 96, 18, "CANCEL", false, true,
             &fonts::FreeSans9pt7b);
}

static void renderMenu() {
  screen = Screen::Menu;
  tft.fillScreen(ColorBackground);

  drawCenteredText("New Game", 20, 12, 280, 40,
                   ColorOrange, &fonts::FreeSansBold12pt7b);
  drawTextAt("Play as", 31, 59, ColorMuted, &fonts::FreeSans9pt7b);

  drawButton(28, 87, 128, 36, "White", playerColor == 'w');
  drawButton(164, 87, 128, 36, "Black", playerColor == 'b');

  char bookText[32];
  snprintf(bookText, sizeof(bookText), "Opening book: %s",
           openingBookEnabled ? "ON" : "OFF");
  drawButton(28, 127, 264, 27, bookText, openingBookEnabled);

  drawTextAt("Stockfish level", 31, 157,
             ColorMuted, &fonts::FreeSans9pt7b);

  drawButton(30, 175, 50, 29, "-");
  char levelText[12];
  snprintf(levelText, sizeof(levelText), "%d", selectedSkillLevel);
  drawCenteredText(levelText, 94, 175, 132, 29,
                   ColorText, &fonts::FreeSansBold12pt7b);
  drawButton(240, 175, 50, 29, "+");

  drawButton(28, 207, 128, 30, "Start");
  drawButton(164, 207, 128, 30, "Cancel");
  drawButton(230, 5, 86, 26, "CALIB.", false, true,
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

static uint32_t savedGameChecksum(const uint8_t *blob, size_t bytes,
                                  size_t headerBytes, size_t checksumOffset) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < checksumOffset; ++i) {
    hash ^= blob[i];
    hash *= 16777619u;
  }
  for (size_t i = checksumOffset + sizeof(uint32_t); i < headerBytes; ++i) {
    hash ^= blob[i];
    hash *= 16777619u;
  }
  for (size_t i = headerBytes; i < bytes; ++i) {
    hash ^= blob[i];
    hash *= 16777619u;
  }
  return hash;
}

static uint8_t *allocateSavedGameBlob(size_t bytes) {
  uint8_t *blob = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!blob) blob = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return blob;
}

static void discardSavedGame() {
  Preferences preferences;
  if (preferences.begin("sf-gui", false)) { preferences.remove("game"); preferences.end(); }
}

static bool saveSkillSettingToNvs() {
  Preferences preferences;
  if (!preferences.begin("sf-gui", false)) {
    Serial.println("[SETTINGS] ERROR: could not open NVS namespace sf-gui");
    return false;
  }
  const size_t written = preferences.putUChar("skill", uint8_t(selectedSkillLevel));
  preferences.end();
  if (written != sizeof(uint8_t)) {
    Serial.println("[SETTINGS] ERROR: skill level was not saved to NVS");
    return false;
  }
  Serial.printf("[SETTINGS] Saved next-game skill level %d to NVS\n", selectedSkillLevel);
  return true;
}

static bool loadSkillSettingFromNvs() {
  Preferences preferences;
  if (!preferences.begin("sf-gui", true)) {
    Serial.println("[SETTINGS] NVS namespace unavailable; using default skill level");
    return false;
  }
  const bool found = preferences.isKey("skill");
  const uint8_t storedSkill = found ? preferences.getUChar("skill", 0) : 0;
  preferences.end();

  if (!found) {
    Serial.printf("[SETTINGS] No saved skill level; using default Level %d\n", skillLevel);
    return false;
  }
  if (storedSkill < 1 || storedSkill > 20) {
    Serial.printf("[SETTINGS] Invalid saved skill level %u; using default Level %d\n",
                  unsigned(storedSkill), skillLevel);
    return false;
  }

  selectedSkillLevel = storedSkill;
  Serial.printf("[SETTINGS] Restored next-game skill level %d from NVS\n", selectedSkillLevel);
  return true;
}

static bool saveOpeningBookSettingToNvs() {
  Preferences preferences;
  if (!preferences.begin("sf-gui", false)) {
    Serial.println("[SETTINGS] ERROR: could not open NVS namespace sf-gui");
    return false;
  }
  const size_t written = preferences.putBool("book", openingBookEnabled);
  preferences.end();
  if (written != sizeof(uint8_t)) {
    Serial.println("[SETTINGS] ERROR: opening-book setting was not saved to NVS");
    return false;
  }
  Serial.printf("[SETTINGS] Saved opening book %s to NVS\\n",
                openingBookEnabled ? "ON" : "OFF");
  return true;
}

static bool loadOpeningBookSettingFromNvs() {
  Preferences preferences;
  if (!preferences.begin("sf-gui", true)) {
    Serial.println("[SETTINGS] NVS namespace unavailable; using default opening-book setting ON");
    openingBookEnabled = true;
    return false;
  }
  const bool found = preferences.isKey("book");
  openingBookEnabled = found ? preferences.getBool("book", true) : true;
  preferences.end();

  Serial.printf("[SETTINGS] %s opening book\\n",
                found ? (openingBookEnabled ? "Restored ON for" : "Restored OFF for")
                      : "No saved setting; default ON for");
  return found;
}

static void setOpeningBookEnabled(bool enabled) {
  openingBookEnabled = enabled;
  const bool saved = saveOpeningBookSettingToNvs();
  Serial.printf("[BOOK] Opening book %s; %s\\n",
                openingBookEnabled ? "ENABLED" : "DISABLED",
                saved ? "setting applies to the next engine move"
                      : "NVS save failed; current runtime setting still applies");
  if (screen == Screen::Menu)
    renderMenu();
  else if (screen == Screen::Game)
    drawGamePanel();
}

static bool saveGameToNvs() {
  if (!hasGame || gameBaseFen.empty() || gameBaseFen.size() > SavedGameMaxFenBytes)
    return false;

  const size_t moveCount = gameHistory.size();
  const size_t totalBytes = sizeof(SavedGameHeader) + gameBaseFen.size()
                          + moveCount * sizeof(uint16_t);
  uint8_t *blob = allocateSavedGameBlob(totalBytes);
  if (!blob) {
    Serial.println("[GAME] ERROR: no memory for NVS save buffer");
    return false;
  }

  SavedGameHeader header{};
  header.magic = SavedGameMagic;
  header.version = SavedGameVersion;
  header.playerColor = uint8_t(playerColor);
  header.skillLevel = uint8_t(skillLevel);
  header.moveCount = uint32_t(moveCount);
  header.baseFenLength = uint16_t(gameBaseFen.size());
  memcpy(blob, &header, sizeof(header));
  memcpy(blob + sizeof(header), gameBaseFen.data(), gameBaseFen.size());
  if (moveCount)
    memcpy(blob + sizeof(header) + gameBaseFen.size(),
           gameHistory.packedData(), moveCount * sizeof(uint16_t));

  header.checksum = savedGameChecksum(blob, totalBytes, sizeof(header), 16);
  memcpy(blob, &header, sizeof(header));

  Preferences preferences;
  if (!preferences.begin("sf-gui", false)) {
    heap_caps_free(blob);
    Serial.println("[GAME] ERROR: could not open NVS namespace sf-gui");
    return false;
  }
  const size_t written = preferences.putBytes("game", blob, totalBytes);
  preferences.end();
  heap_caps_free(blob);

  const bool saved = written == totalBytes;
  if (!saved)
    Serial.printf("[GAME] ERROR: NVS save incomplete (%u/%u bytes)\n",
                  (unsigned)written, (unsigned)totalBytes);
  else
    Serial.printf("[GAME] Saved %u plies and %u FEN bytes to NVS (%u bytes)\n",
                  (unsigned)moveCount, (unsigned)gameBaseFen.size(), (unsigned)totalBytes);
  return saved;
}

static bool loadSavedGameFromNvs(bool hasSavedSkillSetting) {
  Preferences preferences;
  if (!preferences.begin("sf-gui", true)) {
    Serial.println("[GAME] NVS namespace unavailable; starting without saved game");
    return false;
  }
  const size_t bytes = preferences.getBytesLength("game");
  const size_t maxBytes = sizeof(SavedGameHeader) + SavedGameMaxFenBytes
                        + GameHistory::MaxMoves * sizeof(uint16_t);
  if (!bytes) {
    preferences.end();
    Serial.println("[GAME] No saved game found");
    return false;
  }
  if (bytes < sizeof(SavedGameHeaderV1) || bytes > maxBytes) {
    preferences.end();
    discardSavedGame();
    Serial.println("[GAME] Saved game has invalid size; discarded");
    return false;
  }

  uint8_t *blob = allocateSavedGameBlob(bytes);
  if (!blob) {
    preferences.end();
    Serial.println("[GAME] ERROR: no memory to load saved game");
    return false;
  }
  const size_t read = preferences.getBytes("game", blob, bytes);
  preferences.end();
  if (read != bytes) {
    heap_caps_free(blob);
    discardSavedGame();
    Serial.println("[GAME] Saved game read failed; discarded");
    return false;
  }

  uint32_t magic = 0;
  uint16_t version = 0;
  memcpy(&magic, blob, sizeof(magic));
  memcpy(&version, blob + sizeof(magic), sizeof(version));

  SavedGameHeaderV1 headerV1{};
  SavedGameHeader header{};
  std::string candidateFen = StartFen;
  const uint8_t *packedBytes = nullptr;
  uint32_t moveCount = 0;
  bool legacyV1 = false;
  bool valid = magic == SavedGameMagic;

  if (valid && version == 1) {
    legacyV1 = true;
    memcpy(&headerV1, blob, sizeof(headerV1));
    const size_t expected = sizeof(headerV1) + size_t(headerV1.moveCount) * sizeof(uint16_t);
    valid = (headerV1.playerColor == 'w' || headerV1.playerColor == 'b')
         && headerV1.skillLevel >= 1 && headerV1.skillLevel <= 20
         && headerV1.moveCount <= GameHistory::MaxMoves && expected == bytes
         && savedGameChecksum(blob, bytes, sizeof(headerV1), 12) == headerV1.checksum;
    moveCount = headerV1.moveCount;
    packedBytes = blob + sizeof(headerV1);
  } else if (valid && version == SavedGameVersion && bytes >= sizeof(header)) {
    memcpy(&header, blob, sizeof(header));
    const size_t expected = sizeof(header) + size_t(header.baseFenLength)
                          + size_t(header.moveCount) * sizeof(uint16_t);
    valid = (header.playerColor == 'w' || header.playerColor == 'b')
         && header.skillLevel >= 1 && header.skillLevel <= 20
         && header.moveCount <= GameHistory::MaxMoves
         && header.baseFenLength > 0 && header.baseFenLength <= SavedGameMaxFenBytes
         && expected == bytes
         && savedGameChecksum(blob, bytes, sizeof(header), 16) == header.checksum;
    if (valid) {
      const char *fenData = reinterpret_cast<const char *>(blob + sizeof(header));
      valid = memchr(fenData, '\0', header.baseFenLength) == nullptr;
      if (valid)
        candidateFen.assign(fenData, header.baseFenLength);
      moveCount = header.moveCount;
      packedBytes = blob + sizeof(header) + header.baseFenLength;
    }
  } else {
    valid = false;
  }

  const uint8_t savedPlayer = legacyV1 ? headerV1.playerColor : header.playerColor;
  const uint8_t savedSkill = legacyV1 ? headerV1.skillLevel : header.skillLevel;
  GameHistory candidateHistory;
  if (valid)
    valid = candidateHistory.assignPackedBytes(packedBytes, moveCount);
  heap_caps_free(blob);

  if (valid) {
    const std::vector<std::string> moves = candidateHistory.toUciMoves();
    const auto error = engine->set_position(candidateFen, moves);
    if (error.has_value()) {
      Serial.printf("[GAME] Saved position rejected: %s\n", error->what());
      valid = false;
    }  }

  if (!valid) {
    gameHistory.clear();
    gameBaseFen = StartFen;
    const std::vector<std::string> noMoves;
    engine->set_position(StartFen, noMoves);
    syncEnginePosition();
    discardSavedGame();
    Serial.println("[GAME] Saved game validation failed; discarded");
    return false;
  }

  gameHistory.swap(candidateHistory);
  gameBaseFen = candidateFen;
  playerColor = char(savedPlayer);
  skillLevel = savedSkill;
  if (!hasSavedSkillSetting)
    selectedSkillLevel = savedSkill;
  setSkillOption();
  currentFen = engine->fen();
  loadFenBoard(currentFen);
  updateLastMove();
  hasGame = true;
  updateGameStatusFromPosition();
  Serial.printf("[GAME] Restored %u plies from %s FEN (%s history storage)\n",
                (unsigned)gameHistory.size(), legacyV1 ? "legacy startpos" : "saved",
                gameHistory.usesPsram() ? "PSRAM" : "internal RAM");
  if (legacyV1)
    saveGameToNvs();
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

static void handleSearchResult(const char *bestMove, const char *actor = "Stockfish");

static void startSearch(SearchMode mode) {
  if (!engine || searchInFlight || gameOver)
    return;

  if (mode == SearchMode::Hint && sideToMove() != playerColor)
    return;

  const int previousHintFrom = hintFrom;
  const int previousHintTo = hintTo;
  hintFrom = hintTo = -1;
  hintText[0] = '\0';
  if (screen == Screen::Game) {
    drawBoardSquare(previousHintFrom);
    drawBoardSquare(previousHintTo);
  }
  searchMode = mode;
  const char searchSide = sideToMove();

  portENTER_CRITICAL(&searchMux);
  searchMailbox.depth = 0;
  searchMailbox.nps = 0;
  searchMailbox.nodes = 0;
  searchMailbox.scoreSide = searchSide;
  searchMailbox.scoreKind = SearchScoreKind::None;
  searchMailbox.scoreValue = 0;
  searchMailbox.tablebaseWin = false;
  searchMailbox.scoreValid = false;
  searchMailbox.wdlWin = 0;
  searchMailbox.wdlDraw = 0;
  searchMailbox.wdlLoss = 0;
  searchMailbox.wdlValid = false;
  searchMailbox.infoDirty = true;
  searchMailbox.bestMoveReady = false;
  searchMailbox.bestMove[0] = '\0';
  portEXIT_CRITICAL(&searchMux);

  // Clear the previous position's evaluation immediately. An opening-book
  // move does not run Stockfish search and must not leave a stale bar behind.
  evalBarValid = false;
  if (screen == Screen::Game)
    refreshEvaluationBar();

  if (mode == SearchMode::EngineMove && !openingBookEnabled)
    Serial.println("[BOOK] Disabled; using Stockfish search");

  if (mode == SearchMode::EngineMove && openingBookEnabled && openingBook.ready()) {
    // Use Engine's live root position, not a FEN round-trip. Stockfish's FEN
    // omits en-passant squares that are pseudo-legal but pinned; Polyglot
    // deliberately hashes those files. This also avoids rebuilding a Position.
    const Stockfish::Position& position = engine->position();
    Stockfish::Move bookMove;
    PolyglotBook::SelectionStats stats;
    const uint64_t key = PolyglotBook::hashPosition(position);
    const uint64_t randomValue = (uint64_t(esp_random()) << 32) | esp_random();

    if (openingBook.chooseMove(position, randomValue, bookMove, &stats)) {
      const std::string uciMove = Stockfish::UCIEngine::move(bookMove, false);
      Serial.printf("[BOOK] HIT key=%016llX legal-entries=%lu total-weight=%llu move=%s\n",
                    (unsigned long long)key,
                    (unsigned long)stats.legalEntries,
                    (unsigned long long)stats.totalWeight,
                    uciMove.c_str());
      handleSearchResult(uciMove.c_str(), "Book");
      return;
    }

    Serial.printf("[BOOK] MISS key=%016llX; falling back to Stockfish search\n",
                  (unsigned long long)key);
  }

  Stockfish::Search::LimitsType limits;
  // Each search owns a fresh clock. The 1s display-refresh search happens
  // only on the human's turn; it is completed before a later engine move.
  // A regular engine move always receives its unchanged 2400ms budget.
  constexpr Stockfish::TimePoint EngineMoveTimeMs = 2400;
  constexpr Stockfish::TimePoint HintTimeMs = 1400;
  constexpr Stockfish::TimePoint EvaluationTimeMs = 1000;
  limits.movetime = mode == SearchMode::Hint ? HintTimeMs
                  : mode == SearchMode::Evaluation ? EvaluationTimeMs
                                                   : EngineMoveTimeMs;
  limits.startTime = Stockfish::now();

  searchInFlight = true;
  if (mode == SearchMode::Hint)
    setStatus("Finding hint");
  else if (mode == SearchMode::EngineMove)
    setStatus("Thinking");
  // Evaluation-only searches leave the position's normal status text intact.
  engine->go(limits);
  if (screen == Screen::Game)
    drawGamePanelInfo();
}

static void showCurrentGame() {
  renderGame();
  if (gameOver)
    return;

  if (sideToMove() != playerColor)
    startSearch(SearchMode::EngineMove);
  else
    startSearch(SearchMode::Evaluation);
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

static void logAcceptedMove(const char *actor, char movingSide, const char *uciMove) {
  Serial.printf("[MOVE] %s (%s): %s\n", actor,
                movingSide == 'w' ? "White" : "Black", uciMove);
}

static void handleSearchResult(const char *bestMove, const char *actor) {
  if (!bestMove || !bestMove[0] || strcmp(bestMove, "0000") == 0) {
    searchMode = SearchMode::None;
    if (!updateGameStatusFromPosition())
      setStatus("Search ended");
    if (screen == Screen::Game)
      drawGamePanelInfo();
    return;
  }

  if (searchMode == SearchMode::Evaluation) {
    // This search exists only to refresh the static evaluation. Its best move
    // is intentionally ignored; the current game position and history remain
    // untouched.
    searchMode = SearchMode::None;
    if (screen == Screen::Game)
      drawGamePanelInfo();
    return;
  }

  if (searchMode == SearchMode::Hint) {
    const int oldLastFrom = lastFrom, oldLastTo = lastTo;
    const int oldHintFrom = hintFrom, oldHintTo = hintTo;
    const int oldSelected = selectedSquare;
    char oldBoard[64];
    memcpy(oldBoard, board, sizeof(board));

    Serial.printf("[HINT] %s\n", bestMove);
    snprintf(hintText, sizeof(hintText), "%s", bestMove);
    hintFrom = squareFromUci(bestMove);
    hintTo = squareFromUci(bestMove + 2);
    char text[32];
    snprintf(text, sizeof(text), "Hint: %.5s", bestMove);
    setStatus(text);
    searchMode = SearchMode::None;
    if (screen == Screen::Game) {
      drawChangedBoardSquares(oldBoard, oldLastFrom, oldLastTo,
                              oldHintFrom, oldHintTo, oldSelected);
      drawGamePanelInfo();
    }
    return;
  }

  if (searchMode == SearchMode::EngineMove) {
    const char movingSide = sideToMove();
    const int oldLastFrom = lastFrom, oldLastTo = lastTo;
    const int oldHintFrom = hintFrom, oldHintTo = hintTo;
    const int oldSelected = selectedSquare;
    char oldBoard[64];
    memcpy(oldBoard, board, sizeof(board));

    if (!gameHistory.appendUci(bestMove)) {
      setStatus("History full");
    } else if (!syncEnginePosition()) {
      gameHistory.popBack();
      syncEnginePosition();
      setStatus("Engine move error");
    } else {
      logAcceptedMove(actor, movingSide, bestMove);
      updateGameStatusFromPosition();
      saveGameToNvs();
    }
    searchMode = SearchMode::None;
    if (screen == Screen::Game) {
      drawChangedBoardSquares(oldBoard, oldLastFrom, oldLastTo,
                              oldHintFrom, oldHintTo, oldSelected);
      drawGamePanelInfo();
    }
  }
}

static void logSearchEvaluation(const SearchMailbox &result) {
  if (!result.scoreValid)
    return;

  const int whiteSign = result.scoreSide == 'w' ? 1 : -1;
  char scoreText[40] = {};

  switch (result.scoreKind) {
    case SearchScoreKind::Centipawns: {
      const int whiteCp = result.scoreValue * whiteSign;
      snprintf(scoreText, sizeof(scoreText), "%+.2f", whiteCp / 100.0);
      break;
    }
    case SearchScoreKind::Mate: {
      const int whiteMatePlies = result.scoreValue * whiteSign;
      const int mateMoves = whiteMatePlies > 0 ? (whiteMatePlies + 1) / 2
                                               : whiteMatePlies / 2;
      snprintf(scoreText, sizeof(scoreText), "mate %+d", mateMoves);
      break;
    }
    case SearchScoreKind::Tablebase: {
      const bool whiteWins = result.scoreSide == 'w'
                           ? result.tablebaseWin : !result.tablebaseWin;
      snprintf(scoreText, sizeof(scoreText), "TB %s",
               whiteWins ? "win for White" : "win for Black");
      break;
    }
    default:
      return;
  }

  Serial.printf("[SEARCH] depth=%d score=%s (White POV) nodes=%llu nps=%lu",
                result.depth, scoreText,
                (unsigned long long)result.nodes,
                (unsigned long)result.nps);
  if (result.wdlValid) {
    const int whiteWin = result.scoreSide == 'w' ? result.wdlWin : result.wdlLoss;
    const int blackWin = result.scoreSide == 'w' ? result.wdlLoss : result.wdlWin;
    Serial.printf(" WDL(W/D/B)=%d/%d/%d", whiteWin, result.wdlDraw, blackWin);
  }
  Serial.println();
}

static void pollSearchMailbox() {
  bool infoDirty = false;
  bool bestMoveReady = false;
  char bestMove[16] = {};
  SearchMailbox infoSnapshot{};

  portENTER_CRITICAL(&searchMux);
  if (searchMailbox.infoDirty) {
    infoDirty = true;
    searchMailbox.infoDirty = false;
    infoSnapshot = searchMailbox;
  }
  if (searchMailbox.bestMoveReady) {
    bestMoveReady = true;
    memcpy(bestMove, searchMailbox.bestMove, sizeof(bestMove));
    searchMailbox.bestMoveReady = false;
  }
  portEXIT_CRITICAL(&searchMux);

  if (infoDirty && screen == Screen::Game) {
    evalBarValid = infoSnapshot.scoreValid;
    evalBarScoreKind = infoSnapshot.scoreKind;
    evalBarWhiteCp = 0;
    evalBarWhiteWins = false;
    const int whiteSign = infoSnapshot.scoreSide == 'w' ? 1 : -1;
    if (infoSnapshot.scoreKind == SearchScoreKind::Centipawns) {
      evalBarWhiteCp = infoSnapshot.scoreValue * whiteSign;
    } else if (infoSnapshot.scoreKind == SearchScoreKind::Mate) {
      evalBarWhiteWins = infoSnapshot.scoreValue * whiteSign > 0;
    } else if (infoSnapshot.scoreKind == SearchScoreKind::Tablebase) {
      evalBarWhiteWins = infoSnapshot.scoreSide == 'w'
                       ? infoSnapshot.tablebaseWin : !infoSnapshot.tablebaseWin;
    }
    drawGamePanelSearchStats();
    refreshEvaluationBar();
  }

  if (bestMoveReady) {
    engine->wait_for_search_finished();
    searchInFlight = false;

    SearchMailbox finalResult{};
    portENTER_CRITICAL(&searchMux);
    finalResult = searchMailbox;
    portEXIT_CRITICAL(&searchMux);
    logSearchEvaluation(finalResult);

    handleSearchResult(bestMove);
  }
}

static void startNewGame() {
  stopSearch();
  engine->search_clear();
  skillLevel = selectedSkillLevel;
  gameHistory.clear();
  gameBaseFen = StartFen;
  gameOver = false;
  clearLegalDestinations(false);
  selectedSquare = -1;
  hintFrom = hintTo = -1;
  hintText[0] = '\0';
  pendingPromotion.clear();
  searchMode = SearchMode::None;

  setSkillOption();
  saveSkillSettingToNvs();
  if (!syncEnginePosition()) {
    setStatus("Position error");
    renderMenu();
    return;
  }

  hasGame = true;
  setStatus("Ready");
  saveGameToNvs();
  Serial.printf("[GAME] New game started; player %s; Stockfish level %d\n",
                playerColor == 'w' ? "White" : "Black", skillLevel);
  renderGame();

  // When the human chooses Black, Stockfish makes the opening move.
  if (playerColor == 'b')
    startSearch(SearchMode::EngineMove);
  else
    startSearch(SearchMode::Evaluation);
}

static void undoMove() {
  if (searchInFlight)
    stopSearch();

  if (!ChessGuiRules::undoEnabled(gameHistory.size())) {
    setStatus("Nothing to undo");
    if (screen == Screen::Game)
      drawGamePanelInfo();
    return;
  }

  const int oldLastFrom = lastFrom, oldLastTo = lastTo;
  const int oldHintFrom = hintFrom, oldHintTo = hintTo;
  const int oldSelected = selectedSquare;
  char oldBoard[64];
  memcpy(oldBoard, board, sizeof(board));

  const size_t removeCount = ChessGuiRules::undoPliesToRemove(gameHistory.size());
  for (size_t i = 0; i < removeCount; ++i)
    gameHistory.popBack();

  gameOver = false;
  selectedSquare = -1;
  hintFrom = hintTo = -1;
  if (!syncEnginePosition()) {
    setStatus("Undo error");
    if (screen == Screen::Game)
      renderGame();
    return;
  }

  clearLegalDestinations();
  setStatus("Move undone");
  saveGameToNvs();
  if (screen == Screen::Game) {
    drawChangedBoardSquares(oldBoard, oldLastFrom, oldLastTo,
                            oldHintFrom, oldHintTo, oldSelected);
    drawGamePanelInfo();
  }

  if (sideToMove() != playerColor)
    startSearch(SearchMode::EngineMove);
  else
    startSearch(SearchMode::Evaluation);
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

static void applyMove(int from, int to, char promotion) {
  if (from < 0 || to < 0 || from == to)
    return;

  const bool wasPromotion = promotion != '\0';
  const char movingSide = sideToMove();
  const int oldLastFrom = lastFrom, oldLastTo = lastTo;
  const int oldHintFrom = hintFrom, oldHintTo = hintTo;
  const int oldSelected = selectedSquare;
  char oldBoard[64];
  memcpy(oldBoard, board, sizeof(board));

  char move[8];
  uciSquareName(from, move);
  uciSquareName(to, move + 2);
  int length = 4;
  if (wasPromotion)
    move[length++] = promotion;
  move[length] = '\0';

  if (!gameHistory.appendUci(move)) {
    setStatus("History full");
    drawGamePanelInfo();
    return;
  }
  if (!syncEnginePosition()) {
    gameHistory.popBack();
    syncEnginePosition();
    selectedSquare = from;
    setStatus("Illegal move");
    if (wasPromotion) {
      screen = Screen::Game;
      renderGame();
    } else {
      drawChangedBoardSquares(oldBoard, oldLastFrom, oldLastTo,
                              oldHintFrom, oldHintTo, oldSelected);
      drawGamePanelInfo();
    }
    return;
  }

  logAcceptedMove("Player", movingSide, move);
  currentFen = engine->fen();
  loadFenBoard(currentFen);
  updateLastMove();
  selectedSquare = -1;
  hintFrom = hintTo = -1;
  hintText[0] = '\0';
  const bool terminalPosition = updateGameStatusFromPosition();
  saveGameToNvs();
  clearLegalDestinations(!wasPromotion);
  if (!terminalPosition && sideToMove() != playerColor)
    setStatus("Thinking");

  if (wasPromotion) {
    screen = Screen::Game;
    renderGame();
  } else {
    drawChangedBoardSquares(oldBoard, oldLastFrom, oldLastTo,
                            oldHintFrom, oldHintTo, oldSelected);
    drawGamePanelInfo();
  }

  if (!gameOver && sideToMove() != playerColor)
    startSearch(SearchMode::EngineMove);
}

static void attemptMove(int from, int to) {
  if (from < 0 || to < 0 || from == to)
    return;

  const char movingPiece = board[from];
  const bool promotionMove =
    ChessGuiRules::isPawnPromotion(movingPiece, to, playerColor);

  if (promotionMove) {
    pendingPromotion.begin(from, to);
    renderPromotionChooser();
    return;
  }

  applyMove(from, to, '\0');
}

static void cancelPromotion() {
  pendingPromotion.cancel(selectedSquare);
  setStatus("Promotion canceled");
  renderGame();
}

static void handlePromotionTouch(int x, int y) {
  static constexpr int buttonX[] = { 22, 91, 160, 229 };
  static constexpr char promotionMoves[] = { 'q', 'r', 'b', 'n' };

  if (y >= 102 && y < 152) {
    for (int i = 0; i < 4; ++i) {
      if (x >= buttonX[i] && x < buttonX[i] + 64) {
        int from = -1, to = -1;
        if (pendingPromotion.take(from, to))
          applyMove(from, to, promotionMoves[i]);
        return;
      }
    }
  }

  // The explicit Cancel button and any tap outside the four choices cancel
  // the modal without modifying the move list or the Stockfish position.
  cancelPromotion();
}

static int screenToSquare(int x, int y) {
  return ChessGuiRules::screenToSquare(
    x, y, BoardX, BoardY, BoardSize, SquareSize, playerColor);
}

static void handleBoardTouch(int x, int y) {
  if (searchInFlight)
    return;

  const int panelX = PanelX;

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

  if (!ChessGuiRules::acceptsBoardMoves(gameOver))
    return;

  const int square = screenToSquare(x, y);
  if (square < 0 || sideToMove() != playerColor)
    return;

  if (selectedSquare < 0) {
    const char piece = board[square];
    if (piece && ((playerColor == 'w' && isupper((unsigned char)piece))
                  || (playerColor == 'b' && islower((unsigned char)piece)))) {
      const int previousSelection = selectedSquare;
      selectedSquare = square;
      updateLegalDestinations(square);
      setStatus("Select destination");
      drawBoardSquare(previousSelection);
      drawBoardSquare(selectedSquare);
      drawGamePanelInfo();
    }
    return;
  }

  if (square == selectedSquare) {
    const int previousSelection = selectedSquare;
    selectedSquare = -1;
    clearLegalDestinations();
    setStatus("Your move");
    drawBoardSquare(previousSelection);
    drawGamePanelInfo();
    return;
  }

  const char targetPiece = board[square];
  const bool ownTarget = targetPiece
    && ((playerColor == 'w' && isupper((unsigned char)targetPiece))
        || (playerColor == 'b' && islower((unsigned char)targetPiece)));

  if (ownTarget) {
    const int previousSelection = selectedSquare;
    selectedSquare = square;
    updateLegalDestinations(square);
    setStatus("Select destination");
    drawBoardSquare(previousSelection);
    drawBoardSquare(selectedSquare);
    drawGamePanelInfo();
    return;
  }

  const int from = selectedSquare;
  attemptMove(from, square);
}

static void handleMenuTouch(int x, int y) {
  if (y >= 87 && y < 123) {
    if (x >= 28 && x < 156) {
      playerColor = 'w';
      renderMenu();
    } else if (x >= 164 && x < 292) {
      playerColor = 'b';
      renderMenu();
    }
    return;
  }

  if (y >= 127 && y < 154) {
    if (x >= 28 && x < 292)
      setOpeningBookEnabled(!openingBookEnabled);
    return;
  }

  if (y >= 175 && y < 204) {
    if (x >= 30 && x < 80 && selectedSkillLevel > 1) {
      --selectedSkillLevel;
      saveSkillSettingToNvs();
      renderMenu();
    } else if (x >= 240 && x < 290 && selectedSkillLevel < 20) {
      ++selectedSkillLevel;
      saveSkillSettingToNvs();
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
  if (hasGame) showCurrentGame();
  else renderMenu();
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
    case Screen::Promotion:
      handlePromotionTouch(x, y);
      break;
  }
}

static bool applySerialPosition(const std::string &baseFen, GameHistory &candidateHistory,
                                const char *moveActor = nullptr, const char *uciMove = nullptr,
                                char movingSide = '\0') {
  if (!engine)
    return false;
  if (baseFen.empty() || baseFen.size() > SavedGameMaxFenBytes) {
    Serial.printf("[SERIAL] ERROR: FEN must be 1..%u bytes to fit the saved-game format\n",
                  (unsigned)SavedGameMaxFenBytes);
    return false;
  }

  const std::vector<std::string> candidateMoves = candidateHistory.toUciMoves();
  const std::string previousBaseFen = gameBaseFen;
  const std::vector<std::string> previousMoves = gameHistory.toUciMoves();
  const bool previousHadGame = hasGame;

  stopSearch();
  const auto error = engine->set_position(baseFen, candidateMoves);
  if (error.has_value()) {
    Serial.printf("[SERIAL] Position rejected: %s\n", error->what());
    const auto restoreError = engine->set_position(previousBaseFen, previousMoves);
    if (restoreError.has_value()) {
      Serial.printf("[SERIAL] CRITICAL: cannot restore previous position: %s\n",
                    restoreError->what());
      hasGame = false;
      renderMenu();
      return false;
    }
    currentFen = engine->fen();
    loadFenBoard(currentFen);
    updateLastMove();
    updateGameStatusFromPosition();
    if (previousHadGame) {
      if (touchCalibrated) showCurrentGame();
      else beginTouchCalibration();
    }
    return false;
  }

  gameBaseFen = baseFen;
  gameHistory.swap(candidateHistory);
  currentFen = engine->fen();
  if (!loadFenBoard(currentFen)) {
    Serial.println("[SERIAL] ERROR: Stockfish returned an invalid board FEN");
    return false;
  }

  selectedSquare = -1;
  hintFrom = hintTo = -1;
  hintText[0] = '\0';
  pendingPromotion.clear();
  clearLegalDestinations(false);
  updateLastMove();
  hasGame = true;
  updateGameStatusFromPosition();
  saveGameToNvs();

  if (moveActor && uciMove)
    logAcceptedMove(moveActor, movingSide, uciMove);
  if (moveActor) {
    Serial.printf("[SERIAL] Move accepted; %u plies, %s to move\n",
                  (unsigned)gameHistory.size(),
                  sideToMove() == 'w' ? "White" : "Black");
  } else {
    Serial.printf("[SERIAL] POSITION ACCEPTED: %s; %u plies; %s to move\n",
                  baseFen == StartFen ? "startpos" : "FEN",
                  (unsigned)gameHistory.size(),
                  sideToMove() == 'w' ? "White" : "Black");
  }
  Serial.printf("[FEN] %s\n", currentFen.c_str());
  if (touchCalibrated)
    showCurrentGame();
  else
    beginTouchCalibration();
  return true;
}

static bool processPositionCommand(const std::string &line, bool shortFenCommand) {
  std::istringstream input(line);
  std::string keyword;
  input >> keyword;
  std::string form;
  if (shortFenCommand) {
    form = "fen";
  } else if (!(input >> form)) {
    Serial.println("[SERIAL] Usage: position startpos [moves ...] OR position fen <6 FEN fields> [moves ...]");
    return false;
  }

  std::transform(form.begin(), form.end(), form.begin(),
                 [](unsigned char ch) { return char(tolower(ch)); });
  std::string baseFen;
  if (form == "startpos") {
    baseFen = StartFen;
  } else if (form == "fen") {
    std::string fields[6];
    for (unsigned i = 0; i < 6; ++i) {
      if (!(input >> fields[i])) {
        Serial.println("[SERIAL] ERROR: FEN requires all six fields");
        return false;
      }
    }
    for (unsigned i = 0; i < 6; ++i) {
      if (i)
        baseFen += ' ';
      baseFen += fields[i];
    }
  } else {
    Serial.printf("[SERIAL] ERROR: unknown position format '%s'\n", form.c_str());
    return false;
  }

  GameHistory candidateHistory;
  std::string token;
  if (input >> token) {
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char ch) { return char(tolower(ch)); });
    if (token != "moves") {
      Serial.printf("[SERIAL] ERROR: expected 'moves', got '%s'\n", token.c_str());
      return false;
    }
    while (input >> token) {
      if (!candidateHistory.appendUci(token)) {
        Serial.printf("[SERIAL] ERROR: invalid UCI syntax or too many moves: '%s'\n",
                      token.c_str());
        return false;
      }
    }
  }
  return applySerialPosition(baseFen, candidateHistory);
}

static bool processSingleUciMove(std::string move) {
  if (!hasGame) {
    Serial.println("[SERIAL] ERROR: no active game; start a game or load a position first");
    return false;
  }
  if (gameOver) {
    Serial.println("[SERIAL] ERROR: game is over; start a new game or load another position");
    return false;
  }

  // UCI square names and promotion suffixes are normalized to lowercase.
  std::transform(move.begin(), move.end(), move.begin(),
                 [](unsigned char ch) { return char(tolower(ch)); });

  GameHistory candidateHistory;
  if (gameHistory.size()
      && !candidateHistory.assignPackedBytes(
           reinterpret_cast<const uint8_t *>(gameHistory.packedData()), gameHistory.size())) {
    Serial.println("[SERIAL] ERROR: could not copy current move history");
    return false;
  }
  if (!candidateHistory.appendUci(move)) {
    Serial.printf("[SERIAL] ERROR: expected a UCI move such as e2e4 or e7e8q, got '%s'\n",
                  move.c_str());
    return false;
  }

  const char movingSide = sideToMove();
  return applySerialPosition(gameBaseFen, candidateHistory,
                             "Serial", move.c_str(), movingSide);
}

static void printGameUciPosition() {
  if (!hasGame) { Serial.println("[GAME] No active game"); return; }
  Serial.printf("[GAME] %u plies; UCI position follows\n", (unsigned)gameHistory.size());
  if (gameBaseFen == StartFen) {
    Serial.print("position startpos");
  } else {
    Serial.print("position fen ");
    Serial.print(gameBaseFen.c_str());
  }
  if (!gameHistory.empty()) {
    Serial.print(" moves");
    char move[6];
    for (size_t i = 0; i < gameHistory.size(); ++i)
      if (gameHistory.formatMoveAt(i, move)) { Serial.print(' '); Serial.print(move); }
  }
  Serial.println();
  Serial.printf("[FEN] %s\n", currentFen.c_str());
}


static void printPgnToken(const std::string &token, size_t &column) {
  if (column && column + 1 + token.size() > 78) {
    Serial.println();
    column = 0;
  }
  if (column) {
    Serial.print(' ');
    ++column;
  }
  Serial.print(token.c_str());
  column += token.size();
}

static char pgnPieceLetter(Stockfish::PieceType pieceType) {
  switch (pieceType) {
    case Stockfish::KNIGHT: return 'N';
    case Stockfish::BISHOP: return 'B';
    case Stockfish::ROOK: return 'R';
    case Stockfish::QUEEN: return 'Q';
    case Stockfish::KING: return 'K';
    default: return '\0';
  }
}

static bool formatSanMove(Stockfish::Position &position, Stockfish::Move move,
                          Stockfish::StateInfo &nextState, std::string &san) {
  if (!move.is_ok())
    return false;

  const Stockfish::Square from = move.from_sq();
  const Stockfish::Square to = move.to_sq();
  const Stockfish::Piece movingPiece = position.moved_piece(move);
  const Stockfish::PieceType movingType = Stockfish::type_of(movingPiece);
  const bool capture = position.capture(move);
  san.clear();

  if (move.type_of() == Stockfish::CASTLING) {
    san = Stockfish::file_of(to) > Stockfish::file_of(from) ? "O-O" : "O-O-O";
  } else {
    if (movingType == Stockfish::PAWN) {
      if (capture)
        san.push_back(char('a' + int(Stockfish::file_of(from))));
    } else {
      const char pieceLetter = pgnPieceLetter(movingType);
      if (!pieceLetter)
        return false;
      san.push_back(pieceLetter);

      bool alternativeFound = false;
      bool alternativeOnSameFile = false;
      bool alternativeOnSameRank = false;
      for (const Stockfish::Move alternative : Stockfish::MoveList<Stockfish::LEGAL>(position)) {
        if (alternative == move || alternative.to_sq() != to
            || Stockfish::type_of(position.moved_piece(alternative)) != movingType)
          continue;
        alternativeFound = true;
        if (Stockfish::file_of(alternative.from_sq()) == Stockfish::file_of(from))
          alternativeOnSameFile = true;
        if (Stockfish::rank_of(alternative.from_sq()) == Stockfish::rank_of(from))
          alternativeOnSameRank = true;
      }

      if (alternativeFound) {
        if (!alternativeOnSameFile || (alternativeOnSameFile && alternativeOnSameRank))
          san.push_back(char('a' + int(Stockfish::file_of(from))));
        if (alternativeOnSameFile)
          san.push_back(char('1' + int(Stockfish::rank_of(from))));
      }
    }

    if (capture)
      san.push_back('x');

    san += Stockfish::UCIEngine::square(to);

    if (move.type_of() == Stockfish::PROMOTION) {
      const char promotionLetter = pgnPieceLetter(move.promotion_type());
      if (!promotionLetter)
        return false;
      san.push_back('=');
      san.push_back(promotionLetter);
    }
  }

  position.do_move(move, nextState);
  if (position.checkers()) {
    const Stockfish::MoveList<Stockfish::LEGAL> replies(position);
    san.push_back(replies.size() == 0 ? '#' : '+');
  }
  return true;
}

static bool streamPgnMoves(bool emitOutput) {
  std::istringstream fenInput(gameBaseFen);
  std::string placement, sideField, castlingField, epField, halfmoveField, fullmoveField;
  if (!(fenInput >> placement >> sideField >> castlingField
                 >> epField >> halfmoveField >> fullmoveField)
      || (sideField != "w" && sideField != "b")) {
    Serial.println("[PGN] ERROR: stored base FEN cannot be parsed");
    return false;
  }

  unsigned moveNumber = 0;
  for (unsigned char ch : fullmoveField) {
    if (ch < '0' || ch > '9') {
      Serial.println("[PGN] ERROR: invalid FEN fullmove number");
      return false;
    }
    if (moveNumber <= 1000000)
      moveNumber = moveNumber * 10 + (ch - '0');
  }
  if (moveNumber == 0)
    moveNumber = 1;

  bool whiteToMove = sideField == "w";
  bool firstPly = true;
  size_t outputColumn = 0;
  std::string fen = gameBaseFen;
  std::string san;
  char uciMove[6];
  Stockfish::Position position;
  Stockfish::StateInfo currentState;
  Stockfish::StateInfo nextState;

  for (size_t i = 0; i < gameHistory.size(); ++i) {
    if (!gameHistory.formatMoveAt(i, uciMove)) {
      Serial.printf("[PGN] ERROR: cannot decode move %u\n", unsigned(i + 1));
      return false;
    }

    const auto fenError = position.set(fen, false, &currentState);
    if (fenError.has_value()) {
      Serial.printf("[PGN] ERROR: invalid position before ply %u: %s\n",
                    unsigned(i + 1), fenError->what());
      return false;
    }

    const Stockfish::Move move = Stockfish::UCIEngine::to_move(position, uciMove);
    if (move == Stockfish::Move::none()) {
      Serial.printf("[PGN] ERROR: illegal stored move at ply %u: %s\n",
                    unsigned(i + 1), uciMove);
      return false;
    }

    if (!formatSanMove(position, move, nextState, san)) {
      Serial.printf("[PGN] ERROR: SAN conversion failed at ply %u: %s\n",
                    unsigned(i + 1), uciMove);
      return false;
    }

    if (emitOutput) {
      if (whiteToMove)
        printPgnToken(std::to_string(moveNumber) + ".", outputColumn);
      else if (firstPly)
        printPgnToken(std::to_string(moveNumber) + "...", outputColumn);
      printPgnToken(san, outputColumn);
    }

    fen = position.fen();
    if (!whiteToMove)
      ++moveNumber;
    whiteToMove = !whiteToMove;
    firstPly = false;
  }

  if (emitOutput)
    Serial.println();
  return true;
}

static const char *pgnGameResult() {
  if (!gameOver || !engine)
    return "*";

  const Stockfish::Engine::GameStatus gameStatus = engine->game_status();
  if (!gameStatus.hasLegalMoves) {
    if (gameStatus.inCheck)
      return sideToMove() == 'w' ? "0-1" : "1-0";
    return "1/2-1/2";
  }
  if (gameStatus.drawByFiftyMove || gameStatus.drawByRepetition
      || gameStatus.insufficientMaterial)
    return "1/2-1/2";
  return "*";
}

static void exportPgn() {
  if (!hasGame) {
    Serial.println("[PGN] No active game");
    return;
  }

  // Validate the complete move list before writing any PGN headers.
  if (!streamPgnMoves(false))
    return;

  const char *result = pgnGameResult();
  const char *whiteName = playerColor == 'w' ? "Player" : "Stockfish 19";
  const char *blackName = playerColor == 'b' ? "Player" : "Stockfish 19";

  Serial.println("[Event \"Stockfish ESP32-S3 Game\"]");
  Serial.println("[Site \"Freenove FNK0104B\"]");
  Serial.println("[Date \"????.??.??\"]");
  Serial.println("[Round \"-\"]");
  if (playerColor == 'w') {
    Serial.printf("[White \"%s\"]\n", whiteName);
    Serial.printf("[Black \"%s (Level %d)\"]\n", blackName, skillLevel);
  } else {
    Serial.printf("[White \"%s (Level %d)\"]\n", whiteName, skillLevel);
    Serial.printf("[Black \"%s\"]\n", blackName);
  }
  Serial.printf("[Result \"%s\"]\n", result);
  Serial.printf("[PlyCount \"%u\"]\n", unsigned(gameHistory.size()));
  if (gameBaseFen != StartFen) {
    Serial.println("[SetUp \"1\"]");
    Serial.printf("[FEN \"%s\"]\n", gameBaseFen.c_str());
  }
  Serial.println();

  if (!streamPgnMoves(true))
    return;
  Serial.println(result);
}

static void executeSerialCommand() {
  if (serialCommandOverflow) {
    Serial.printf("[SERIAL] ERROR: command exceeded %u bytes; discarded\n",
                  (unsigned)(SerialCommandCapacity - 1));
    serialCommandLength = 0;
    serialCommandOverflow = false;
    return;
  }

  serialCommand[serialCommandLength] = '\0';
  const std::string line(serialCommand, serialCommandLength);
  if (!line.empty())
    Serial.printf("[SERIAL] RX (%u bytes): %s\n",
                  (unsigned)line.size(), line.c_str());
  std::istringstream input(line);
  std::string command;
  input >> command;
  std::transform(command.begin(), command.end(), command.begin(),
                 [](unsigned char ch) { return char(tolower(ch)); });

  if (command == "moves" || command == "uci") {
    printGameUciPosition();
  } else if (command == "undo") {
    if (!hasGame) {
      Serial.println("[SERIAL] ERROR: no active game");
    } else {      const size_t previousPlies = gameHistory.size();
      undoMove();
      const size_t removedPlies = previousPlies - gameHistory.size();
      if (removedPlies)
        Serial.printf("[UNDO] Removed %u plies; %u remain\n",
                      unsigned(removedPlies), unsigned(gameHistory.size()));
      else
        Serial.println("[UNDO] No move was removed");
    }
  } else if (command == "hint") {
    if (!hasGame)
      Serial.println("[SERIAL] ERROR: no active game");
    else if (gameOver)
      Serial.println("[SERIAL] ERROR: game is over");
    else if (searchInFlight)
      Serial.println("[SERIAL] ERROR: a search is already running");
    else if (sideToMove() != playerColor)
      Serial.println("[SERIAL] ERROR: it is not the player's turn");
    else
      startSearch(SearchMode::Hint);
  } else if (command == "stop") {
    if (!searchInFlight)
      Serial.println("[SERIAL] No search is running");
    else {
      stopSearch();
      setStatus("Search stopped");
      if (screen == Screen::Game)
        drawGamePanelInfo();
      Serial.println("[SERIAL] Search stopped");
    }
  } else if (command == "level") {
    std::string levelText;
    std::string extra;
    if (!(input >> levelText)) {
      Serial.printf("[SETTINGS] Next game: Level %d; current game: Level %d\n",
                    selectedSkillLevel, skillLevel);
    } else {
      int requestedLevel = 0;
      bool validLevel = !levelText.empty();
      for (unsigned char ch : levelText) {
        if (ch < '0' || ch > '9') {
          validLevel = false;
          break;
        }
        requestedLevel = requestedLevel * 10 + (ch - '0');
        if (requestedLevel > 20) {
          validLevel = false;
          break;
        }
      }
      if (!validLevel || requestedLevel < 1 || requestedLevel > 20 || (input >> extra)) {
        Serial.println("[SERIAL] Usage: LEVEL [1..20]");
      } else {
        selectedSkillLevel = requestedLevel;
        if (saveSkillSettingToNvs())
          Serial.printf("[SETTINGS] Next game will use Level %d\n", selectedSkillLevel);
      }
    }
  } else if (command == "book") {
    std::string requestedState;
    std::string extra;
    if (!(input >> requestedState)) {
      Serial.printf("[BOOK] Opening book is %s%s\\n",
                    openingBookEnabled ? "ON" : "OFF",
                    openingBookEnabled && !openingBook.ready()
                      ? " (image unavailable; Stockfish fallback)" : "");
    } else {
      std::transform(requestedState.begin(), requestedState.end(), requestedState.begin(),
                     [](unsigned char ch) { return char(tolower(ch)); });
      if ((requestedState != "on" && requestedState != "off") || (input >> extra)) {
        Serial.println("[SERIAL] Usage: BOOK [ON|OFF]");
      } else {
        setOpeningBookEnabled(requestedState == "on");
      }
    }
  } else if (command == "pgn" || command == "exportpgn") {
    exportPgn();
  } else if (command == "position") {
    processPositionCommand(line, false);
  } else if (command == "fen") {
    processPositionCommand(line, true);
  } else if (command == "newgame") {
    std::string argument;
    int requestedSkill = selectedSkillLevel;
    char requestedPlayer = playerColor;
    bool skillSpecified = false;
    bool playerSpecified = false;
    bool valid = true;

    // Accept either ordering: NEWGAME 12 BLACK or NEWGAME BLACK 12.
    while (input >> argument) {
      std::transform(argument.begin(), argument.end(), argument.begin(),
                     [](unsigned char ch) { return char(tolower(ch)); });

      if (argument == "white" || argument == "black") {
        if (playerSpecified) {
          valid = false;
          break;
        }
        playerSpecified = true;
        requestedPlayer = argument == "white" ? 'w' : 'b';
        continue;
      }

      if (skillSpecified || argument.empty()) {
        valid = false;
        break;
      }

      int parsedSkill = 0;
      for (unsigned char ch : argument) {
        if (ch < '0' || ch > '9') {
          valid = false;
          break;
        }
        parsedSkill = parsedSkill * 10 + (ch - '0');
        if (parsedSkill > 20) {
          valid = false;
          break;
        }
      }
      if (!valid || parsedSkill < 1 || parsedSkill > 20) {
        valid = false;
        break;
      }
      skillSpecified = true;
      requestedSkill = parsedSkill;
    }

    if (!valid) {
      Serial.println("[SERIAL] Usage: NEWGAME [1..20] [WHITE|BLACK] (colour may also be given alone)");
    } else {
      selectedSkillLevel = requestedSkill;
      playerColor = requestedPlayer;
      startNewGame();
    }
  } else if (command == "help") {
    Serial.println("Commands:");
    Serial.println("  position startpos [moves e2e4 ...]");
    Serial.println("  position fen <6 FEN fields> [moves e2e4 ...]");
    Serial.println("  fen <6 FEN fields>  (shorthand)");
    Serial.println("  moves (or uci)  print current position as a UCI command");
    Serial.println("  e2e4 [or e7e8q]  apply one legal UCI move to the current game");
    Serial.println("  UNDO  undo the last move pair (or the only move)");
    Serial.println("  HINT  ask Stockfish for a move suggestion");
    Serial.println("  STOP  stop the current search");
    Serial.println("  LEVEL [1..20]  show/set skill for the next game");
    Serial.println("  BOOK [ON|OFF]  show or set opening-book usage");
    Serial.println("  PGN  export the current game in PGN/SAN format");
    Serial.println("  NEWGAME [1..20] [WHITE|BLACK]  start a new game");
    Serial.println("  NEWGAME BLACK  start as Black (or use WHITE)");
    Serial.println("FEN piece-letter case is preserved. Invalid positions leave the current game unchanged.");
  } else if (!command.empty()) {
    if (command.size() == 4 || command.size() == 5)
      processSingleUciMove(command);
    else
      Serial.printf("[SERIAL] Unknown command: %s\n", command.c_str());
  }
  serialCommandLength = 0;
}

static void pollSerialCommands() {
  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value == '\r') continue;
    if (value == '\n') { executeSerialCommand(); continue; }
    if (serialCommandOverflow)
      continue; // Discard the rest of an overlong line up to its newline.
    if (serialCommandLength + 1 < sizeof(serialCommand))
      serialCommand[serialCommandLength++] = char(value);
    else {
      serialCommandLength = 0;
      serialCommandOverflow = true;
    }
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

static void initializeOpeningBook() {
  if (openingBook.begin()) {
    Serial.printf("[BOOK] Ready: %lu Polyglot entries, %lu data bytes mapped read-only from flash\n",
                  (unsigned long)openingBook.entryCount(),
                  (unsigned long)openingBook.dataBytes());
  } else {
    Serial.printf("[BOOK] Unavailable: %s\n", openingBook.error());
  }
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

  std::istringstream wdlOption("name UCI_ShowWDL value true");
  engine->get_options().setoption(wdlOption);
  Serial.println("[GUI] Stockfish WDL output enabled for advantage bar");

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
    SearchScoreKind scoreKind = SearchScoreKind::None;
    int scoreValue = 0;
    bool tablebaseWin = false;

    if (info.score.is<Stockfish::Score::InternalUnits>()) {
      scoreKind = SearchScoreKind::Centipawns;
      scoreValue = info.score.get<Stockfish::Score::InternalUnits>().value;
    } else if (info.score.is<Stockfish::Score::Mate>()) {
      scoreKind = SearchScoreKind::Mate;
      scoreValue = info.score.get<Stockfish::Score::Mate>().plies;
    } else if (info.score.is<Stockfish::Score::Tablebase>()) {
      const Stockfish::Score::Tablebase tablebase =
        info.score.get<Stockfish::Score::Tablebase>();
      scoreKind = SearchScoreKind::Tablebase;
      scoreValue = tablebase.plies;
      tablebaseWin = tablebase.win;
    }

    int wdlWin = 0, wdlDraw = 0, wdlLoss = 0;
    const bool wdlValid = parseWdl(info.wdl, wdlWin, wdlDraw, wdlLoss);

    portENTER_CRITICAL(&searchMux);
    searchMailbox.depth = info.depth;
    searchMailbox.nps = (uint32_t)info.nps;
    searchMailbox.nodes = (uint64_t)info.nodes;
    searchMailbox.scoreKind = scoreKind;
    searchMailbox.scoreValue = scoreValue;
    searchMailbox.tablebaseWin = tablebaseWin;
    searchMailbox.scoreValid = scoreKind != SearchScoreKind::None;
    searchMailbox.wdlWin = wdlWin;
    searchMailbox.wdlDraw = wdlDraw;
    searchMailbox.wdlLoss = wdlLoss;
    searchMailbox.wdlValid = wdlValid;
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
  // USB-CDC defaults to a 256-byte RX queue. Position commands can be longer;
  // size the queue before begin() so a burst is not lost while the GUI loop
  // is servicing touch or display work.
  const size_t serialRxBuffer = Serial.setRxBufferSize(SerialCommandCapacity);
  Serial.begin(115200);
  if (serialRxBuffer < SerialCommandCapacity)
    Serial.printf("[SERIAL] WARNING: USB-CDC RX buffer is %u bytes (requested %u)\n",
                  (unsigned)serialRxBuffer, (unsigned)SerialCommandCapacity);
  else
    Serial.printf("[SERIAL] USB-CDC RX buffer: %u bytes\n",
                  (unsigned)serialRxBuffer);
  delay(1200);
  Serial.println();
  Serial.println("Stockfish ESP32-S3 touchscreen GUI");
  Serial.println("[GUI] Build: ips-invert-panel-clear-v3");
  Serial.println("[GUI] ILI9341 config: IPS inversion=ON, color order=BGR");
  Serial.printf("[GUI] Palette RGB565: background=0x%04X panel=0x%04X\n",
                ColorBackground, ColorPanel);

  initializeDisplayAndTouch();
  renderLoading();
  initializeEngine();
  loadOpeningBookSettingFromNvs();
  initializeOpeningBook();

  loadTouchCalibration();
  Serial.printf("[GUI] Touch calibration: %s\n",
                touchCalibrated ? "loaded from NVS" : "not found");
  const bool hasSavedSkillSetting = loadSkillSettingFromNvs();
  const bool restoredGame = loadSavedGameFromNvs(hasSavedSkillSetting);
  Serial.printf("[GUI] Move history storage: %s (PSRAM preferred)\n",
                gameHistory.usesPsram() ? "PSRAM" : "internal RAM fallback");
  if (!touchCalibrated) beginTouchCalibration();
  else if (restoredGame) showCurrentGame();
  else renderMenu();
}

void loop() {
  pollSerialCommands();
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