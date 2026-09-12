#include "display_ui.h"

#include <TFT_eSPI.h>

#include "ST77922.h"
#include "ST77922_Touch.h"

namespace {

TFT_eSPI tft;
TFT_eSprite screen(&tft);
ST77922 display;
ST77922_TOUCH touch;

// ---------------------------------------------------------------------------
// Layout (rotation 0 -> 320x480, TFT_eSPI font 4 -> 26 px glyph cell)
// ---------------------------------------------------------------------------
constexpr int MARGIN = 10;
constexpr int TEXT_FONT = 4;
constexpr int TEXT_SIZE = 1;

constexpr int FONT_CELL_H = 26;
constexpr int LINE_SPACING = 2;
constexpr int STATUS_H = FONT_CELL_H * TEXT_SIZE + 2 * LINE_SPACING; // 30
constexpr int LABEL_H = FONT_CELL_H * TEXT_SIZE + LINE_SPACING;      // 28
constexpr int LINE_H = FONT_CELL_H * TEXT_SIZE + LINE_SPACING;       // 28

// RGB565 colours
constexpr uint16_t COLOR_BG = 0x0000;        // black
constexpr uint16_t COLOR_TEXT = 0xFFFF;      // white
constexpr uint16_t COLOR_LABEL = 0xfec0;     // yellow
constexpr uint16_t COLOR_STATUS_BG = 0xfb80; // dark orange
constexpr uint16_t COLOR_RULE = 0x4208;      // dark grey

String currentStatus;
String currentUser;
String currentAssistant;

void pushToPanel() {
  display.Fill_Colors(0, 0, display.Get_Width(), display.Get_Height(),
                      static_cast<uint16_t *>(screen.getPointer()));
}

int drawWrapped(const String &text, int x, int y, int maxWidth, int maxY) {
  const int len = text.length();
  int curY = y;
  int i = 0;
  String lastLine;
  int lastY = -1;
  bool truncated = false;

  while (i < len) {
    while (i < len && (text[i] == ' ' || text[i] == '\n'))
      i++; // skip leading whitespace
    if (i >= len)
      break;

    String line;
    while (i < len) {
      int j = i;
      while (j < len && text[j] != ' ' && text[j] != '\n')
        j++;
      const String word = text.substring(i, j);
      const String candidate = line.isEmpty() ? word : (line + " " + word);
      if (!line.isEmpty() && screen.textWidth(candidate) > maxWidth)
        break;
      line = candidate;
      i = j;
      if (i < len && text[i] == '\n') {
        i++;
        break;
      }
      while (i < len && text[i] == ' ')
        i++;
    }

    if (curY + LINE_H > maxY) {
      truncated = true;
      break;
    }
    screen.drawString(line, x, curY);
    lastLine = line;
    lastY = curY;
    curY += LINE_H;
  }

  if (truncated && lastY >= 0) {
    // Redraw the final line with an ellipsis so it is clear text was dropped.
    screen.fillRect(x, lastY, screen.width() - x, LINE_H, COLOR_BG);
    screen.drawString(lastLine + " ...", x, lastY);
  }
  return curY;
}

// Draws a labelled block of text and returns the y coordinate after it.
int drawBlock(const String &label, const String &text, int x, int y, int maxY) {
  screen.setTextDatum(TL_DATUM);
  screen.setTextColor(COLOR_LABEL);
  screen.drawString(label, x, y);
  y += LABEL_H;

  screen.setTextColor(COLOR_TEXT);
  return drawWrapped(text, x, y, screen.width() - 2 * x, maxY);
}

void render() {
  screen.fillSprite(COLOR_BG);
  screen.setTextFont(TEXT_FONT);
  screen.setTextSize(TEXT_SIZE);

  if (!currentStatus.isEmpty()) {
    screen.fillRect(0, 0, screen.width(), STATUS_H, COLOR_STATUS_BG);
    screen.setTextDatum(ML_DATUM);
    screen.setTextColor(COLOR_TEXT);
    screen.drawString(currentStatus, MARGIN, STATUS_H / 2);
  }

  const int top = STATUS_H + 8;
  const int bottom = screen.height() - MARGIN;
  const int userMaxY = top + (bottom - top) * 2 / 5;

  int y = drawBlock("You:", currentUser, MARGIN, top, userMaxY);
  y += 6;
  screen.drawFastHLine(MARGIN, y, screen.width() - 2 * MARGIN, COLOR_RULE);
  y += 8;
  screen.setTextDatum(TL_DATUM);
  drawBlock("Assistant:", currentAssistant, MARGIN, y, bottom);

  pushToPanel();
}

} // namespace

void initDisplayUi() {
  display.Init();
  display.Set_Rotation(0);

  screen.createSprite(display.Get_Width(), display.Get_Height());
  screen.setSwapBytes(true);
  screen.fillSprite(COLOR_BG);

  touch.init();
  touch.Set_Rotation(0);
}

bool readTouch() {
  return touch.Get_Touch();
}

void uiMessage(const String &message) {
  currentStatus = "";
  currentUser = "";
  currentAssistant = "";

  screen.fillSprite(COLOR_BG);
  screen.setTextColor(COLOR_TEXT);
  screen.setTextFont(TEXT_FONT);
  screen.setTextSize(TEXT_SIZE);
  screen.setTextDatum(MC_DATUM);
  screen.drawString(message, screen.width() / 2, screen.height() / 2);
  pushToPanel();
}

void uiStatus(const String &status) {
  currentStatus = status;
  render();
}

void uiConversation(const String &status, const String &user, const String &assistant) {
  currentStatus = status;
  currentUser = user;
  currentAssistant = assistant;
  render();
}
