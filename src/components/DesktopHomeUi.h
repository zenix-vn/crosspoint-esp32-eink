#pragma once

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;
namespace weather {
struct Forecast;
}

namespace DesktopHomeUi {

// Same thumb height as Lyra's home card, so both themes share cached covers.
constexpr int COVER_HEIGHT = 226;
constexpr int RECENT_COVER_HEIGHT = 120;
constexpr int MAX_RECENTS = 4;
constexpr int MAX_RECOMMENDED = 2;        // cards on Home
constexpr int MAX_RECOMMENDED_PAGE = 15;  // books on the "Recommended" page
constexpr int DOCK_COUNT = 4;
constexpr int BOOK_LIST_PAGE_SIZE = 5;

struct BookInfo {
  int percent = -1;                 // -1 = unknown
  std::string chapter;              // empty = unknown
  int32_t secondsLeft = -1;         // estimated reading time to the end, -1 = unknown
  int32_t chapterSecondsLeft = -1;  // same, to the end of the current chapter
};

struct RecommendedBook {
  RecentBook book;
  std::string reason;
};

// A highlight from /My Clippings.txt, picked once per day.
struct Quote {
  std::string text;
  std::string source;  // book title
};

// Figures for the vocabulary statistics page, gathered by the vocabulary trainer.
struct VocabStats {
  static constexpr int DAYS = 7;
  int32_t today = -1;
  int score = -1;  // 0-100 rating of the last week, -1 = nothing studied
  int activeDays = 0;
  int streak = 0;
  int best = 0;
  int weekCards = 0;
  int weekNew = 0;
  int recall = -1;  // percent of reviews remembered, -1 = no reviews
  int quiz = -1;    // percent of quiz answers right, -1 = no quiz
  int fresh = 0;
  int learning = 0;
  int familiar = 0;
  int mastered = 0;
  int dueToday = 0;
  int dueTomorrow = 0;
  int dueWeek = 0;
  int dayCards[DAYS] = {};  // oldest first, today last
  int dayNew[DAYS] = {};
  const char* tip = "";
};

// What the "Today" sleep screen shows besides the date, weather and quote.
struct TodayInfo {
  int vocabDue = -1;  // words due for review, -1 = no word list
  int vocabStreak = 0;
  int vocabLearned = 0;
  int vocabTotal = 0;
  const RecentBook* book = nullptr;  // book being read, may be null
  int bookPercent = -1;
};

enum class DockGlyph : uint8_t { Library, Stats, Settings, Vocab, More };

struct DockItem {
  const char* label;
  DockGlyph glyph;
};

struct Layout {
  Rect header;
  Rect clock;
  Rect hero;
  Rect stats;
  Rect recents;
  Rect recommended;
  Rect dock;
};

// Action sheet rising from the dock: the "More" places and the book menu.
enum class SheetGlyph : uint8_t { Folder, Transfer, Plugins, Stats, Details, Remove, Heart, Light, Settings };

struct SheetItem {
  const char* label;
  SheetGlyph glyph;
};

Layout layout(const GfxRenderer& renderer);
Rect sheetRect(const Layout& layout, int count, const Rect& anchor);
Rect sheetRow(const Rect& sheet, int index);
void drawSheet(const GfxRenderer& renderer, const Rect& sheet, const Rect& anchor, const char* title,
               const SheetItem* items, int count, int selected);

Rect recentTabRect(const Layout& layout, bool favoritesTab);
Rect seeAllRect(const Layout& layout);
Rect recommendedSeeMoreRect(const Layout& layout);
Rect recentSlot(const Layout& layout, int index);
Rect recommendedSlot(const Layout& layout, int index);
Rect dockSlot(const Layout& layout, int index);

void drawClock(const GfxRenderer& renderer, Rect rect);
void drawHero(const GfxRenderer& renderer, Rect rect, const RecentBook* book, const BookInfo& info, bool focused);
void drawRecents(const GfxRenderer& renderer, const Layout& layout, const RecentBook* books, int count, int focused,
                 bool favoritesTab = false, bool seeAllFocused = false);
void drawRecommended(const GfxRenderer& renderer, const Layout& layout, const RecommendedBook* books, int count,
                     int focused, bool seeMoreFocused = false);
void drawStatsCard(const GfxRenderer& renderer, Rect rect, bool focused, const Quote* quote);
void drawDock(const GfxRenderer& renderer, const Layout& layout, const DockItem* items, int focused);
void drawQuote(const GfxRenderer& renderer, Rect rect, const Quote& quote, int maxLines);
void drawStatsPage(const GfxRenderer& renderer, Rect area);
void drawVocabStatsPage(const GfxRenderer& renderer, Rect area, const VocabStats& stats);

// Dedicated "See all" book list page (Recent Books & Favorite Books tabs).
Rect bookListTabRect(Rect area, bool favoritesTab);
Rect bookListRowRect(Rect area, int rowIndex);
Rect bookListHeartRect(const Rect& rowRect);
Rect bookListPrevPageRect(Rect area);
Rect bookListNextPageRect(Rect area);
void drawBookListPage(const GfxRenderer& renderer, Rect area, const RecentBook* books, int totalCount, int page,
                      int selectedInPage, bool favoritesTab, int recentTotal, int favTotal);

// Dedicated book pages ("Recommended", "Unfinished"). Shares the book list's
// rows and pager, with `caption` where the list has its tabs and each book's
// `reason` as the tag above its title.
void drawRecommendedPage(const GfxRenderer& renderer, Rect area, const RecommendedBook* books, int totalCount,
                         int page, int selectedInPage, const char* caption, const char* emptyText);

bool drawSleepCard(const GfxRenderer& renderer, const RecentBook& book, const BookInfo& info, const Quote* quote);
// The "Today" sleep screen: date, weather, reading goal, words to review,
// streaks, the current book and a quote.
void drawTodayCard(const GfxRenderer& renderer, const TodayInfo& info, const weather::Forecast* forecast,
                   const Quote* quote);
// The saved forecast, read once per boot (it only changes on the way to sleep).
const weather::Forecast* cachedForecast();

const Quote* quoteOfDay();
const char* batteryLabel();

BookInfo readBookInfo(const RecentBook& book);

}  // namespace DesktopHomeUi
