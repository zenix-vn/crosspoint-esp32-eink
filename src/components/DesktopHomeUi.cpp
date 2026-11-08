#include "DesktopHomeUi.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "ReadingStatsStore.h"
#include "WeatherStore.h"
#include "components/WeatherIcon.h"
#include "components/UITheme.h"
#include "components/icons/blocks.h"
#include "components/icons/book.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/vocab.h"
#include "fontIds.h"

namespace DesktopHomeUi {
namespace {

constexpr int SIDE_PADDING = 20;
constexpr int GAP = 14;
constexpr int CARD_PADDING = 12;
constexpr int CARD_RADIUS = 12;
constexpr int FOCUS_LINE = 3;  // focused cards and tiles get a thick outline
constexpr int CLOCK_HEIGHT = 48;
constexpr int RECENTS_LABEL_HEIGHT = 30;
constexpr int RECENTS_HEIGHT = 152;
constexpr int RECOMMENDED_HEIGHT = 160;
constexpr int STATS_HEIGHT = 64;
constexpr int DOCK_HEIGHT = 68;
constexpr int BUTTON_HEIGHT = 40;
constexpr int ICON_SIZE = 32;
constexpr int PROGRESS_BAR_HEIGHT = 8;
constexpr int COVER_WIDTH = COVER_HEIGHT * 3 / 5;
constexpr int RECENT_COVER_WIDTH = (RECENT_COVER_HEIGHT - 8) * 3 / 5;
constexpr int CARD_HEIGHT = COVER_HEIGHT + 2 * CARD_PADDING;
constexpr float RING_FULL_TURN = 6.2831853f;
constexpr int MIN_VALID_YEAR = 2024;

int bottomOf(const Rect& r) { return r.y + r.height; }

const char* weekdayName(const int wday) {
  static constexpr StrId NAMES[] = {StrId::STR_WEEKDAY_SUN, StrId::STR_WEEKDAY_MON, StrId::STR_WEEKDAY_TUE,
                                    StrId::STR_WEEKDAY_WED, StrId::STR_WEEKDAY_THU, StrId::STR_WEEKDAY_FRI,
                                    StrId::STR_WEEKDAY_SAT};
  return I18N.get(NAMES[std::clamp(wday, 0, 6)]);
}

const char* weekdayShort(const int wday) {
  static constexpr StrId NAMES[] = {StrId::STR_WD_SHORT_SUN, StrId::STR_WD_SHORT_MON, StrId::STR_WD_SHORT_TUE,
                                    StrId::STR_WD_SHORT_WED, StrId::STR_WD_SHORT_THU, StrId::STR_WD_SHORT_FRI,
                                    StrId::STR_WD_SHORT_SAT};
  return I18N.get(NAMES[std::clamp(wday, 0, 6)]);
}

int weekdayOfDay(const int32_t day) { return static_cast<int>((day % 7 + 7 + 4) % 7); }

void drawTextCentered(const GfxRenderer& renderer, const Rect& rect, const int fontId, const int y, const char* text,
                      const EpdFontFamily::Style style = EpdFontFamily::REGULAR, const bool black = true) {
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, rect.x + (rect.width - width) / 2, y, text, black, style);
}

void drawTextRight(const GfxRenderer& renderer, const int right, const int y, const int fontId, const char* text,
                   const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  renderer.drawText(fontId, right - renderer.getTextWidth(fontId, text, style), y, text, true, style);
}

void drawCard(const GfxRenderer& renderer, const Rect& rect, const bool focused) {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, focused ? FOCUS_LINE : 1, CARD_RADIUS, true);
}

void drawBar(const GfxRenderer& renderer, const int x, const int y, const int width, const int percent) {
  renderer.drawRoundedRect(x, y, width, PROGRESS_BAR_HEIGHT, 1, PROGRESS_BAR_HEIGHT / 2, true);
  const int filled = width * std::clamp(percent, 0, 100) / 100;
  if (filled > 0) renderer.fillRoundedRect(x, y, filled, PROGRESS_BAR_HEIGHT, PROGRESS_BAR_HEIGHT / 2, Color::Black);
}

// Crisp geometric heart icon (filled or 2px outline) centered at (cx, cy).
void drawHeartShape(const GfxRenderer& renderer, const int cx, const int cy, const int size, const bool filled,
                    const bool black = true) {
  const float scale = static_cast<float>(size) / 12.0f;
  for (int dy = -size; dy <= size; ++dy) {
    for (int dx = -size; dx <= size; ++dx) {
      const float x = static_cast<float>(dx) / (scale * 8.5f);
      const float y = static_cast<float>(-dy + size / 6) / (scale * 8.5f);
      const float a = x * x + y * y - 1.0f;
      const float val = a * a * a - x * x * y * y * y;
      if (filled) {
        if (val <= 0.0f) renderer.drawPixel(cx + dx, cy + dy, black);
      } else {
        if (val <= 0.0f && val >= -0.45f) renderer.drawPixel(cx + dx, cy + dy, black);
      }
    }
  }
}

void drawRing(const GfxRenderer& renderer, const int cx, const int cy, const int radius, const int thickness,
              const int percent) {
  const int outer = radius * radius;
  const int inner = (radius - thickness) * (radius - thickness);
  const float limit = RING_FULL_TURN * static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int d2 = dx * dx + dy * dy;
      if (d2 > outer || d2 < inner) continue;
      float angle = std::atan2(static_cast<float>(dx), static_cast<float>(-dy));
      if (angle < 0) angle += RING_FULL_TURN;
      if ((percent > 0 && angle <= limit) || ((dx + dy) & 1) == 0) renderer.drawPixel(cx + dx, cy + dy, true);
    }
  }
}

struct BigText {
  int fontId = UI_12_FONT_ID;
  int32_t advanceFP = 0;
  int top = 0;

  BigText(const GfxRenderer& renderer, const char* text) {
    if (renderer.ensureFontLoaded(NOTOSANS_18_FONT_ID)) fontId = NOTOSANS_18_FONT_ID;
    for (const char* c = text; *c; ++c) {
      int32_t advance = 0;
      int glyphTop = 0;
      if (!renderer.getCodepointMetrics(fontId, static_cast<uint8_t>(*c), EpdFontFamily::BOLD, advance, glyphTop)) {
        continue;
      }
      advanceFP += advance;
      if (*c >= '0' && *c <= '9') top = std::max(top, glyphTop);
    }
  }
  bool valid() const { return advanceFP > 0 && top > 0; }
  int scaleFor(const int height, const int maxWidth) const {
    const int byHeight = height * 256 / top;
    const int byWidth = static_cast<int>(static_cast<int64_t>(maxWidth) * 256 * 16 / advanceFP);
    return std::max(256, std::min(byHeight, byWidth));
  }
  int width(const int scale256) const { return static_cast<int>(static_cast<int64_t>(advanceFP) * scale256 / 4096); }
  void draw(const GfxRenderer& renderer, const char* text, int x, const int baseline, const int scale256) const {
    for (const char* c = text; *c; ++c) {
      x += renderer.drawScaledCodepoint(fontId, static_cast<uint8_t>(*c), EpdFontFamily::BOLD, x, baseline, scale256);
    }
  }
};

void drawSun(const GfxRenderer& renderer, const int cx, const int cy) {
  constexpr int CORE = 7;
  for (int dy = -CORE; dy <= CORE; ++dy) {
    for (int dx = -CORE; dx <= CORE; ++dx) {
      const int d2 = dx * dx + dy * dy;
      if (d2 <= CORE * CORE && d2 >= (CORE - 2) * (CORE - 2)) renderer.drawPixel(cx + dx, cy + dy, true);
    }
  }
  static constexpr int RAYS[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
  for (const auto& ray : RAYS) {
    const bool diagonal = ray[0] != 0 && ray[1] != 0;
    const int inner = diagonal ? 8 : 11;
    const int outer = diagonal ? 11 : 15;
    renderer.drawLine(cx + ray[0] * inner, cy + ray[1] * inner, cx + ray[0] * outer, cy + ray[1] * outer, 2, true);
  }
}

void drawDots(const GfxRenderer& renderer, const int cx, const int cy) {
  constexpr int DOT = 6;
  constexpr int STEP = 12;
  for (int i = -1; i <= 1; ++i) {
    renderer.fillRoundedRect(cx + i * STEP - DOT / 2, cy - DOT / 2, DOT, DOT, DOT / 2, Color::Black);
  }
}

void drawCover(const GfxRenderer& renderer, const Rect& slot, const RecentBook& book, const int height) {
  bool drawn = false;
  if (!book.coverBmpPath.empty()) {
    HalFile file;
    if (Storage.openFileForRead("DESK", UITheme::getCoverThumbPath(book.coverBmpPath, height), file)) {
      Bitmap bitmap(file);
      drawn = bitmap.parseHeaders() == BmpReaderError::Ok && BaseTheme::drawCoverThumbFill(renderer, bitmap, slot);
    }
  }
  if (drawn) {
    renderer.drawRect(slot.x, slot.y, slot.width, slot.height, true);
  } else {
    BaseTheme::drawCoverPlaceholder(renderer, slot);
    const int textTop = slot.y + slot.height / 3 + 4;
    const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int maxLines = std::max(1, (slot.y + slot.height - 4 - textTop) / lineHeight);
    int y = textTop;
    for (const auto& line : renderer.wrappedText(SMALL_FONT_ID, book.title.c_str(), slot.width - 8, maxLines)) {
      renderer.drawText(SMALL_FONT_ID, slot.x + 4, y, line.c_str(), false);
      y += lineHeight;
    }
  }
  // Draw favorite badge in top-right corner of the cover if marked as favorite.
  if (RECENT_BOOKS.isFavorite(book.path) && slot.width >= 40) {
    const Rect badge{slot.x + slot.width - 20, slot.y + 3, 17, 17};
    renderer.fillRoundedRect(badge.x, badge.y, badge.width, badge.height, 4, Color::White);
    renderer.drawRoundedRect(badge.x, badge.y, badge.width, badge.height, 1, 4, true);
    drawHeartShape(renderer, badge.x + badge.width / 2, badge.y + badge.height / 2, 6, true, true);
  }
}

constexpr int SHEET_TITLE_HEIGHT = 48;
constexpr int SHEET_ROW_HEIGHT = 56;
constexpr int SHEET_RADIUS = 14;
constexpr int SHEET_SHADOW = 4;
constexpr int SHEET_WIDTH = 300;
constexpr int SHEET_POINTER = 12;

void drawStatsGlyph(const GfxRenderer& renderer, const int x, const int y) {
  renderer.fillRect(x + 4, y + 18, 6, 10, true);
  renderer.fillRect(x + 13, y + 10, 6, 18, true);
  renderer.fillRect(x + 22, y + 4, 6, 24, true);
  renderer.drawLine(x + 2, y + 29, x + 29, y + 29, 2, true);
}

void drawRemoveGlyph(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int R = 13;
  const int cx = x + 16;
  const int cy = y + 16;
  for (int dy = -R; dy <= R; ++dy) {
    for (int dx = -R; dx <= R; ++dx) {
      const int d2 = dx * dx + dy * dy;
      if (d2 <= R * R && d2 >= (R - 2) * (R - 2)) renderer.drawPixel(cx + dx, cy + dy, true);
    }
  }
  renderer.drawLine(cx - 5, cy - 5, cx + 5, cy + 5, 2, true);
  renderer.drawLine(cx - 5, cy + 5, cx + 5, cy - 5, 2, true);
}

void drawSheetGlyph(const GfxRenderer& renderer, const SheetGlyph glyph, const int x, const int y) {
  switch (glyph) {
    case SheetGlyph::Folder:
      renderer.drawIcon(FolderIcon, x, y, ICON_SIZE);
      break;
    case SheetGlyph::Transfer:
      renderer.drawIcon(TransferIcon, x, y, ICON_SIZE);
      break;
    case SheetGlyph::Plugins:
      renderer.drawIcon(BlocksIcon, x, y, ICON_SIZE);
      break;
    case SheetGlyph::Details:
      renderer.drawIcon(BookIcon, x, y, ICON_SIZE);
      break;
    case SheetGlyph::Stats:
      drawStatsGlyph(renderer, x, y);
      break;
    case SheetGlyph::Remove:
      drawRemoveGlyph(renderer, x, y);
      break;
    case SheetGlyph::Heart:
      drawHeartShape(renderer, x + ICON_SIZE / 2, y + ICON_SIZE / 2, 11, true, true);
      break;
    case SheetGlyph::Light:
      drawSun(renderer, x + ICON_SIZE / 2, y + ICON_SIZE / 2);
      break;
    case SheetGlyph::Settings:
      renderer.drawIcon(Settings2Icon, x, y, ICON_SIZE);
      break;
  }
}

bool readProgressBytes(const std::string& cachePath, uint8_t* data, const int size) {
  HalFile file;
  if (!Storage.openFileForRead("DESK", cachePath + "/progress.bin", file)) return false;
  return file.read(data, size) >= size;
}

void formatDuration(char* out, const size_t size, const uint32_t seconds) {
  const uint32_t minutes = seconds / 60;
  if (minutes < 60) {
    snprintf(out, size, "%u %s", static_cast<unsigned>(minutes), tr(STR_DESKTOP_MIN));
  } else {
    snprintf(out, size, "%uh%02u", static_cast<unsigned>(minutes / 60), static_cast<unsigned>(minutes % 60));
  }
}

}  // namespace

Layout layout(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  const int top = std::max(36, metrics.homeTopPadding - 8);
  const int bottom = content.y + content.height - metrics.buttonHintsHeight - 6;
  const int left = content.x + SIDE_PADDING;
  const int width = content.width - 2 * SIDE_PADDING;

  Layout out;
  out.header = Rect{0, metrics.topPadding, renderer.getScreenWidth(), top - metrics.topPadding};
  if (content.width > content.height) {
    // Landscape: clock & hero on the left; recents, recommended, dock on the right.
    const int column = (width - GAP) / 2;
    const int rightX = left + column + GAP;
    out.clock = Rect{left, top, column, CLOCK_HEIGHT};
    const int heroY = bottomOf(out.clock) + GAP;
    out.hero = Rect{left, heroY, column, std::max(160, bottom - heroY)};
    out.recents = Rect{rightX, top, column, RECENTS_HEIGHT};
    out.dock = Rect{rightX, bottom - DOCK_HEIGHT, column, DOCK_HEIGHT};
    const int recY = bottomOf(out.recents) + GAP;
    out.recommended = Rect{rightX, recY, column, std::max(100, out.dock.y - GAP - recY)};
  } else {
    out.clock = Rect{left, top, width, CLOCK_HEIGHT};
    const int heroY = bottomOf(out.clock) + GAP;
    const int fixedBelowHero = GAP + RECENTS_HEIGHT + GAP + RECOMMENDED_HEIGHT + GAP + DOCK_HEIGHT;
    const int heroH = std::clamp(bottom - fixedBelowHero - heroY, 196, CARD_HEIGHT);
    out.hero = Rect{left, heroY, width, heroH};
    out.recents = Rect{left, bottomOf(out.hero) + GAP, width, RECENTS_HEIGHT};
    out.recommended = Rect{left, bottomOf(out.recents) + GAP, width, RECOMMENDED_HEIGHT};
    out.dock = Rect{left, bottom - DOCK_HEIGHT, width, DOCK_HEIGHT};
  }
  return out;
}

Rect recentTabRect(const Layout& layout, const bool favoritesTab) {
  const Rect& r = layout.recents;
  if (!favoritesTab) {
    return Rect{r.x, r.y, 104, RECENTS_LABEL_HEIGHT};
  }
  return Rect{r.x + 108, r.y, 128, RECENTS_LABEL_HEIGHT};
}

Rect seeAllRect(const Layout& layout) {
  const Rect& r = layout.recents;
  return Rect{r.x + r.width - 120, r.y, 120, RECENTS_LABEL_HEIGHT};
}

Rect recommendedSeeMoreRect(const Layout& layout) {
  const Rect& r = layout.recommended;
  return Rect{r.x + r.width - 120, r.y, 120, RECENTS_LABEL_HEIGHT};
}

Rect recentSlot(const Layout& layout, const int index) {
  const Rect& r = layout.recents;
  const int step = r.width / MAX_RECENTS;
  const int coverH = std::min(RECENT_COVER_HEIGHT, r.height - RECENTS_LABEL_HEIGHT);
  const int coverW = coverH * 3 / 5;
  return Rect{r.x + index * step + (step - coverW) / 2, r.y + RECENTS_LABEL_HEIGHT, coverW, coverH};
}

Rect recommendedSlot(const Layout& layout, const int index) {
  const Rect& r = layout.recommended;
  const int cardW = (r.width - GAP) / MAX_RECOMMENDED;
  const int cardH = r.height - RECENTS_LABEL_HEIGHT;
  return Rect{r.x + index * (cardW + GAP), r.y + RECENTS_LABEL_HEIGHT, cardW, cardH};
}

Rect dockSlot(const Layout& layout, const int index) {
  const Rect& r = layout.dock;
  const int step = r.width / DOCK_COUNT;
  return Rect{r.x + index * step + 2, r.y, step - 4, r.height};
}

Rect sheetRect(const Layout& layout, const int count, const Rect& anchor) {
  const int height = SHEET_TITLE_HEIGHT + count * SHEET_ROW_HEIGHT + CARD_PADDING;
  const int screenWidth = layout.header.width;
  const int width = std::min(SHEET_WIDTH, screenWidth - 2 * SIDE_PADDING);
  const int x = std::clamp(anchor.x + anchor.width / 2 - width / 2, SIDE_PADDING, screenWidth - SIDE_PADDING - width);
  const int above = anchor.y - SHEET_POINTER - height;
  const int y = above >= layout.clock.y ? above : anchor.y + anchor.height + SHEET_POINTER;
  return Rect{x, y, width, height};
}

Rect sheetRow(const Rect& sheet, const int index) {
  return Rect{sheet.x + 8, sheet.y + SHEET_TITLE_HEIGHT + index * SHEET_ROW_HEIGHT, sheet.width - 16, SHEET_ROW_HEIGHT};
}

void drawSheet(const GfxRenderer& renderer, const Rect& sheet, const Rect& anchor, const char* title,
               const SheetItem* items, const int count, const int selected) {
  renderer.fillRoundedRect(sheet.x + SHEET_SHADOW, sheet.y + SHEET_SHADOW, sheet.width, sheet.height, SHEET_RADIUS,
                           Color::Black);
  renderer.fillRoundedRect(sheet.x, sheet.y, sheet.width, sheet.height, SHEET_RADIUS, Color::White);
  renderer.drawRoundedRect(sheet.x, sheet.y, sheet.width, sheet.height, 2, SHEET_RADIUS, true);

  const bool pointsDown = sheet.y + sheet.height <= anchor.y;
  const int tipX = std::clamp(anchor.x + anchor.width / 2, sheet.x + SHEET_RADIUS + SHEET_POINTER,
                              sheet.x + sheet.width - SHEET_RADIUS - SHEET_POINTER);
  const int edge = pointsDown ? sheet.y + sheet.height - 1 : sheet.y;
  const int dir = pointsDown ? 1 : -1;
  const int outerX[] = {tipX - SHEET_POINTER, tipX + SHEET_POINTER, tipX};
  const int outerY[] = {edge, edge, edge + dir * SHEET_POINTER};
  renderer.fillPolygon(outerX, outerY, 3, true);
  const int innerX[] = {tipX - SHEET_POINTER + 3, tipX + SHEET_POINTER - 3, tipX};
  const int innerY[] = {edge - dir * 2, edge - dir * 2, edge + dir * (SHEET_POINTER - 3)};
  renderer.fillPolygon(innerX, innerY, 3, false);

  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const std::string heading =
      renderer.truncatedText(UI_12_FONT_ID, title, sheet.width - 2 * (CARD_PADDING + 8), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, sheet.x + CARD_PADDING + 8, sheet.y + (SHEET_TITLE_HEIGHT - titleLine) / 2 + 2,
                    heading.c_str(), true, EpdFontFamily::BOLD);

  for (int i = 0; i < count; ++i) {
    const Rect row = sheetRow(sheet, i);
    if (i > 0) renderer.drawLine(row.x + 56, row.y, row.x + row.width - 12, row.y, true);
    if (i == selected) renderer.drawRoundedRect(row.x, row.y + 4, row.width, row.height - 8, FOCUS_LINE, 10, true);
    drawSheetGlyph(renderer, items[i].glyph, row.x + 12, row.y + (row.height - ICON_SIZE) / 2);
    renderer.drawText(UI_12_FONT_ID, row.x + 12 + ICON_SIZE + 14, row.y + (row.height - titleLine) / 2,
                      items[i].label, true, i == selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int cx = row.x + row.width - 20;
    const int cy = row.y + row.height / 2;
    renderer.drawLine(cx - 4, cy - 6, cx + 2, cy, 2, true);
    renderer.drawLine(cx + 2, cy, cx - 4, cy + 6, 2, true);
  }
}

void drawClock(const GfxRenderer& renderer, const Rect rect) {
  struct tm now {};
  const bool hasTime = halClock.localTime(now) && now.tm_year + 1900 >= MIN_VALID_YEAR;

  char timeText[8] = "--:--";
  const char* suffix = nullptr;
  if (hasTime) {
    int hour = now.tm_hour;
    if (SETTINGS.clockFormat == 1) {
      suffix = hour < 12 ? "AM" : "PM";
      hour %= 12;
      if (hour == 0) hour = 12;
      snprintf(timeText, sizeof(timeText), "%d:%02d", hour, now.tm_min);
    } else {
      snprintf(timeText, sizeof(timeText), "%02d:%02d", hour, now.tm_min);
    }
  }

  const int baseline = rect.y + rect.height - 6;
  const BigText big(renderer, timeText);
  int x = rect.x;
  if (big.valid()) {
    const int scale = big.scaleFor(rect.height - 10, rect.width / 2);
    big.draw(renderer, timeText, x, baseline, scale);
    x += big.width(scale) + 6;
  }
  const int ascender = renderer.getFontAscenderSize(UI_12_FONT_ID);
  if (suffix) renderer.drawText(UI_12_FONT_ID, x, baseline - ascender, suffix, true, EpdFontFamily::BOLD);

  const int right = rect.x + rect.width;
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int dateY = baseline - ascender;
  if (!hasTime) {
    drawTextRight(renderer, right, dateY, SMALL_FONT_ID, tr(STR_DESKTOP_CLOCK_NOT_SET));
    return;
  }
  drawTextRight(renderer, right, dateY - smallLine - 2, SMALL_FONT_ID, weekdayName(now.tm_wday));
  char dateText[16];
  snprintf(dateText, sizeof(dateText), "%02d/%02d/%04d", now.tm_mday, now.tm_mon + 1, now.tm_year + 1900);
  drawTextRight(renderer, right, dateY, UI_12_FONT_ID, dateText, EpdFontFamily::BOLD);

  // Weather between the time and the date, when there is room.
  const weather::Forecast* forecast = cachedForecast();
  if (!forecast->valid()) return;
  const int dateLeft = right - std::max(renderer.getTextWidth(UI_12_FONT_ID, dateText, EpdFontFamily::BOLD),
                                        renderer.getTextWidth(SMALL_FONT_ID, weekdayName(now.tm_wday)));
  char temp[8];
  snprintf(temp, sizeof(temp), "%d\xC2\xB0", forecast->tempC);
  const int icon = std::min(36, rect.height - 8);
  const int tempW = renderer.getTextWidth(UI_12_FONT_ID, temp, EpdFontFamily::BOLD);
  const int blockW = icon + 4 + tempW;
  const int gapLeft = x + (suffix ? renderer.getTextWidth(UI_12_FONT_ID, suffix, EpdFontFamily::BOLD) : 0) + 12;
  const int gapRight = dateLeft - 12;
  if (gapRight - gapLeft < blockW) return;
  const int bx = gapLeft + (gapRight - gapLeft - blockW) / 2;
  drawWeatherIcon(renderer, bx, baseline - icon, icon, weather::kindOf(forecast->code));
  renderer.drawText(UI_12_FONT_ID, bx + icon + 4, dateY, temp, true, EpdFontFamily::BOLD);
}

void drawHero(const GfxRenderer& renderer, const Rect rect, const RecentBook* book, const BookInfo& info,
              const bool focused) {
  drawCard(renderer, rect, focused);
  const bool compact = rect.width < 340;
  const int pad = compact ? 12 : CARD_PADDING;
  const int maxCoverH = compact ? 160 : COVER_HEIGHT;
  const int coverHeight = std::min(maxCoverH, rect.height - 2 * pad);
  const int coverWidth = coverHeight * 3 / 5;
  const Rect cover{rect.x + pad, rect.y + (rect.height - coverHeight) / 2, coverWidth, coverHeight};
  const int textX = cover.x + cover.width + (compact ? 10 : 14);
  const int textWidth = rect.x + rect.width - pad - textX;

  const int btnH = compact ? 36 : BUTTON_HEIGHT;
  const int btnFont = compact ? UI_10_FONT_ID : UI_12_FONT_ID;
  const Rect button{textX, bottomOf(cover) - btnH, textWidth, btnH};
  renderer.fillRoundedRect(button.x, button.y, button.width, button.height, btnH / 2, Color::Black);
  const char* buttonLabel = book ? tr(STR_DESKTOP_CONTINUE) : tr(STR_LIBRARY);
  const int labelWidth = renderer.getTextWidth(btnFont, buttonLabel, EpdFontFamily::BOLD);
  constexpr int ARROW = 7;
  const int labelX = button.x + (button.width - labelWidth - ARROW - 8) / 2;
  const int labelY = button.y + (button.height - renderer.getLineHeight(btnFont)) / 2;
  renderer.drawText(btnFont, labelX, labelY, buttonLabel, false, EpdFontFamily::BOLD);
  const int arrowX = labelX + labelWidth + 8;
  const int arrowY = button.y + button.height / 2;
  const int xs[] = {arrowX, arrowX + ARROW, arrowX};
  const int ys[] = {arrowY - ARROW, arrowY, arrowY + ARROW};
  renderer.fillPolygon(xs, ys, 3, false);

  const int titleFont = compact ? UI_10_FONT_ID : UI_12_FONT_ID;
  const int subFont = compact ? SMALL_FONT_ID : UI_10_FONT_ID;

  if (!book) {
    BaseTheme::drawCoverPlaceholder(renderer, cover);
    int y = cover.y + 6;
    for (const auto& line :
         renderer.wrappedText(titleFont, tr(STR_NO_OPEN_BOOK), textWidth, 2, EpdFontFamily::BOLD)) {
      renderer.drawText(titleFont, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(titleFont);
    }
    y += 4;
    for (const auto& line : renderer.wrappedText(subFont, tr(STR_START_READING), textWidth, 3)) {
      renderer.drawText(subFont, textX, y, line.c_str());
      y += renderer.getLineHeight(subFont);
    }
    return;
  }

  drawCover(renderer, cover, *book, COVER_HEIGHT);

  const int rowHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int progressY = button.y - 6 - rowHeight;
  if (info.percent >= 0) {
    char percentText[8];
    snprintf(percentText, sizeof(percentText), "%d%%", info.percent);
    const int percentWidth = renderer.getTextWidth(SMALL_FONT_ID, percentText, EpdFontFamily::BOLD);
    drawBar(renderer, textX, progressY + (rowHeight - PROGRESS_BAR_HEIGHT) / 2, textWidth - percentWidth - 8,
            info.percent);
    drawTextRight(renderer, textX + textWidth, progressY, SMALL_FONT_ID, percentText, EpdFontFamily::BOLD);
  }

  const int titleLine = renderer.getLineHeight(titleFont);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  int y = cover.y + 1;
  renderer.drawText(SMALL_FONT_ID, textX, y, tr(STR_DESKTOP_NOW_READING), true, EpdFontFamily::BOLD);
  if (!compact && info.secondsLeft >= 0) {
    char left[40];
    const int minutesLeft = std::max(1, static_cast<int>(info.secondsLeft / 60));
    if (minutesLeft >= 60) {
      snprintf(left, sizeof(left), tr(STR_DESKTOP_LEFT_HM), minutesLeft / 60, minutesLeft % 60);
    } else {
      snprintf(left, sizeof(left), tr(STR_DESKTOP_LEFT_M), minutesLeft);
    }
    drawTextRight(renderer, textX + textWidth, y, SMALL_FONT_ID, left);
  }
  y += smallLine + 3;
  const int authorLine = book->author.empty() ? 0 : renderer.getLineHeight(subFont) + 2;
  const int chapterLine = (!compact && !info.chapter.empty()) ? smallLine + 4 : 0;
  const int titleLines = std::clamp((progressY - 2 - y - authorLine - chapterLine) / titleLine, 1, 3);
  for (const auto& line :
       renderer.wrappedText(titleFont, book->title.c_str(), textWidth, titleLines, EpdFontFamily::BOLD)) {
    renderer.drawText(titleFont, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLine;
  }
  if (authorLine > 0 && y + authorLine <= progressY) {
    renderer.drawText(subFont, textX, y + 1,
                      renderer.truncatedText(subFont, book->author.c_str(), textWidth).c_str());
    y += authorLine;
  }
  if (chapterLine > 0 && y + chapterLine <= progressY) {
    int chapterWidth = textWidth;
    if (info.chapterSecondsLeft >= 0) {
      char left[40];
      const int minutesLeft = std::max(1, static_cast<int>(info.chapterSecondsLeft / 60));
      if (minutesLeft >= 60) {
        snprintf(left, sizeof(left), tr(STR_DESKTOP_LEFT_HM), minutesLeft / 60, minutesLeft % 60);
      } else {
        snprintf(left, sizeof(left), tr(STR_DESKTOP_LEFT_M), minutesLeft);
      }
      drawTextRight(renderer, textX + textWidth, y + 4, SMALL_FONT_ID, left, EpdFontFamily::BOLD);
      chapterWidth -= renderer.getTextWidth(SMALL_FONT_ID, left, EpdFontFamily::BOLD) + 10;
    }
    renderer.drawText(SMALL_FONT_ID, textX, y + 4,
                      renderer.truncatedText(SMALL_FONT_ID, info.chapter.c_str(), chapterWidth).c_str());
  }
}

void drawRecents(const GfxRenderer& renderer, const Layout& layout, const RecentBook* books, const int count,
                 const int focused, const bool favoritesTab, const bool seeAllFocused) {
  const Rect& r = layout.recents;
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);

  // Header row: [GẦN ĐÂY]  [♥ YÊU THÍCH] ............... [Xem tất cả >]
  const char* recentLabel = tr(STR_DESKTOP_RECENT);
  const auto recentStyle = !favoritesTab ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  renderer.drawText(SMALL_FONT_ID, r.x, r.y + 1, recentLabel, true, recentStyle);
  const int recentW = renderer.getTextWidth(SMALL_FONT_ID, recentLabel, recentStyle);
  if (!favoritesTab) {
    renderer.fillRect(r.x, r.y + smallLine + 2, recentW, 2, true);
  }

  const int favX = r.x + std::max(96, recentW + 18);
  drawHeartShape(renderer, favX + 6, r.y + smallLine / 2 + 1, 5, favoritesTab, true);
  const char* favLabel = tr(STR_DESKTOP_FAVORITES);
  const auto favStyle = favoritesTab ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  renderer.drawText(SMALL_FONT_ID, favX + 16, r.y + 1, favLabel, true, favStyle);
  const int favW = renderer.getTextWidth(SMALL_FONT_ID, favLabel, favStyle);
  if (favoritesTab) {
    renderer.fillRect(favX, r.y + smallLine + 2, 16 + favW, 2, true);
  }

  const char* seeAll = tr(STR_DESKTOP_SEE_ALL);
  const int seeAllW = renderer.getTextWidth(SMALL_FONT_ID, seeAll, EpdFontFamily::BOLD);
  drawTextRight(renderer, r.x + r.width, r.y + 1, SMALL_FONT_ID, seeAll, EpdFontFamily::BOLD);
  if (seeAllFocused) {
    renderer.drawRoundedRect(r.x + r.width - seeAllW - 8, r.y - 2, seeAllW + 12, smallLine + 6, 2, 6, true);
  }

  if (count <= 0) {
    const Rect first = recentSlot(layout, 0);
    const Rect emptyBox{r.x, first.y, r.width, first.height};
    renderer.drawRoundedRect(emptyBox.x, emptyBox.y, emptyBox.width, emptyBox.height, 1, CARD_RADIUS, true);
    drawTextCentered(renderer, emptyBox, UI_10_FONT_ID,
                     emptyBox.y + (emptyBox.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                     favoritesTab ? tr(STR_DESKTOP_FAVORITES_EMPTY) : tr(STR_DESKTOP_RECENT_EMPTY));
    return;
  }
  for (int i = 0; i < std::min(count, MAX_RECENTS); ++i) {
    const Rect slot = recentSlot(layout, i);
    drawCover(renderer, slot, books[i], RECENT_COVER_HEIGHT);
    // Reading progress along the bottom of the cover, on a white strip so it
    // reads over any artwork.
    const int percent = readBookInfo(books[i]).percent;
    if (percent >= 0) {
      const int barX = slot.x + 5;
      const int barW = slot.width - 10;
      const int barY = slot.y + slot.height - PROGRESS_BAR_HEIGHT - 5;
      renderer.fillRect(barX - 2, barY - 2, barW + 4, PROGRESS_BAR_HEIGHT + 4, false);
      drawBar(renderer, barX, barY, barW, percent);
    }
    if (i == focused) {
      renderer.drawRoundedRect(slot.x - 4, slot.y - 4, slot.width + 8, slot.height + 8, FOCUS_LINE, 4, true);
    }
  }
}

void drawRecommended(const GfxRenderer& renderer, const Layout& layout, const RecommendedBook* books, const int count,
                     const int focused, const bool seeMoreFocused) {
  const Rect& r = layout.recommended;
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);

  // Header row: [NÊN ĐỌC] ............................ [Xem thêm >]
  renderer.drawText(SMALL_FONT_ID, r.x, r.y + 1, tr(STR_DESKTOP_RECOMMENDED), true, EpdFontFamily::BOLD);
  const char* seeMore = tr(STR_DESKTOP_SEE_MORE);
  const int seeMoreW = renderer.getTextWidth(SMALL_FONT_ID, seeMore, EpdFontFamily::BOLD);
  drawTextRight(renderer, r.x + r.width, r.y + 1, SMALL_FONT_ID, seeMore, EpdFontFamily::BOLD);
  if (seeMoreFocused) {
    renderer.drawRoundedRect(r.x + r.width - seeMoreW - 8, r.y - 2, seeMoreW + 12, smallLine + 6, 2, 6, true);
  }

  if (count <= 0) {
    const Rect first = recommendedSlot(layout, 0);
    const Rect emptyBox{r.x, first.y, r.width, first.height};
    renderer.drawRoundedRect(emptyBox.x, emptyBox.y, emptyBox.width, emptyBox.height, 1, CARD_RADIUS, true);
    drawTextCentered(renderer, emptyBox, UI_10_FONT_ID,
                     emptyBox.y + (emptyBox.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                     tr(STR_DESKTOP_RECOMMENDED_EMPTY));
    return;
  }

  const int titleLine = renderer.getLineHeight(UI_10_FONT_ID);
  for (int i = 0; i < std::min(count, MAX_RECOMMENDED); ++i) {
    const Rect card = recommendedSlot(layout, i);
    drawCard(renderer, card, i == focused);

    const int coverH = card.height - 20;
    const int coverW = coverH * 3 / 5;
    const Rect cover{card.x + 10, card.y + 10, coverW, coverH};
    drawCover(renderer, cover, books[i].book, RECENT_COVER_HEIGHT);

    const int textX = cover.x + cover.width + 10;
    const int textW = card.x + card.width - 10 - textX;
    int y = cover.y;

    if (!books[i].reason.empty()) {
      const std::string tag =
          renderer.truncatedText(SMALL_FONT_ID, books[i].reason.c_str(), textW - 8, EpdFontFamily::BOLD);
      const int tagW = std::min(textW, renderer.getTextWidth(SMALL_FONT_ID, tag.c_str(), EpdFontFamily::BOLD) + 10);
      renderer.fillRoundedRect(textX, y, tagW, smallLine + 4, 6, Color::Black);
      renderer.drawText(SMALL_FONT_ID, textX + 5, y + 2, tag.c_str(), false, EpdFontFamily::BOLD);
      y += smallLine + 8;
    }

    for (const auto& line :
         renderer.wrappedText(UI_10_FONT_ID, books[i].book.title.c_str(), textW, 3, EpdFontFamily::BOLD)) {
      if (y + titleLine > card.y + card.height - 6) break;
      renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += titleLine;
    }

    if (!books[i].book.author.empty() && y + smallLine <= card.y + card.height - 4) {
      renderer.drawText(SMALL_FONT_ID, textX, y + 2,
                        renderer.truncatedText(SMALL_FONT_ID, books[i].book.author.c_str(), textW).c_str());
    }
  }
}

void drawStatsCard(const GfxRenderer& renderer, const Rect rect, const bool focused, const Quote*) {
  const auto& stats = READING_STATS;
  const int32_t today = ReadingStatsStore::today();
  const int goal = std::max<int>(1, SETTINGS.dailyReadingGoalMinutes);
  const int minutes = static_cast<int>(stats.secondsOn(today) / 60);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);

  if (rect.height > rect.width) {
    // 1/3 right column beside Hero on Home: two vertically stacked circular ring cards.
    const int halfH = (rect.height - 10) / 2;
    const Rect topCard{rect.x, rect.y, rect.width, halfH};
    const Rect botCard{rect.x, rect.y + rect.height - halfH, rect.width, halfH};
    drawCard(renderer, topCard, focused);
    drawCard(renderer, botCard, focused);

    constexpr int RING = 19;
    const int cx = rect.x + rect.width / 2;
    const int maxTextW = rect.width - 12;
    char text[48];

    // Top ring: Minutes read today
    const int topRingY = topCard.y + 27;
    drawRing(renderer, cx, topRingY, RING, 5, minutes * 100 / goal);
    snprintf(text, sizeof(text), "%d", minutes);
    drawTextCentered(renderer, topCard, SMALL_FONT_ID, topRingY - smallLine / 2, text, EpdFontFamily::BOLD);
    snprintf(text, sizeof(text), "%d/%d %s", minutes, goal, tr(STR_DESKTOP_MIN));
    drawTextCentered(renderer, topCard, UI_10_FONT_ID, topCard.y + 50,
                     renderer.truncatedText(UI_10_FONT_ID, text, maxTextW, EpdFontFamily::BOLD).c_str(),
                     EpdFontFamily::BOLD);
    drawTextCentered(renderer, topCard, SMALL_FONT_ID, topCard.y + 72,
                     renderer.truncatedText(SMALL_FONT_ID, tr(STR_DESKTOP_TODAY_LOWER), maxTextW).c_str());

    // Bottom ring: Reading streak
    const int curStreak = stats.currentStreak(today);
    const int bestStreak = stats.bestStreak();
    const int streakPct = bestStreak > 0 ? std::clamp(curStreak * 100 / bestStreak, curStreak > 0 ? 15 : 0, 100) : 0;
    const int botRingY = botCard.y + 27;
    drawRing(renderer, cx, botRingY, RING, 5, streakPct);
    snprintf(text, sizeof(text), "%d", curStreak);
    drawTextCentered(renderer, botCard, SMALL_FONT_ID, botRingY - smallLine / 2, text, EpdFontFamily::BOLD);
    snprintf(text, sizeof(text), tr(STR_DESKTOP_STREAK_FMT), curStreak);
    drawTextCentered(renderer, botCard, UI_10_FONT_ID, botCard.y + 50,
                     renderer.truncatedText(UI_10_FONT_ID, text, maxTextW, EpdFontFamily::BOLD).c_str(),
                     EpdFontFamily::BOLD);
    snprintf(text, sizeof(text), tr(STR_DESKTOP_BEST_FMT), bestStreak);
    drawTextCentered(renderer, botCard, SMALL_FONT_ID, botCard.y + 72,
                     renderer.truncatedText(SMALL_FONT_ID, text, maxTextW).c_str());
    return;
  }

  drawCard(renderer, rect, focused);
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int textTop = rect.y + (rect.height - titleLine - smallLine) / 2;
  const int half = rect.width / 2;

  constexpr int RING = 20;
  const int ringX = rect.x + CARD_PADDING + RING;
  drawRing(renderer, ringX, rect.y + rect.height / 2, RING, 5, minutes * 100 / goal);
  const int todayX = ringX + RING + 10;
  char text[48];
  snprintf(text, sizeof(text), "%d/%d %s", minutes, goal, tr(STR_DESKTOP_MIN));
  renderer.drawText(UI_12_FONT_ID, todayX, textTop, text, true, EpdFontFamily::BOLD);
  snprintf(text, sizeof(text), "%s · %u %s", tr(STR_DESKTOP_TODAY_LOWER), static_cast<unsigned>(stats.pagesOn(today)),
           tr(STR_DESKTOP_PAGES));
  renderer.drawText(SMALL_FONT_ID, todayX, textTop + titleLine,
                    renderer.truncatedText(SMALL_FONT_ID, text, rect.x + half - 6 - todayX).c_str());
  renderer.drawLine(rect.x + half, rect.y + 10, rect.x + half, rect.y + rect.height - 11, true);

  const int streakX = rect.x + half + CARD_PADDING;
  const int streakWidth = rect.x + rect.width - CARD_PADDING - streakX;
  snprintf(text, sizeof(text), tr(STR_DESKTOP_STREAK_FMT), stats.currentStreak(today));
  renderer.drawText(UI_12_FONT_ID, streakX, textTop,
                    renderer.truncatedText(UI_12_FONT_ID, text, streakWidth, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
  snprintf(text, sizeof(text), tr(STR_DESKTOP_BEST_FMT), stats.bestStreak());
  renderer.drawText(SMALL_FONT_ID, streakX, textTop + titleLine,
                    renderer.truncatedText(SMALL_FONT_ID, text, streakWidth).c_str());
}

void drawDock(const GfxRenderer& renderer, const Layout& layout, const DockItem* items, const int focused) {
  const Rect& r = layout.dock;
  renderer.drawLine(r.x, r.y - GAP / 2, r.x + r.width - 1, r.y - GAP / 2, true);
  const int labelLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int iconY = r.y + (r.height - ICON_SIZE - 4 - labelLine) / 2;
  for (int i = 0; i < DOCK_COUNT; ++i) {
    const Rect tile = dockSlot(layout, i);
    const bool isFocused = i == focused;
    if (isFocused) renderer.drawRoundedRect(tile.x, tile.y, tile.width, tile.height, 2, CARD_RADIUS, true);
    const int iconX = tile.x + (tile.width - ICON_SIZE) / 2;
    switch (items[i].glyph) {
      case DockGlyph::Library:
        renderer.drawIcon(LibraryIcon, iconX, iconY, ICON_SIZE);
        break;
      case DockGlyph::Stats:
        drawStatsGlyph(renderer, iconX, iconY);
        break;
      case DockGlyph::Settings:
        renderer.drawIcon(Settings2Icon, iconX, iconY, ICON_SIZE);
        break;
      case DockGlyph::Vocab:
        renderer.drawIcon(VocabIcon, iconX, iconY, ICON_SIZE);
        break;
      case DockGlyph::More:
        drawDots(renderer, tile.x + tile.width / 2, iconY + ICON_SIZE / 2);
        break;
    }
    const auto style = isFocused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, items[i].label, tile.width - 4, style);
    drawTextCentered(renderer, tile, SMALL_FONT_ID, iconY + ICON_SIZE + 4, label.c_str(), style);
  }
}

Rect bookListTabRect(const Rect area, const bool favoritesTab) {
  const int left = area.x + SIDE_PADDING;
  const int width = area.width - 2 * SIDE_PADDING;
  const int tabW = (width - GAP) / 2;
  return Rect{left + (favoritesTab ? tabW + GAP : 0), area.y + 8, tabW, 40};
}

Rect bookListRowRect(const Rect area, const int rowIndex) {
  const int left = area.x + SIDE_PADDING;
  const int width = area.width - 2 * SIDE_PADDING;
  const int listTop = area.y + 58;
  const int listBottom = area.y + area.height - 46;
  const int rowStride = (listBottom - listTop) / BOOK_LIST_PAGE_SIZE;
  const int rowH = rowStride - 8;
  return Rect{left, listTop + rowIndex * rowStride, width, rowH};
}

Rect bookListHeartRect(const Rect& rowRect) {
  return Rect{rowRect.x + rowRect.width - 48, rowRect.y + (rowRect.height - 44) / 2, 44, 44};
}

Rect bookListPrevPageRect(const Rect area) {
  const int cx = area.x + area.width / 2;
  const int y = area.y + area.height - 40;
  return Rect{cx - 110, y, 40, 34};
}

Rect bookListNextPageRect(const Rect area) {
  const int cx = area.x + area.width / 2;
  const int y = area.y + area.height - 40;
  return Rect{cx + 70, y, 40, 34};
}

// "<  2 / 3  >" under a book list; nothing when everything fits on one page.
static void drawBookListPager(const GfxRenderer& renderer, const Rect area, const int page, const int totalPages) {
  if (totalPages <= 1) return;
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const Rect prevBtn = bookListPrevPageRect(area);
  const Rect nextBtn = bookListNextPageRect(area);
  renderer.drawRoundedRect(prevBtn.x, prevBtn.y, prevBtn.width, prevBtn.height, 1, 8, true);
  renderer.drawRoundedRect(nextBtn.x, nextBtn.y, nextBtn.width, nextBtn.height, 1, 8, true);
  drawTextCentered(renderer, prevBtn, UI_12_FONT_ID, prevBtn.y + (prevBtn.height - titleLine) / 2, "<",
                   EpdFontFamily::BOLD);
  drawTextCentered(renderer, nextBtn, UI_12_FONT_ID, nextBtn.y + (nextBtn.height - titleLine) / 2, ">",
                   EpdFontFamily::BOLD);
  char pageText[32];
  snprintf(pageText, sizeof(pageText), "%d / %d", page + 1, totalPages);
  drawTextCentered(renderer, Rect{prevBtn.x + prevBtn.width, prevBtn.y, nextBtn.x - (prevBtn.x + prevBtn.width), prevBtn.height},
                   UI_10_FONT_ID, prevBtn.y + (prevBtn.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, pageText,
                   EpdFontFamily::BOLD);
}

void drawBookListPage(const GfxRenderer& renderer, const Rect area, const RecentBook* books, const int totalCount,
                      const int page, const int selectedInPage, const bool favoritesTab, const int recentTotal,
                      const int favTotal) {
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);

  // Top segmented tabs: [Gần đây (N)]  [♥ Yêu thích (M)]
  for (int t = 0; t < 2; ++t) {
    const bool isFavTab = t == 1;
    const bool active = favoritesTab == isFavTab;
    const Rect tab = bookListTabRect(area, isFavTab);
    if (active) {
      renderer.fillRoundedRect(tab.x, tab.y, tab.width, tab.height, 10, Color::Black);
    } else {
      renderer.drawRoundedRect(tab.x, tab.y, tab.width, tab.height, 1, 10, true);
    }
    char label[64];
    snprintf(label, sizeof(label), "%s (%d)", isFavTab ? tr(STR_FAVORITE_BOOKS_TITLE) : tr(STR_RECENT_BOOKS_TITLE),
             isFavTab ? favTotal : recentTotal);
    drawTextCentered(renderer, tab, UI_10_FONT_ID, tab.y + (tab.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                     label, active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, !active);
  }

  const int startIdx = page * BOOK_LIST_PAGE_SIZE;
  const int pageCount = std::clamp(totalCount - startIdx, 0, BOOK_LIST_PAGE_SIZE);

  if (pageCount <= 0) {
    const Rect first = bookListRowRect(area, 0);
    const Rect emptyBox{first.x, first.y, first.width, 160};
    renderer.drawRoundedRect(emptyBox.x, emptyBox.y, emptyBox.width, emptyBox.height, 1, CARD_RADIUS, true);
    drawTextCentered(renderer, emptyBox, UI_10_FONT_ID, emptyBox.y + (emptyBox.height - titleLine) / 2,
                     favoritesTab ? tr(STR_DESKTOP_FAVORITES_EMPTY) : tr(STR_DESKTOP_RECENT_EMPTY));
    return;
  }

  for (int i = 0; i < pageCount; ++i) {
    const RecentBook& book = books[startIdx + i];
    const Rect row = bookListRowRect(area, i);
    drawCard(renderer, row, i == selectedInPage);

    const int coverH = row.height - 12;
    const int coverW = coverH * 3 / 5;
    const Rect cover{row.x + 8, row.y + 6, coverW, coverH};
    drawCover(renderer, cover, book, RECENT_COVER_HEIGHT);

    const Rect heartRect = bookListHeartRect(row);
    const bool isFav = RECENT_BOOKS.isFavorite(book.path);
    drawHeartShape(renderer, heartRect.x + heartRect.width / 2, heartRect.y + heartRect.height / 2, 10, isFav, true);

    const int textX = cover.x + cover.width + 12;
    const int textW = heartRect.x - 8 - textX;
    int y = row.y + 8;

    for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textW, 2, EpdFontFamily::BOLD)) {
      renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += titleLine;
    }
    // Reading progress takes the last line of the row; the author fits above it.
    const int percent = readBookInfo(book).percent;
    const int lineY = row.y + row.height - 8 - smallLine;
    const int textBottom = percent >= 0 ? lineY - 2 : row.y + row.height - 4;
    if (!book.author.empty() && y + 2 + smallLine <= textBottom) {
      renderer.drawText(SMALL_FONT_ID, textX, y + 2,
                        renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), textW).c_str());
    }
    if (percent >= 0) {
      char percentText[8];
      snprintf(percentText, sizeof(percentText), "%d%%", percent);
      const int percentW = renderer.getTextWidth(SMALL_FONT_ID, percentText, EpdFontFamily::BOLD);
      drawTextRight(renderer, textX + textW, lineY, SMALL_FONT_ID, percentText, EpdFontFamily::BOLD);
      drawBar(renderer, textX, lineY + (smallLine - PROGRESS_BAR_HEIGHT) / 2, textW - percentW - 8, percent);
    }
  }

  drawBookListPager(renderer, area, page, (totalCount + BOOK_LIST_PAGE_SIZE - 1) / BOOK_LIST_PAGE_SIZE);
}

void drawRecommendedPage(const GfxRenderer& renderer, const Rect area, const RecommendedBook* books,
                         const int totalCount, const int page, const int selectedInPage, const char* caption,
                         const char* emptyText) {
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);

  // Caption in the band the book list uses for its tabs.
  const Rect band = bookListTabRect(area, false);
  const int left = area.x + SIDE_PADDING;
  const int bandW = area.width - 2 * SIDE_PADDING;
  renderer.drawText(SMALL_FONT_ID, left, band.y + (band.height - smallLine) / 2,
                    renderer.truncatedText(SMALL_FONT_ID, caption, bandW).c_str());

  const int startIdx = page * BOOK_LIST_PAGE_SIZE;
  const int pageCount = std::clamp(totalCount - startIdx, 0, BOOK_LIST_PAGE_SIZE);

  if (pageCount <= 0) {
    const Rect first = bookListRowRect(area, 0);
    const Rect emptyBox{first.x, first.y, first.width, 160};
    renderer.drawRoundedRect(emptyBox.x, emptyBox.y, emptyBox.width, emptyBox.height, 1, CARD_RADIUS, true);
    drawTextCentered(renderer, emptyBox, UI_10_FONT_ID, emptyBox.y + (emptyBox.height - titleLine) / 2, emptyText);
    return;
  }

  for (int i = 0; i < pageCount; ++i) {
    const RecommendedBook& rec = books[startIdx + i];
    const Rect row = bookListRowRect(area, i);
    drawCard(renderer, row, i == selectedInPage);

    const int coverH = row.height - 12;
    const int coverW = coverH * 3 / 5;
    const Rect cover{row.x + 8, row.y + 6, coverW, coverH};
    drawCover(renderer, cover, rec.book, RECENT_COVER_HEIGHT);

    const int textX = cover.x + cover.width + 12;
    const int textW = row.x + row.width - 12 - textX;
    int y = row.y + 8;

    if (!rec.reason.empty()) {
      const std::string tag = renderer.truncatedText(SMALL_FONT_ID, rec.reason.c_str(), textW - 10, EpdFontFamily::BOLD);
      const int tagW = std::min(textW, renderer.getTextWidth(SMALL_FONT_ID, tag.c_str(), EpdFontFamily::BOLD) + 10);
      renderer.fillRoundedRect(textX, y, tagW, smallLine + 4, 6, Color::Black);
      renderer.drawText(SMALL_FONT_ID, textX + 5, y + 2, tag.c_str(), false, EpdFontFamily::BOLD);
      y += smallLine + 8;
    }

    for (const auto& line :
         renderer.wrappedText(UI_12_FONT_ID, rec.book.title.c_str(), textW, 2, EpdFontFamily::BOLD)) {
      if (y + titleLine > row.y + row.height - 4) break;
      renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += titleLine;
    }
    if (!rec.book.author.empty() && y + smallLine <= row.y + row.height - 4) {
      renderer.drawText(SMALL_FONT_ID, textX, y + 2,
                        renderer.truncatedText(SMALL_FONT_ID, rec.book.author.c_str(), textW).c_str());
    }
  }

  drawBookListPager(renderer, area, page, (totalCount + BOOK_LIST_PAGE_SIZE - 1) / BOOK_LIST_PAGE_SIZE);
}

void drawStatsPage(const GfxRenderer& renderer, const Rect area) {
  const auto& stats = READING_STATS;
  const int32_t today = ReadingStatsStore::today();
  const int goal = std::max<int>(1, SETTINGS.dailyReadingGoalMinutes);
  const int minutes = static_cast<int>(stats.secondsOn(today) / 60);
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const bool landscape = area.width > area.height;
  const int left = area.x + SIDE_PADDING;
  const int width = area.width - 2 * SIDE_PADDING;
  const int column = landscape ? (width - GAP) / 2 : width;
  char text[48];

  const int RING = landscape ? 36 : 64;
  const int ringX = left + RING;
  const int ringY = area.y + 16 + RING;
  drawRing(renderer, ringX, ringY, RING, landscape ? 8 : 12, minutes * 100 / goal);
  snprintf(text, sizeof(text), "%d%%", std::min(999, minutes * 100 / goal));
  drawTextCentered(renderer, Rect{ringX - RING, 0, 2 * RING, 0}, UI_12_FONT_ID, ringY - titleLine / 2, text,
                   EpdFontFamily::BOLD);
  const int infoX = ringX + RING + 20;
  const int infoWidth = left + column - infoX;
  int infoY = ringY - (titleLine + 2 * smallLine + 8) / 2;
  renderer.drawText(SMALL_FONT_ID, infoX, infoY, tr(STR_DESKTOP_TODAY), true, EpdFontFamily::BOLD);
  infoY += smallLine + 2;
  snprintf(text, sizeof(text), "%d / %d %s", minutes, goal, tr(STR_DESKTOP_MIN));
  renderer.drawText(UI_12_FONT_ID, infoX, infoY, text, true, EpdFontFamily::BOLD);
  infoY += titleLine + 4;
  snprintf(text, sizeof(text), "%u %s", static_cast<unsigned>(stats.pagesOn(today)), tr(STR_DESKTOP_PAGES));
  renderer.drawText(SMALL_FONT_ID, infoX, infoY, renderer.truncatedText(SMALL_FONT_ID, text, infoWidth).c_str());
  int y = ringY + RING + 20;

  uint32_t weekSeconds = 0;
  for (int i = 0; i < ReadingStatsStore::HISTORY_DAYS && today >= 0; ++i) weekSeconds += stats.secondsOn(today - i);
  const int32_t month = ReadingStatsStore::thisMonth();
  const int year = month >= 0 ? month / 12 : -1;
  char values[6][40];
  char duration[24];
  snprintf(values[0], sizeof(values[0]), "%d", stats.currentStreak(today));
  snprintf(values[1], sizeof(values[1]), "%d", stats.bestStreak());
  formatDuration(values[2], sizeof(values[2]), weekSeconds);
  formatDuration(values[3], sizeof(values[3]), weekSeconds / ReadingStatsStore::HISTORY_DAYS);
  formatDuration(duration, sizeof(duration), stats.secondsInMonth(month));
  snprintf(values[4], sizeof(values[4]), tr(STR_DESKTOP_BOOKS_FMT), duration, stats.finishedInMonth(month));
  formatDuration(duration, sizeof(duration), year >= 0 ? stats.secondsInYear(year) : 0);
  snprintf(values[5], sizeof(values[5]), tr(STR_DESKTOP_BOOKS_FMT), duration,
           year >= 0 ? stats.finishedInYear(year) : 0);
  const char* captions[6] = {tr(STR_DESKTOP_CURRENT_STREAK), tr(STR_DESKTOP_BEST_STREAK), tr(STR_DESKTOP_WEEK_TOTAL),
                             tr(STR_DESKTOP_DAILY_AVG),      tr(STR_DESKTOP_THIS_MONTH),  tr(STR_DESKTOP_THIS_YEAR)};
  const int tileWidth = (column - GAP) / 2;
  const int tilePadding = landscape ? 6 : CARD_PADDING;
  const int tileGap = landscape ? 8 : GAP;
  const int tileHeight = titleLine + smallLine + 2 * tilePadding;
  for (int i = 0; i < 6; ++i) {
    const Rect tile{left + (i % 2) * (tileWidth + GAP), y + (i / 2) * (tileHeight + tileGap), tileWidth, tileHeight};
    drawCard(renderer, tile, false);
    renderer.drawText(UI_12_FONT_ID, tile.x + tilePadding, tile.y + tilePadding,
                      renderer.truncatedText(UI_12_FONT_ID, values[i], tile.width - 2 * tilePadding,
                                             EpdFontFamily::BOLD)
                          .c_str(),
                      true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, tile.x + tilePadding, tile.y + tilePadding + titleLine,
                      renderer.truncatedText(SMALL_FONT_ID, captions[i], tile.width - 2 * tilePadding).c_str());
  }
  y += 3 * (tileHeight + tileGap) + 8;

  const Rect chart = landscape ? Rect{left + column + GAP, area.y + 16, column, area.height - 32}
                               : Rect{left, y, width, area.y + area.height - 16 - y};
  renderer.drawText(SMALL_FONT_ID, chart.x, chart.y, tr(STR_DESKTOP_LAST_7_DAYS), true, EpdFontFamily::BOLD);
  snprintf(text, sizeof(text), tr(STR_DESKTOP_GOAL_FMT), goal);
  drawTextRight(renderer, chart.x + chart.width, chart.y, SMALL_FONT_ID, text);
  const int chartTop = chart.y + 2 * smallLine + 8;
  const int chartBottom = chart.y + chart.height - smallLine - 4;
  const int chartHeight = chartBottom - chartTop;
  if (chartHeight < 20) return;
  const int slot = chart.width / ReadingStatsStore::HISTORY_DAYS;
  int maxMinutes = goal;
  for (int i = 0; i < ReadingStatsStore::HISTORY_DAYS && today >= 0; ++i) {
    maxMinutes = std::max(maxMinutes, static_cast<int>(stats.secondsOn(today - i) / 60));
  }
  for (int i = 0; i < ReadingStatsStore::HISTORY_DAYS; ++i) {
    const bool isToday = i == ReadingStatsStore::HISTORY_DAYS - 1;
    const int32_t day = today < 0 ? -1 : today - (ReadingStatsStore::HISTORY_DAYS - 1) + i;
    const int dayMinutes = static_cast<int>(stats.secondsOn(day) / 60);
    int barHeight = chartHeight * dayMinutes / maxMinutes;
    if (dayMinutes > 0) barHeight = std::max(barHeight, 3);
    const Rect cell{chart.x + i * slot, 0, slot, 0};
    const int barWidth = slot * 3 / 5;
    const int barX = cell.x + (slot - barWidth) / 2;
    if (barHeight > 0) {
      if (isToday) {
        renderer.fillRect(barX, chartBottom - barHeight, barWidth, barHeight, true);
      } else {
        renderer.fillRectDither(barX, chartBottom - barHeight, barWidth, barHeight, Color::DarkGray);
        renderer.drawRect(barX, chartBottom - barHeight, barWidth, barHeight, true);
      }
      snprintf(text, sizeof(text), "%d", dayMinutes);
      drawTextCentered(renderer, cell, SMALL_FONT_ID, chartBottom - barHeight - smallLine - 2, text);
    }
    if (day >= 0) {
      drawTextCentered(renderer, cell, SMALL_FONT_ID, chartBottom + 4, weekdayShort(weekdayOfDay(day)),
                       isToday ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }
  }
  renderer.drawLine(chart.x, chartBottom, chart.x + chart.width - 1, chartBottom, true);
  const int goalY = chartBottom - chartHeight * goal / maxMinutes;
  for (int x = chart.x; x < chart.x + chart.width; x += 8) {
    renderer.drawLine(x, goalY, std::min(x + 3, chart.x + chart.width - 1), goalY, true);
  }
}

void drawVocabStatsPage(const GfxRenderer& renderer, const Rect area, const VocabStats& stats) {
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const bool landscape = area.width > area.height;
  const int left = area.x + SIDE_PADDING;
  const int width = area.width - 2 * SIDE_PADDING;
  const int column = landscape ? (width - GAP) / 2 : width;
  char text[64];

  // Rating ring with the level, the active days and one piece of advice.
  const int RING = landscape ? 36 : 52;
  const int ringX = left + RING;
  const int ringY = area.y + 12 + RING;
  drawRing(renderer, ringX, ringY, RING, landscape ? 8 : 10, std::max(0, stats.score));
  snprintf(text, sizeof(text), "%d", stats.score);
  if (stats.score < 0) snprintf(text, sizeof(text), "-");
  drawTextCentered(renderer, Rect{ringX - RING, 0, 2 * RING, 0}, UI_12_FONT_ID, ringY - titleLine / 2, text,
                   EpdFontFamily::BOLD);
  const char* level = tr(STR_VOCAB_LEVEL_NONE);
  if (stats.score >= 85) {
    level = tr(STR_VOCAB_LEVEL_EXCELLENT);
  } else if (stats.score >= 70) {
    level = tr(STR_VOCAB_LEVEL_GOOD);
  } else if (stats.score >= 50) {
    level = tr(STR_VOCAB_LEVEL_FAIR);
  } else if (stats.score >= 0) {
    level = tr(STR_VOCAB_LEVEL_WEAK);
  }
  const int infoX = ringX + RING + 20;
  const int infoWidth = left + column - infoX;
  int infoY = ringY - (titleLine + 2 * smallLine + 8) / 2;
  renderer.drawText(SMALL_FONT_ID, infoX, infoY, tr(STR_VOCAB_ASSESSMENT), true, EpdFontFamily::BOLD);
  infoY += smallLine + 2;
  renderer.drawText(UI_12_FONT_ID, infoX, infoY,
                    renderer.truncatedText(UI_12_FONT_ID, level, infoWidth, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
  infoY += titleLine + 4;
  snprintf(text, sizeof(text), tr(STR_VOCAB_ACTIVE_DAYS_FMT), stats.activeDays, VocabStats::DAYS);
  renderer.drawText(SMALL_FONT_ID, infoX, infoY, renderer.truncatedText(SMALL_FONT_ID, text, infoWidth).c_str());
  int y = ringY + RING + 12;
  for (const auto& line : renderer.wrappedText(SMALL_FONT_ID, stats.tip, column, 2)) {
    renderer.drawText(SMALL_FONT_ID, left, y, line.c_str());
    y += smallLine;
  }
  y += 10;

  // Tiles: streaks, this week's work, recall and quiz accuracy.
  char values[6][16];
  snprintf(values[0], sizeof(values[0]), "%d", stats.streak);
  snprintf(values[1], sizeof(values[1]), "%d", stats.best);
  snprintf(values[2], sizeof(values[2]), "%d", stats.weekCards);
  snprintf(values[3], sizeof(values[3]), "%d", stats.weekNew);
  const auto percent = [](char* out, const size_t size, const int value) {
    if (value >= 0) {
      snprintf(out, size, "%d%%", value);
    } else {
      snprintf(out, size, "-");
    }
  };
  percent(values[4], sizeof(values[4]), stats.recall);
  percent(values[5], sizeof(values[5]), stats.quiz);
  const char* captions[6] = {tr(STR_DESKTOP_CURRENT_STREAK), tr(STR_DESKTOP_BEST_STREAK), tr(STR_VOCAB_WEEK_CARDS),
                             tr(STR_VOCAB_WEEK_NEW),         tr(STR_VOCAB_RECALL),        tr(STR_VOCAB_QUIZ_RATE)};
  const int tileGap = landscape ? 8 : 10;
  const int tileWidth = (column - 2 * tileGap) / 3;
  const int tilePadding = landscape ? 6 : 10;
  const int tileHeight = titleLine + smallLine + 2 * tilePadding;
  for (int i = 0; i < 6; ++i) {
    const Rect tile{left + (i % 3) * (tileWidth + tileGap), y + (i / 3) * (tileHeight + tileGap), tileWidth,
                    tileHeight};
    drawCard(renderer, tile, false);
    renderer.drawText(UI_12_FONT_ID, tile.x + tilePadding, tile.y + tilePadding, values[i], true,
                      EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, tile.x + tilePadding, tile.y + tilePadding + titleLine,
                      renderer.truncatedText(SMALL_FONT_ID, captions[i], tile.width - 2 * tilePadding).c_str());
  }
  y += 2 * (tileHeight + tileGap) + 6;

  // Word mastery: one bar split by how long each word now stays remembered.
  const int total = stats.fresh + stats.learning + stats.familiar + stats.mastered;
  const int known = total - stats.fresh;
  renderer.drawText(SMALL_FONT_ID, left, y, tr(STR_VOCAB_MASTERY), true, EpdFontFamily::BOLD);
  snprintf(text, sizeof(text), "%d / %d", known, total);
  drawTextRight(renderer, left + column, y, SMALL_FONT_ID, text, EpdFontFamily::BOLD);
  y += smallLine + 6;
  constexpr int BAR = 16;
  const Color shades[3] = {Color::Black, Color::DarkGray, Color::LightGray};
  const int parts[3] = {stats.mastered, stats.familiar, stats.learning};
  int barX = left;
  for (int i = 0; i < 3 && total > 0; ++i) {
    const int w = column * parts[i] / total;
    if (w <= 0) continue;
    if (shades[i] == Color::Black) {
      renderer.fillRect(barX, y, w, BAR, true);
    } else {
      renderer.fillRectDither(barX, y, w, BAR, shades[i]);
    }
    barX += w;
  }
  renderer.drawRect(left, y, column, BAR, true);
  y += BAR + 8;
  const StrId legendIds[4] = {StrId::STR_VOCAB_MASTERED_FMT, StrId::STR_VOCAB_FAMILIAR_FMT,
                              StrId::STR_VOCAB_LEARNING_FMT, StrId::STR_VOCAB_NOT_STARTED_FMT};
  const int legendValues[4] = {stats.mastered, stats.familiar, stats.learning, stats.fresh};
  const int half = column / 2;
  constexpr int SWATCH = 10;
  for (int i = 0; i < 4; ++i) {
    const int x = left + (i % 2) * half;
    const int ly = y + (i / 2) * smallLine;
    const int sy = ly + (smallLine - SWATCH) / 2;
    if (i == 0) {
      renderer.fillRect(x, sy, SWATCH, SWATCH, true);
    } else if (i < 3) {
      renderer.fillRectDither(x, sy, SWATCH, SWATCH, shades[i]);
    }
    renderer.drawRect(x, sy, SWATCH, SWATCH, true);
    snprintf(text, sizeof(text), I18N.get(legendIds[i]), legendValues[i]);
    renderer.drawText(SMALL_FONT_ID, x + SWATCH + 6, ly,
                      renderer.truncatedText(SMALL_FONT_ID, text, half - SWATCH - 10).c_str());
  }
  y += 2 * smallLine + 4;
  snprintf(text, sizeof(text), tr(STR_VOCAB_DUE_FMT), stats.dueToday, stats.dueTomorrow, stats.dueWeek);
  renderer.drawText(SMALL_FONT_ID, left, y, renderer.truncatedText(SMALL_FONT_ID, text, column).c_str());
  y += smallLine + 14;

  // Cards per day: new words solid, reviews shaded.
  const Rect chart = landscape ? Rect{left + column + GAP, area.y + 12, column, area.height - 24}
                               : Rect{left, y, width, area.y + area.height - 12 - y};
  renderer.drawText(SMALL_FONT_ID, chart.x, chart.y, tr(STR_DESKTOP_LAST_7_DAYS), true, EpdFontFamily::BOLD);
  drawTextRight(renderer, chart.x + chart.width, chart.y, SMALL_FONT_ID, tr(STR_VOCAB_CARDS_PER_DAY));
  const int chartTop = chart.y + 2 * smallLine + 4;
  const int chartBottom = chart.y + chart.height - smallLine - 4;
  const int chartHeight = chartBottom - chartTop;
  if (chartHeight < 20) return;
  const int slot = chart.width / VocabStats::DAYS;
  int maxCards = 10;
  for (const int cards : stats.dayCards) maxCards = std::max(maxCards, cards);
  for (int i = 0; i < VocabStats::DAYS; ++i) {
    const bool isToday = i == VocabStats::DAYS - 1;
    const int32_t day = stats.today < 0 ? -1 : stats.today - (VocabStats::DAYS - 1) + i;
    const int cards = stats.dayCards[i];
    int barHeight = chartHeight * cards / maxCards;
    if (cards > 0) barHeight = std::max(barHeight, 3);
    const int newHeight = cards > 0 ? barHeight * stats.dayNew[i] / cards : 0;
    const Rect cell{chart.x + i * slot, 0, slot, 0};
    const int barWidth = slot * 3 / 5;
    const int barX = cell.x + (slot - barWidth) / 2;
    if (barHeight > 0) {
      renderer.fillRectDither(barX, chartBottom - barHeight, barWidth, barHeight - newHeight, Color::LightGray);
      if (newHeight > 0) renderer.fillRect(barX, chartBottom - newHeight, barWidth, newHeight, true);
      renderer.drawRect(barX, chartBottom - barHeight, barWidth, barHeight, true);
      snprintf(text, sizeof(text), "%d", cards);
      drawTextCentered(renderer, cell, SMALL_FONT_ID, chartBottom - barHeight - smallLine - 2, text,
                       isToday ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }
    if (day >= 0) {
      drawTextCentered(renderer, cell, SMALL_FONT_ID, chartBottom + 4, weekdayShort(weekdayOfDay(day)),
                       isToday ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }
  }
  renderer.drawLine(chart.x, chartBottom, chart.x + chart.width - 1, chartBottom, true);
}

void drawQuote(const GfxRenderer& renderer, const Rect rect, const Quote& quote, const int maxLines) {
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int lines = std::min(maxLines, rect.height / lineHeight);
  if (lines <= 0 || quote.text.empty()) return;
  const std::string body = "\xE2\x80\x9C" + quote.text + "\xE2\x80\x9D \xE2\x80\x94 " + quote.source;
  int y = rect.y + (rect.height - lines * lineHeight) / 2;
  for (const auto& line : renderer.wrappedText(SMALL_FONT_ID, body.c_str(), rect.width, lines)) {
    renderer.drawText(SMALL_FONT_ID, rect.x, y, line.c_str());
    y += lineHeight;
  }
}

bool drawSleepCard(const GfxRenderer& renderer, const RecentBook& book, const BookInfo& info, const Quote* quote) {
  constexpr int SLEEP_COVER_HEIGHT = 400;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int left = SIDE_PADDING;
  const int width = pageWidth - 2 * SIDE_PADDING;
  const Rect page{0, 0, pageWidth, pageHeight};
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);

  int y = 20;
  renderer.drawText(SMALL_FONT_ID, left, y, SETTINGS.deviceName[0] ? SETTINGS.deviceName : tr(STR_DEVICE_NAME_DEFAULT),
                    true, EpdFontFamily::BOLD);
  struct tm now {};
  if (halClock.localTime(now) && now.tm_year + 1900 >= MIN_VALID_YEAR) {
    char date[40];
    snprintf(date, sizeof(date), "%s, %02d/%02d", weekdayName(now.tm_wday), now.tm_mday, now.tm_mon + 1);
    drawTextRight(renderer, left + width, y, SMALL_FONT_ID, date);
  }
  y += smallLine + 16;

  const Rect cover{(pageWidth - SLEEP_COVER_HEIGHT * 3 / 5) / 2, y, SLEEP_COVER_HEIGHT * 3 / 5, SLEEP_COVER_HEIGHT};
  drawCover(renderer, cover, book, SLEEP_COVER_HEIGHT);
  y += cover.height + 16;

  for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), width, 2, EpdFontFamily::BOLD)) {
    drawTextCentered(renderer, page, UI_12_FONT_ID, y, line.c_str(), EpdFontFamily::BOLD);
    y += titleLine;
  }
  if (!book.author.empty()) {
    drawTextCentered(renderer, page, UI_10_FONT_ID, y + 2,
                     renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), width).c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID) + 2;
  }
  y += 10;
  if (info.percent >= 0) {
    constexpr int BAR_WIDTH = 280;
    char percentText[8];
    snprintf(percentText, sizeof(percentText), "%d%%", info.percent);
    const int percentWidth = renderer.getTextWidth(UI_10_FONT_ID, percentText, EpdFontFamily::BOLD);
    const int barX = (pageWidth - BAR_WIDTH - 10 - percentWidth) / 2;
    const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID);
    drawBar(renderer, barX, y + (rowHeight - PROGRESS_BAR_HEIGHT) / 2, BAR_WIDTH, info.percent);
    renderer.drawText(UI_10_FONT_ID, barX + BAR_WIDTH + 10, y, percentText, true, EpdFontFamily::BOLD);
    y += rowHeight + 16;
  }

  drawStatsCard(renderer, Rect{left, y, width, STATS_HEIGHT}, false, nullptr);
  y += STATS_HEIGHT + 12;

  const int footerY = pageHeight - 16 - smallLine;
  if (quote) drawQuote(renderer, Rect{left, y, width, footerY - 12 - y}, *quote, 3);
  drawTextCentered(renderer, page, SMALL_FONT_ID, footerY, tr(STR_DESKTOP_SLEEPING));
  return true;
}

const weather::Forecast* cachedForecast() {
  static weather::Forecast forecast;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    forecast.load();
  }
  return &forecast;
}

void drawTodayCard(const GfxRenderer& renderer, const TodayInfo& info, const weather::Forecast* forecast,
                   const Quote* quote) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int left = SIDE_PADDING;
  const int width = pageWidth - 2 * SIDE_PADDING;
  const Rect page{0, 0, pageWidth, pageHeight};
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int line = renderer.getLineHeight(UI_10_FONT_ID);
  char text[96];

  // Header: device name, and when this screen was drawn (it stays until wake).
  int y = 20;
  renderer.drawText(SMALL_FONT_ID, left, y, SETTINGS.deviceName[0] ? SETTINGS.deviceName : tr(STR_DEVICE_NAME_DEFAULT),
                    true, EpdFontFamily::BOLD);
  struct tm now {};
  const bool hasTime = halClock.localTime(now) && now.tm_year + 1900 >= MIN_VALID_YEAR;
  if (hasTime) {
    snprintf(text, sizeof(text), tr(STR_TODAY_UPDATED_FMT), now.tm_hour, now.tm_min);
    drawTextRight(renderer, left + width, y, SMALL_FONT_ID, text);
  }
  y += smallLine + 14;

  // Date: weekday, then day and month large, then the year.
  if (hasTime) {
    drawTextCentered(renderer, page, UI_12_FONT_ID, y, weekdayName(now.tm_wday), EpdFontFamily::BOLD);
    y += titleLine + 4;
    snprintf(text, sizeof(text), "%02d/%02d", now.tm_mday, now.tm_mon + 1);
    const BigText big(renderer, text);
    constexpr int DATE_HEIGHT = 64;
    if (big.valid()) {
      const int scale = big.scaleFor(DATE_HEIGHT, width);
      big.draw(renderer, text, (pageWidth - big.width(scale)) / 2, y + DATE_HEIGHT, scale);
    }
    y += DATE_HEIGHT + 10;
    snprintf(text, sizeof(text), "%04d", now.tm_year + 1900);
    drawTextCentered(renderer, page, UI_10_FONT_ID, y, text);
    y += line + 16;
  }

  // Weather: now, today's range, and the next two days.
  if (forecast && forecast->valid()) {
    constexpr int ICON = 76;
    const int cardH = ICON + 2 * CARD_PADDING + smallLine + 10;
    const Rect card{left, y, width, cardH};
    drawCard(renderer, card, false);
    const int ix = card.x + CARD_PADDING;
    const int iy = card.y + CARD_PADDING;
    drawWeatherIcon(renderer, ix, iy, ICON, weather::kindOf(forecast->code));
    const int tx = ix + ICON + 14;
    const int textW = card.x + card.width - CARD_PADDING - tx;
    // BigText reads one byte per codepoint, so the degree sign is Latin-1 0xB0.
    snprintf(text, sizeof(text), "%d\xB0", forecast->tempC);
    const BigText big(renderer, text);
    int ty = iy;
    if (big.valid()) {
      constexpr int TEMP_HEIGHT = 34;
      const int scale = big.scaleFor(TEMP_HEIGHT, textW / 2);
      big.draw(renderer, text, tx, ty + TEMP_HEIGHT, scale);
      renderer.drawText(UI_12_FONT_ID, tx + big.width(scale) + 12, ty + TEMP_HEIGHT - titleLine + 4,
                        renderer
                            .truncatedText(UI_12_FONT_ID, weather::describe(forecast->code),
                                           textW - big.width(scale) - 12, EpdFontFamily::BOLD)
                            .c_str(),
                        true, EpdFontFamily::BOLD);
      ty += TEMP_HEIGHT + 8;
    }
    char date[11] = "";
    const weather::Day* today = hasTime && weather::localDate(date, sizeof(date)) ? forecast->day(date) : nullptr;
    if (today) {
      snprintf(text, sizeof(text), tr(STR_TODAY_WEATHER_RANGE_FMT), today->maxC, today->minC,
               static_cast<int>(today->rainPct));
      renderer.drawText(SMALL_FONT_ID, tx, ty, renderer.truncatedText(SMALL_FONT_ID, text, textW).c_str());
      ty += smallLine + 2;
    }
    snprintf(text, sizeof(text), tr(STR_TODAY_WEATHER_PLACE_FMT), forecast->place.c_str(),
             static_cast<int>(forecast->humidity));
    renderer.drawText(SMALL_FONT_ID, tx, ty, renderer.truncatedText(SMALL_FONT_ID, text, textW).c_str());

    // The next two days along the bottom of the card.
    const int rowY = card.y + card.height - CARD_PADDING - smallLine - 4;
    const int colW = (card.width - 2 * CARD_PADDING) / 2;
    for (int offset = 1; offset <= 2 && hasTime; offset++) {
      if (!weather::localDate(date, sizeof(date), offset)) break;
      const weather::Day* next = forecast->day(date);
      if (!next) continue;
      const int cx = card.x + CARD_PADDING + (offset - 1) * colW;
      renderer.drawText(SMALL_FONT_ID, cx, rowY, weekdayShort((now.tm_wday + offset) % 7), true, EpdFontFamily::BOLD);
      const int iconX = cx + 36;
      drawWeatherIcon(renderer, iconX, rowY - 2, smallLine + 4, weather::kindOf(next->code));
      snprintf(text, sizeof(text), "%d\xC2\xB0 / %d\xC2\xB0", next->maxC, next->minC);
      renderer.drawText(SMALL_FONT_ID, iconX + smallLine + 10, rowY, text);
    }
    y += cardH + 14;
  }

  // Reading goal and vocabulary, side by side.
  const auto& stats = READING_STATS;
  const int32_t day = ReadingStatsStore::today();
  const int goal = std::max<int>(1, SETTINGS.dailyReadingGoalMinutes);
  const int minutes = static_cast<int>(stats.secondsOn(day) / 60);
  constexpr int RING = 30;
  const int boxW = (width - GAP) / 2;
  const int boxH = 2 * RING + 2 * CARD_PADDING + smallLine + 6;
  for (int i = 0; i < 2; i++) {
    const Rect box{left + i * (boxW + GAP), y, boxW, boxH};
    drawCard(renderer, box, false);
    const int ringX = box.x + CARD_PADDING + RING;
    const int ringY = box.y + CARD_PADDING + RING;
    const int infoX = ringX + RING + 12;
    const int infoW = box.x + box.width - CARD_PADDING - infoX;
    int iy = box.y + CARD_PADDING;
    if (i == 0) {
      drawRing(renderer, ringX, ringY, RING, 7, minutes * 100 / goal);
      snprintf(text, sizeof(text), "%d%%", std::min(999, minutes * 100 / goal));
      drawTextCentered(renderer, Rect{ringX - RING, 0, 2 * RING, 0}, SMALL_FONT_ID, ringY - smallLine / 2, text,
                       EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, infoX, iy, tr(STR_TODAY_READING), true, EpdFontFamily::BOLD);
      iy += smallLine + 4;
      snprintf(text, sizeof(text), "%d/%d %s", minutes, goal, tr(STR_DESKTOP_MIN));
      renderer.drawText(UI_10_FONT_ID, infoX, iy,
                        renderer.truncatedText(UI_10_FONT_ID, text, infoW, EpdFontFamily::BOLD).c_str(), true,
                        EpdFontFamily::BOLD);
      iy += line + 2;
      snprintf(text, sizeof(text), tr(STR_DESKTOP_STREAK_FMT), stats.currentStreak(day));
    } else {
      const int due = std::max(0, info.vocabDue);
      const int learnedPct = info.vocabTotal > 0 ? info.vocabLearned * 100 / info.vocabTotal : 0;
      drawRing(renderer, ringX, ringY, RING, 7, learnedPct);
      snprintf(text, sizeof(text), "%d", due);
      drawTextCentered(renderer, Rect{ringX - RING, 0, 2 * RING, 0}, UI_12_FONT_ID, ringY - titleLine / 2, text,
                       EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, infoX, iy, tr(STR_TODAY_VOCAB), true, EpdFontFamily::BOLD);
      iy += smallLine + 4;
      if (info.vocabDue < 0) {
        snprintf(text, sizeof(text), "%s", tr(STR_TODAY_NO_WORDS));
      } else {
        snprintf(text, sizeof(text), tr(STR_TODAY_VOCAB_DUE_FMT), due);
      }
      renderer.drawText(UI_10_FONT_ID, infoX, iy,
                        renderer.truncatedText(UI_10_FONT_ID, text, infoW, EpdFontFamily::BOLD).c_str(), true,
                        EpdFontFamily::BOLD);
      iy += line + 2;
      snprintf(text, sizeof(text), tr(STR_DESKTOP_STREAK_FMT), info.vocabStreak);
    }
    renderer.drawText(SMALL_FONT_ID, infoX, iy, renderer.truncatedText(SMALL_FONT_ID, text, infoW).c_str());
    // Ring caption: the share of today's goal, or of the word list learned.
    if (i == 1 && info.vocabTotal > 0) {
      snprintf(text, sizeof(text), tr(STR_VOCAB_LEARNED_FMT), info.vocabLearned, info.vocabTotal);
    } else {
      snprintf(text, sizeof(text), "%s", i == 0 ? tr(STR_DESKTOP_TODAY_LOWER) : "");
    }
    renderer.drawText(SMALL_FONT_ID, box.x + CARD_PADDING, box.y + box.height - CARD_PADDING - smallLine,
                      renderer.truncatedText(SMALL_FONT_ID, text, box.width - 2 * CARD_PADDING).c_str());
  }
  y += boxH + 14;

  // The book being read.
  if (info.book && !info.book->title.empty()) {
    renderer.drawText(SMALL_FONT_ID, left, y, tr(STR_TODAY_READING_NOW), true, EpdFontFamily::BOLD);
    y += smallLine + 2;
    char percent[8] = "";
    if (info.bookPercent >= 0) snprintf(percent, sizeof(percent), "%d%%", info.bookPercent);
    const int percentW = percent[0] ? renderer.getTextWidth(UI_10_FONT_ID, percent, EpdFontFamily::BOLD) : 0;
    renderer.drawText(UI_10_FONT_ID, left, y,
                      renderer
                          .truncatedText(UI_10_FONT_ID, info.book->title.c_str(), width - percentW - 12,
                                         EpdFontFamily::BOLD)
                          .c_str(),
                      true, EpdFontFamily::BOLD);
    if (percent[0]) drawTextRight(renderer, left + width, y, UI_10_FONT_ID, percent, EpdFontFamily::BOLD);
    y += line + 4;
    if (info.bookPercent >= 0) {
      drawBar(renderer, left, y, width, info.bookPercent);
      y += PROGRESS_BAR_HEIGHT;
    }
    y += 14;
  }

  const int footerY = pageHeight - 16 - smallLine;
  if (quote) drawQuote(renderer, Rect{left, y, width, footerY - 12 - y}, *quote, 4);
  drawTextCentered(renderer, page, SMALL_FONT_ID, footerY, tr(STR_DESKTOP_SLEEPING));
}

const Quote* quoteOfDay() {
  static Quote cached;
  static int32_t cachedDay = -2;
  static bool cachedFound = false;
  const int32_t day = ReadingStatsStore::today();
  if (day == cachedDay) return cachedFound ? &cached : nullptr;
  cachedDay = day;
  cachedFound = false;

  constexpr const char* PATH = "/My Clippings.txt";
  constexpr int32_t SCAN_LIMIT = 512 * 1024;
  constexpr size_t MAX_LINE = 600;
  const auto forEachEntry = [&](const auto& visit) {
    HalFile file;
    if (!Storage.openFileForRead("DESK", PATH, file)) return;
    char buffer[256];
    int length = 0;
    int position = 0;
    int32_t consumed = 0;
    std::string line;
    std::string title;
    std::string text;
    int lineInEntry = 0;
    bool highlight = false;
    while (true) {
      if (position >= length) {
        if (consumed >= SCAN_LIMIT) break;
        length = file.read(buffer, sizeof(buffer));
        if (length <= 0) break;
        position = 0;
        consumed += length;
      }
      const char c = buffer[position++];
      if (c == '\r') continue;
      if (c != '\n') {
        if (line.size() < MAX_LINE) line += c;
        continue;
      }
      if (line.rfind("==========", 0) == 0) {
        if (!visit(title, highlight ? text : std::string())) return;
        title.clear();
        text.clear();
        lineInEntry = 0;
        highlight = false;
      } else {
        if (lineInEntry == 0) {
          title = line.rfind("\xEF\xBB\xBF", 0) == 0 ? line.substr(3) : line;
          const size_t author = title.rfind(" (");
          if (author != std::string::npos && author > 0) title.resize(author);
        } else if (lineInEntry == 1) {
          highlight = line.find("Highlight") != std::string::npos;
        } else if (!line.empty()) {
          if (!text.empty()) text += ' ';
          text += line;
        }
        ++lineInEntry;
      }
      line.clear();
    }
  };

  int count = 0;
  forEachEntry([&](const std::string&, const std::string& text) {
    if (!text.empty()) ++count;
    return true;
  });
  if (count == 0 || day < 0) return nullptr;
  int target = day % count;
  forEachEntry([&](const std::string& title, const std::string& text) {
    if (text.empty()) return true;
    if (target-- > 0) return true;
    cached.text = text;
    cached.source = title;
    cachedFound = true;
    return false;
  });
  return cachedFound ? &cached : nullptr;
}

const char* batteryLabel() {
  static char label[32];
  const time_t now = time(nullptr);
  const int percent = std::min<int>(100, powerManager.getBatteryPercentage());
  if (READING_STATS.noteBattery(now, percent, gpio.isUsbConnected())) READING_STATS.saveToFile();
  const int days = READING_STATS.batteryDaysLeft(now, percent);
  if (days < 1) return nullptr;
  snprintf(label, sizeof(label), tr(STR_DESKTOP_BATTERY_DAYS), static_cast<unsigned>(percent), days);
  return label;
}

BookInfo readBookInfo(const RecentBook& book) {
  BookInfo info;
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    uint8_t data[6];
    if (!epub || !epub->load(false, true) || !readProgressBytes(epub->getCachePath(), data, sizeof(data))) {
      return info;
    }
    const int spine = data[0] | (data[1] << 8);
    const int page = data[2] | (data[3] << 8);
    const int pageCount = data[4] | (data[5] << 8);
    const float chapter = pageCount > 0 ? static_cast<float>(page + 1) / pageCount : 0.0f;
    const float progress = epub->calculateProgress(spine, chapter);
    info.percent = std::clamp(static_cast<int>(progress * 100.0f + 0.5f), 0, 100);
    const int chapterLeftBp =
        std::max(0, static_cast<int>((epub->calculateProgress(spine, 1.0f) - progress) * 10000.0f + 0.5f));
    info.chapterSecondsLeft = READING_STATS.secondsForProgress(book.path, chapterLeftBp);
    const int tocIndex = epub->getTocIndexForSpineIndex(spine);
    if (tocIndex >= 0) info.chapter = epub->getTocItem(tocIndex).title;
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    uint8_t data[4];
    if (!xtc || !xtc->load() || !readProgressBytes(xtc->getCachePath(), data, sizeof(data))) return info;
    const uint32_t page = data[0] | (data[1] << 8) | (data[2] << 16) | (static_cast<uint32_t>(data[3]) << 24);
    const uint32_t pageCount = xtc->getPageCount();
    if (pageCount == 0) return info;
    info.percent = std::clamp(static_cast<int>((page + 1) * 100 / pageCount), 0, 100);
  }
  if (info.percent >= 0) info.secondsLeft = READING_STATS.secondsLeft(book.path, info.percent * 100);
  return info;
}

}  // namespace DesktopHomeUi
