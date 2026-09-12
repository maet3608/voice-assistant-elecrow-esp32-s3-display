#include "display_ui.h"

#include <TFT_eSPI.h>

#include "ST77922.h"
#include "ST77922_Touch.h"
#include "latin1_font.h"

namespace {

TFT_eSPI tft;
TFT_eSprite screen(&tft);
ST77922 display;
ST77922_TOUCH touch;

// Layout for rotation 0 (320x480). The Latin-1 GFX font replaces TFT_eSPI's
// built-in font 4, which only contains ASCII 0x20-0x7F and therefore silently
// drops accented characters such as u-umlaut or sharp-s. Its line advance is
// exposed as Latin1Font24_YADVANCE by the generated header.
constexpr int MARGIN = 10;
constexpr int TEXT_SIZE = 1;

constexpr int FONT_CELL_H = Latin1Font24_YADVANCE;
constexpr int LINE_SPACING = 2;
constexpr int STATUS_H = (FONT_CELL_H * TEXT_SIZE + 2 * LINE_SPACING) * 1.2;
constexpr int LINE_H = FONT_CELL_H * TEXT_SIZE + LINE_SPACING;
constexpr int TOP_GAP = 10;
constexpr int BLOCK_GAP = 14;

// RGB565 colours
constexpr uint16_t COLOR_BG = 0x0000;        // black
constexpr uint16_t COLOR_TEXT = 0xFFFF;      // white
constexpr uint16_t COLOR_LABEL = 0xfec0;     // yellow
constexpr uint16_t COLOR_STATUS_BG = 0xfb80; // dark orange

String currentStatus;
String currentUser;
String currentAssistant;

void pushToPanel() {
  display.Fill_Colors(0, 0, display.Get_Width(), display.Get_Height(),
                      static_cast<uint16_t *>(screen.getPointer()));
}

// Clears the sprite and restores the font setup shared by every view.
void resetCanvas() {
  screen.fillSprite(COLOR_BG);
  screen.setFreeFont(&Latin1Font24);
  screen.setTextSize(TEXT_SIZE);
  screen.setTextColor(COLOR_TEXT);
  screen.setTextDatum(TL_DATUM);
}

// Returns the advance the renderer adds for a character, read straight from the
// GFX font table.
//
// TFT_eSPI::textWidth() measures the *last* character of a string as
// (xOffset + width) instead of its xAdvance, and the blank space glyph has
// neither (it is { bitmapOffset 0, width 0, height 0, xAdvance 7, 0, 0 }).
// textWidth(" ") therefore reports 0 even though drawString() advances the
// cursor by xAdvance, so the measurement cannot be used for the word separator.
int glyphAdvance(char ch) {
  const GFXglyph *glyphs = (const GFXglyph *)pgm_read_dword(&Latin1Font24.glyph);
  const int index = static_cast<int>(ch) - pgm_read_word(&Latin1Font24.first);
  if (index < 0)
    return 0;
  return pgm_read_byte(&glyphs[index].xAdvance) * TEXT_SIZE;
}

// Draws word-wrapped text inside the (x, x + maxWidth) column, stopping before
// maxY. Returns the y coordinate of the next free line. A trailing "..." marks
// text that did not fit.
int drawWrapped(const String &text, int x, int y, int maxWidth, int maxY) {
  const int len = text.length();
  const int spaceWidth = glyphAdvance(' ');
  int i = 0;
  int curY = y;
  String lastLine;
  int lastY = -1;

  while (i < len && curY + LINE_H <= maxY) {
    while (i < len && (text[i] == ' ' || text[i] == '\n'))
      i++; // skip separators between words/lines
    if (i >= len)
      break;

    String line;
    int lineWidth = 0;
    while (i < len && text[i] != '\n') {
      int j = i;
      while (j < len && text[j] != ' ' && text[j] != '\n')
        j++;
      const String word = text.substring(i, j);
      const int wordWidth = screen.textWidth(word);
      const int gap = line.isEmpty() ? 0 : spaceWidth;
      if (!line.isEmpty() && lineWidth + gap + wordWidth > maxWidth)
        break; // wrap before the word that does not fit
      if (!line.isEmpty())
        line += ' '; // always keep the separator, whatever the measured advance
      line += word;
      lineWidth += gap + wordWidth;
      i = j;
      while (i < len && text[i] == ' ')
        i++;
    }
    if (i < len && text[i] == '\n')
      i++; // explicit line break in the source text

    screen.drawString(line, x, curY);
    lastLine = line;
    lastY = curY;
    curY += LINE_H;
  }

  if (i < len && lastY >= 0) {
    // Out of vertical room: redraw the final line with an ellipsis so it is
    // clear that text was dropped.
    screen.fillRect(x, lastY, maxWidth, LINE_H, COLOR_BG);
    screen.drawString(lastLine + " ...", x, lastY);
  }
  return curY;
}

// Draws a labelled block of text and returns the y coordinate after it.
int drawBlock(const String &label, const String &text, int x, int y, int maxY) {
  screen.setTextDatum(TL_DATUM);
  screen.setTextColor(COLOR_LABEL);
  screen.drawString(label, x, y);
  y += LINE_H;

  screen.setTextColor(COLOR_TEXT);
  return drawWrapped(text, x, y, screen.width() - 2 * x, maxY);
}

void render() {
  resetCanvas();

  if (!currentStatus.isEmpty()) {
    screen.fillRect(0, 0, screen.width(), STATUS_H, COLOR_STATUS_BG);
    screen.setTextDatum(ML_DATUM);
    screen.drawString(currentStatus, MARGIN, STATUS_H / 2);
  }

  const int top = STATUS_H + TOP_GAP;
  const int bottom = screen.height() - MARGIN;
  const int userMaxY = top + (bottom - top) * 2 / 5;

  int y = drawBlock("You:", currentUser, MARGIN, top, userMaxY);
  y += BLOCK_GAP;
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

  resetCanvas();
  screen.setTextDatum(MC_DATUM);
  screen.drawString(message, screen.width() / 2, screen.height() / 2);
  pushToPanel();
}

void uiConversation(const String &status, const String &user, const String &assistant) {
  currentStatus = status;
  currentUser = user;
  currentAssistant = assistant;
  render();
}
