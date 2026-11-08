#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <LibraryText.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HapticFeedback.h"
#include "HiddenRecommendations.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "activities/network/InboxActivity.h"
#include "activities/dictionary/DictionaryActivity.h"
#include "activities/vocab/VocabActivity.h"
#include "activities/util/DetailsActivity.h"
#include "components/DesktopHomeUi.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

std::string parentDirOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return path.substr(0, slash);
}

std::string toLowerAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool authorMatches(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;
  const std::string la = toLowerAscii(a);
  const std::string lb = toLowerAscii(b);
  return la == lb || la.find(lb) != std::string::npos || lb.find(la) != std::string::npos;
}

bool sharesTitleKeyword(const std::string& titleA, const std::string& titleB) {
  if (titleA.empty() || titleB.empty()) return false;
  const std::string la = toLowerAscii(titleA);
  const std::string lb = toLowerAscii(titleB);
  size_t pos = 0;
  while (pos < la.size()) {
    while (pos < la.size() && (la[pos] == ' ' || la[pos] == '-' || la[pos] == ':' || la[pos] == ',')) ++pos;
    size_t end = pos;
    while (end < la.size() && la[end] != ' ' && la[end] != '-' && la[end] != ':' && la[end] != ',') ++end;
    if (end - pos >= 4) {
      const std::string word = la.substr(pos, end - pos);
      if (lb.find(word) != std::string::npos) return true;
    }
    pos = end;
  }
  return false;
}

struct GenreTerm {
  std::string folded;
  int weight;
};

// How far the reader has got in a series they have opened or favorited.
struct SeriesProgress {
  std::string folded;
  float maxIndex;  // highest position seen, -1 when no book gave one
};

// Cheap per-day jitter so equal scores rotate instead of always surfacing the
// same alphabetical neighbours.
uint32_t dailyJitter(const int32_t day, const uint16_t ordinal) {
  uint32_t h = static_cast<uint32_t>(day) * 2654435761u ^ static_cast<uint32_t>(ordinal) * 40503u;
  h ^= h >> 15;
  return h % 5;
}

}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Library, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasLibrarySlot()) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    if (recentBooks.size() >= static_cast<size_t>(maxBooks)) {
      break;
    }
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, height).c_str()))
    return;
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (Storage.exists(epub->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (Storage.exists(xtc->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
      if (!Storage.exists(coverPath.c_str())) {
        if (FsHelpers::hasReflowableBookExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          epub.load(false, true);

          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(thumbHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(thumbHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  hasPlugins = SETTINGS.pluginsEnabled != 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  desktopHome = !coverGridUi && UITheme::hasDesktopHome();
  if (desktopHome) {
    reloadDesktopBooks();
  } else {
    loadRecentBooks(coverGridUi ? CoverGridHomeUi::MAX_BOOKS : metrics.homeRecentBooksCount);
    hasContinueReading = !recentBooks.empty();
  }
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    coverGridUi->begin(recentBooks, hasLibrarySlot(), hasContinueReading);
  }

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasLibrarySlot());
  if (desktopHome) selectorIndex = 0;

  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
  coverGridUi.reset();
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // A gray pass repaints the frame into a gray plane; keep the B/W copy.
  if (renderer.inUiGrayPass()) return coverBuffer != nullptr;
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  if (desktopHome) {
    loopDesktop();
    return;
  }
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      onRecentBookSelected(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasLibrarySlot())) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        hasPlugins ? onPluginsOpen() : onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  if (!coverGridUi) {
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    onRecentBookSelected(recentBooks[0].path);
    return;
  }

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      selectorIndex = base + idx;
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, +1);
    });
    return;
  }

  const Rect content = UITheme::getInstance().getContentArea(renderer);
  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), coverColumnCount);
  const int coverColumnWidth = (content.width - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, content.x + metrics.contentSidePadding, coverColumnWidth,
                                               recentCount, metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                              0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

int HomeActivity::desktopShelfCount() const {
  if (desktopFavoritesTab) {
    return std::min(DesktopHomeUi::MAX_RECENTS, static_cast<int>(favoriteBooks.size()));
  }
  if (recentBooks.size() <= 1) return 0;
  return std::min(DesktopHomeUi::MAX_RECENTS, static_cast<int>(recentBooks.size()) - 1);
}

int HomeActivity::desktopRecommendedCount() const {
  return std::min(DesktopHomeUi::MAX_RECOMMENDED, static_cast<int>(recommendedBooks.size()));
}

// Focus order:
// 0: hero card
// 1 .. shelfCount: shelf covers (Recent or Favorites)
// seeAll: "Xem tất cả >" of the shelf
// recMore: "Xem thêm >" of the recommendations ("NÊN ĐỌC")
// recBase .. recBase + recCount - 1: recommended cards
// dockBase .. dockBase + DOCK_COUNT - 1: dock
HomeActivity::DesktopFocusMap HomeActivity::desktopFocusMap() const {
  DesktopFocusMap map{};
  map.shelfCount = desktopShelfCount();
  map.seeAll = 1 + map.shelfCount;
  map.recMore = map.seeAll + 1;
  map.recBase = map.recMore + 1;
  map.recCount = desktopRecommendedCount();
  map.dockBase = map.recBase + map.recCount;
  map.count = map.dockBase + DesktopHomeUi::DOCK_COUNT;
  return map;
}

int HomeActivity::desktopFocusCount() const { return desktopFocusMap().count; }

void HomeActivity::reloadDesktopBooks() {
  loadRecentBooks(RecentBooksStore::MAX_RECENT_BOOKS);
  favoriteBooks.clear();
  for (const RecentBook& fav : RECENT_BOOKS.getFavorites()) {
    if (!RecentBooksStore::isMissing(fav)) favoriteBooks.push_back(fav);
  }
  hasContinueReading = !recentBooks.empty();
  desktopBook = hasContinueReading ? DesktopHomeUi::readBookInfo(recentBooks[0]) : DesktopHomeUi::BookInfo{};
  loadRecommendedBooks();
  recentsLoaded = false;
  recommendedCoversPage = -1;
}

void HomeActivity::loadRecommendedBooks() {
  recommendedBooks.clear();
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) return;
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) return;

  // How much a profile book says about taste, learned from the reading stats:
  // finishing it or spending hours in it counts for more than opening it, and
  // a book dropped after a few minutes counts for nothing. The book being read
  // right now is spared that, since it may simply have just been started.
  const auto& stats = READING_STATS;
  const auto bookWeight = [&](const RecentBook& b, const bool favorite) {
    const bool finished = stats.wasFinished(b.path);
    const int32_t seconds = stats.secondsReading(b.path);
    const bool current = !recentBooks.empty() && recentBooks[0].path == b.path;
    if (!favorite && !finished && !current && seconds >= 0 && seconds < 5 * 60) return 0;
    int weight = favorite ? 2 : 1;
    if (finished) weight += 2;
    if (seconds >= 2 * 3600) {
      weight += 2;
    } else if (seconds >= 30 * 60) {
      weight += 1;
    }
    return weight;
  };

  // Build user reading profile from read books + favorite books.
  struct WeightedAuthor {
    std::string name;
    int weight;
  };
  std::vector<WeightedAuthor> profileAuthors;
  std::vector<std::string> profileFolders;
  std::vector<std::string> profileTitles;

  const auto addProfile = [&](const RecentBook& b, const int weight) {
    if (weight <= 0) return;
    if (!b.author.empty()) {
      auto it = std::find_if(profileAuthors.begin(), profileAuthors.end(),
                             [&](const WeightedAuthor& a) { return authorMatches(a.name, b.author); });
      if (it != profileAuthors.end()) {
        it->weight += weight;
      } else {
        profileAuthors.push_back(WeightedAuthor{b.author, weight});
      }
    }
    const std::string dir = parentDirOf(b.path);
    if (dir != "/" && std::find(profileFolders.begin(), profileFolders.end(), dir) == profileFolders.end()) {
      profileFolders.push_back(dir);
    }
    if (!b.title.empty() && profileTitles.size() < 8) {
      profileTitles.push_back(b.title);
    }
  };
  for (const auto& b : recentBooks) addProfile(b, bookWeight(b, false));
  for (const auto& b : favoriteBooks) addProfile(b, bookWeight(b, true));

  // Genre terms and series progress come from what the index stored for the
  // profile books. One batched lookup finds them all, weighted as above.
  std::vector<GenreTerm> profileGenres;
  std::vector<SeriesProgress> profileSeries;
  {
    constexpr size_t MAX_PROFILE_GENRES = 24;
    constexpr size_t MAX_PROFILE_SERIES = 16;
    library::BookIdentity ids[library::LibraryIndexFile::MAX_IDENTITY_LOOKUPS];
    int weights[library::LibraryIndexFile::MAX_IDENTITY_LOOKUPS];
    size_t idCount = 0;
    const auto addIdentity = [&](const RecentBook& b, const int weight) {
      if (weight <= 0 || idCount >= library::LibraryIndexFile::MAX_IDENTITY_LOOKUPS) return;
      ids[idCount] = {library::clixPathHash(b.path.data(), b.path.size()), 0};
      weights[idCount++] = weight;
    };
    for (const auto& b : favoriteBooks) addIdentity(b, bookWeight(b, true));
    for (const auto& b : recentBooks) addIdentity(b, bookWeight(b, false));

    uint16_t rows[library::LibraryIndexFile::MAX_IDENTITY_LOOKUPS];
    std::string subjects;
    std::string series;
    float seriesIndex = -1.0f;
    if (idCount > 0 && index.recentRowsFor(ids, idCount, rows)) {
      for (size_t i = 0; i < idCount; ++i) {
        if (rows[i] == 0xFFFF) continue;
        const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentAsc, rows[i]);
        if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
        if (index.readSubjects(record, subjects)) {
          library::forEachSubjectTerm(subjects, [&](const std::string& term, std::string_view) {
            auto it = std::find_if(profileGenres.begin(), profileGenres.end(),
                                   [&](const GenreTerm& g) { return g.folded == term; });
            if (it != profileGenres.end()) {
              it->weight += weights[i];
            } else if (profileGenres.size() < MAX_PROFILE_GENRES) {
              profileGenres.push_back(GenreTerm{term, weights[i]});
            }
          });
        }
        if (index.readSeries(record, series, seriesIndex) && !series.empty()) {
          std::string folded = library::fold(series);
          auto it = std::find_if(profileSeries.begin(), profileSeries.end(),
                                 [&](const SeriesProgress& s) { return s.folded == folded; });
          if (it != profileSeries.end()) {
            it->maxIndex = std::max(it->maxIndex, seriesIndex);
          } else if (profileSeries.size() < MAX_PROFILE_SERIES) {
            profileSeries.push_back(SeriesProgress{std::move(folded), seriesIndex});
          }
        }
      }
    }
  }

  // Already-read and dismissed books are excluded by path hash, so a book is
  // rejected without rebuilding its path from the folder section.
  std::vector<uint64_t> excluded = hidden_recommendations::pathHashes();
  excluded.reserve(excluded.size() + recentBooks.size());
  for (const auto& b : recentBooks) excluded.push_back(library::clixPathHash(b.path.data(), b.path.size()));

  const uint16_t total = index.bookCount();
  if (total == 0) return;

  // The newest arrivals on the card, from the index's own arrival order.
  constexpr uint16_t NEWEST_COUNT = 15;
  std::vector<uint16_t> newest;
  for (uint16_t row = 0; row < std::min(NEWEST_COUNT, total); ++row) {
    const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentDesc, row);
    if (ordinal != 0xFFFF) newest.push_back(ordinal);
  }

  // Ordered by strength: when several apply, the label names the strongest.
  enum class Reason : uint8_t { ForYou, Folder, New, Genre, Author, SameSeries, NextInSeries };
  struct Candidate {
    uint16_t ordinal;
    int score;
    Reason reason;
    std::string genre;  // shown spelling of the strongest matching genre, for Reason::Genre
  };
  // Only the best survive the scan; their paths are resolved afterwards, since
  // a missing file or the author limit can still knock some out.
  constexpr size_t MAX_CANDIDATES = 30;
  std::vector<Candidate> candidates;
  candidates.reserve(MAX_CANDIDATES + 1);

  // Records and their name blobs are both laid out in title order, so walking
  // ordinals reads both sections sequentially. That is what makes a scan of the
  // whole library affordable; the cap and time budget bound very large cards.
  constexpr uint16_t MAX_SCAN = 1000;
  constexpr uint32_t SCAN_BUDGET_MS = 500;
  const uint16_t scanCap = std::min<uint16_t>(total, MAX_SCAN);
  const int32_t daySeed = std::max<int32_t>(0, ReadingStatsStore::today());
  // When the cap is hit, the window moves each day so the rest gets a turn.
  const uint16_t start = static_cast<uint16_t>((static_cast<uint32_t>(daySeed) * 97u) % total);
  const uint32_t scanStart = millis();

  std::string title;
  std::string author;
  std::string subjects;
  std::string series;
  std::string path;
  for (uint16_t i = 0; i < scanCap; ++i) {
    if ((i & 31) == 31 && millis() - scanStart > SCAN_BUDGET_MS) {
      LOG_DBG("HOME", "Recommendation scan stopped at %u/%u books (time budget)", static_cast<unsigned>(i),
              static_cast<unsigned>(total));
      break;
    }
    const uint16_t ordinal = static_cast<uint16_t>((start + i) % total);
    uint64_t pathHash = 0;
    if (!index.readRecord(ordinal, record) || !index.readPathHash(record, pathHash)) {
      if (index.ioFailed()) break;
      continue;
    }
    if (std::find(excluded.begin(), excluded.end(), pathHash) != excluded.end()) continue;
    if (!index.readTitle(record, title) && !index.readName(record, title)) continue;
    index.readAuthor(record, author);
    index.readSubjects(record, subjects);
    float seriesIndex = -1.0f;
    index.readSeries(record, series, seriesIndex);
    if (index.ioFailed()) break;

    int score = static_cast<int>(dailyJitter(daySeed, ordinal));
    Reason reason = Reason::ForYou;
    const auto raise = [&reason](const Reason r) {
      if (r > reason) reason = r;
    };

    if (!series.empty() && !profileSeries.empty()) {
      const std::string folded = library::fold(series);
      for (const auto& s : profileSeries) {
        if (s.folded != folded) continue;
        // The volume right after the furthest one read is the obvious next
        // pick; anything else in the series still beats a stranger.
        if (s.maxIndex >= 0.0f && seriesIndex > s.maxIndex && seriesIndex <= s.maxIndex + 1.0f) {
          score += 70;
          raise(Reason::NextInSeries);
        } else {
          score += 35;
          raise(Reason::SameSeries);
        }
        break;
      }
    }

    // An author read for hours, or finished, outranks one merely opened.
    const auto authorIt = std::find_if(profileAuthors.begin(), profileAuthors.end(),
                                       [&](const WeightedAuthor& a) { return authorMatches(a.name, author); });
    if (authorIt != profileAuthors.end()) {
      score += 25 + 5 * std::min(authorIt->weight, 5);
      raise(Reason::Author);
    }

    int genreWeight = 0;
    int bestGenreWeight = 0;
    std::string_view bestGenre;  // points into `subjects`; copied before the next book
    if (!profileGenres.empty()) {
      library::forEachSubjectTerm(subjects, [&](const std::string& term, const std::string_view shown) {
        for (const auto& g : profileGenres) {
          if (g.folded != term) continue;
          genreWeight += g.weight;
          if (g.weight > bestGenreWeight) {
            bestGenreWeight = g.weight;
            bestGenre = shown;
          }
        }
      });
    }
    if (genreWeight > 0) {
      score += std::min(genreWeight * 10, 30);
      raise(Reason::Genre);
    }

    if (std::find(newest.begin(), newest.end(), ordinal) != newest.end()) {
      score += 12;
      raise(Reason::New);
    }

    // The folder needs the path; only books that could still place pay for it
    // (8 for the folder plus 6 for a title keyword is the most left to gain).
    if (!profileFolders.empty() &&
        (candidates.size() < MAX_CANDIDATES || score + 14 > candidates.back().score) && index.readPath(record, path)) {
      const std::string dir = parentDirOf(path);
      if (dir != "/" && std::find(profileFolders.begin(), profileFolders.end(), dir) != profileFolders.end()) {
        score += 8;
        raise(Reason::Folder);
      }
    }

    if (std::any_of(profileTitles.begin(), profileTitles.end(),
                    [&](const std::string& t) { return sharesTitleKeyword(t, title); })) {
      score += 6;
    }

    if (candidates.size() >= MAX_CANDIDATES && score <= candidates.back().score) continue;
    const auto at = std::upper_bound(candidates.begin(), candidates.end(), score,
                                     [](const int s, const Candidate& c) { return s > c.score; });
    candidates.insert(at, Candidate{ordinal, score, reason,
                                    reason == Reason::Genre ? std::string(bestGenre) : std::string()});
    if (candidates.size() > MAX_CANDIDATES) candidates.pop_back();
  }

  const auto reasonText = [](const Candidate& c) -> std::string {
    switch (c.reason) {
      case Reason::NextInSeries:
        return tr(STR_RECOMMEND_NEXT_IN_SERIES);
      case Reason::SameSeries:
        return tr(STR_RECOMMEND_SAME_SERIES);
      case Reason::Author:
        return tr(STR_RECOMMEND_SAME_AUTHOR);
      case Reason::Genre:
        // The genre itself says more than "Same genre".
        return c.genre.empty() ? std::string(tr(STR_RECOMMEND_SAME_GENRE)) : c.genre;
      case Reason::New:
        return tr(STR_RECOMMEND_NEW);
      case Reason::Folder:
        return tr(STR_RECOMMEND_SAME_FOLDER);
      default:
        return tr(STR_RECOMMEND_FOR_YOU);
    }
  };

  // Home shows the first MAX_RECOMMENDED, so those get distinct authors; the
  // page allows two per author. The next volume of a series is exempt, since
  // its author naturally repeats. A second pass fills whatever is left.
  const auto authorCount = [this](const std::string& name) {
    return std::count_if(recommendedBooks.begin(), recommendedBooks.end(),
                         [&](const DesktopHomeUi::RecommendedBook& r) { return authorMatches(r.book.author, name); });
  };
  const size_t wanted = DesktopHomeUi::MAX_RECOMMENDED_PAGE;
  for (int pass = 0; pass < 2 && recommendedBooks.size() < wanted; ++pass) {
    for (const auto& c : candidates) {
      if (recommendedBooks.size() >= wanted) break;
      if (!index.readRecord(c.ordinal, record)) continue;
      RecentBook book;
      if (!index.readPath(record, book.path)) continue;
      if (std::any_of(recommendedBooks.begin(), recommendedBooks.end(),
                      [&](const DesktopHomeUi::RecommendedBook& r) { return r.book.path == book.path; })) {
        continue;
      }
      if (RecentBooksStore::isMissing(book)) continue;
      if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
      index.readAuthor(record, book.author);
      if (pass == 0 && c.reason != Reason::NextInSeries && !book.author.empty()) {
        const auto limit = recommendedBooks.size() < static_cast<size_t>(DesktopHomeUi::MAX_RECOMMENDED) ? 1 : 2;
        if (authorCount(book.author) >= limit) continue;
      }
      recommendedBooks.push_back(DesktopHomeUi::RecommendedBook{std::move(book), reasonText(c)});
    }
  }
}

void HomeActivity::loadDesktopCovers() {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;
  const size_t recentCap = std::min<size_t>(recentBooks.size(), 1 + DesktopHomeUi::MAX_RECENTS);
  for (size_t i = 0; i < recentCap; ++i) {
    const int height = i == 0 ? DesktopHomeUi::COVER_HEIGHT : DesktopHomeUi::RECENT_COVER_HEIGHT;
    loadGridCover(recentBooks[i], height, showingLoading, popupRect);
  }
  const size_t favCap = std::min<size_t>(favoriteBooks.size(), DesktopHomeUi::MAX_RECENTS);
  for (size_t i = 0; i < favCap; ++i) {
    loadGridCover(favoriteBooks[i], DesktopHomeUi::RECENT_COVER_HEIGHT, showingLoading, popupRect);
  }
  // Only the Home cards; the Recommended page loads its own covers a page at a time.
  const size_t recCap = std::min<size_t>(recommendedBooks.size(), DesktopHomeUi::MAX_RECOMMENDED);
  for (size_t i = 0; i < recCap; ++i) {
    loadGridCover(recommendedBooks[i].book, DesktopHomeUi::RECENT_COVER_HEIGHT, showingLoading, popupRect);
  }
  recentsLoaded = true;
  recentsLoading = false;
  if (showingLoading) requestUpdate();
}

void HomeActivity::loopDesktopBookList() {
  const auto& list = desktopFavoritesTab ? favoriteBooks : recentBooks;
  const int totalCount = static_cast<int>(list.size());
  const int totalPages = std::max(1, (totalCount + DesktopHomeUi::BOOK_LIST_PAGE_SIZE - 1) / DesktopHomeUi::BOOK_LIST_PAGE_SIZE);
  desktopBookListPage = std::clamp(desktopBookListPage, 0, totalPages - 1);
  const int pageCount = std::clamp(totalCount - desktopBookListPage * DesktopHomeUi::BOOK_LIST_PAGE_SIZE, 0,
                                   DesktopHomeUi::BOOK_LIST_PAGE_SIZE);
  desktopBookListSelected = std::clamp(desktopBookListSelected, 0, std::max(0, pageCount - 1));

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight;
  const Rect content = UITheme::getContentArea(renderer);
  const Rect area{content.x, top, content.width, content.y + content.height - metrics.buttonHintsHeight - top};
  const auto contains = [](const Rect& r, const int x, const int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
  };

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenLongPress(x, y)) {
    for (int i = 0; i < pageCount; ++i) {
      const Rect row = DesktopHomeUi::bookListRowRect(area, i);
      if (contains(row, x, y)) {
        desktopBookListSelected = i;
        const int idx = desktopBookListPage * DesktopHomeUi::BOOK_LIST_PAGE_SIZE + i;
        openBookActionsFor(list[idx], row);
        return;
      }
    }
  }

  if (mappedInput.wasScreenTapped(x, y)) {
    // Header back button (top-left)
    if (y < top && x < 100) {
      haptic_feedback::touchAction();
      desktopBookListOpen = false;
      requestUpdate();
      return;
    }
    // Top segmented tabs: Recent | Favorites
    if (contains(DesktopHomeUi::bookListTabRect(area, false), x, y)) {
      if (desktopFavoritesTab) {
        haptic_feedback::touchAction();
        desktopFavoritesTab = false;
        desktopBookListPage = 0;
        desktopBookListSelected = 0;
        requestUpdate();
      }
      return;
    }
    if (contains(DesktopHomeUi::bookListTabRect(area, true), x, y)) {
      if (!desktopFavoritesTab) {
        haptic_feedback::touchAction();
        desktopFavoritesTab = true;
        desktopBookListPage = 0;
        desktopBookListSelected = 0;
        requestUpdate();
      }
      return;
    }
    // Pagination buttons
    if (totalPages > 1) {
      if (contains(DesktopHomeUi::bookListPrevPageRect(area), x, y)) {
        haptic_feedback::touchAction();
        desktopBookListPage = (desktopBookListPage + totalPages - 1) % totalPages;
        desktopBookListSelected = 0;
        requestUpdate();
        return;
      }
      if (contains(DesktopHomeUi::bookListNextPageRect(area), x, y)) {
        haptic_feedback::touchAction();
        desktopBookListPage = (desktopBookListPage + 1) % totalPages;
        desktopBookListSelected = 0;
        requestUpdate();
        return;
      }
    }
    // Book rows (heart icon on right toggles favorite; row body opens book)
    for (int i = 0; i < pageCount; ++i) {
      const Rect row = DesktopHomeUi::bookListRowRect(area, i);
      if (contains(row, x, y)) {
        haptic_feedback::touchAction();
        const int idx = desktopBookListPage * DesktopHomeUi::BOOK_LIST_PAGE_SIZE + i;
        if (contains(DesktopHomeUi::bookListHeartRect(row), x, y)) {
          RECENT_BOOKS.toggleFavorite(list[idx]);
          reloadDesktopBooks();
          requestUpdate();
          return;
        }
        desktopBookListSelected = i;
        onRecentBookSelected(list[idx].path);
        return;
      }
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left && totalPages > 1) {
    desktopBookListPage = (desktopBookListPage + 1) % totalPages;
    desktopBookListSelected = 0;
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right && totalPages > 1) {
    desktopBookListPage = (desktopBookListPage + totalPages - 1) % totalPages;
    desktopBookListSelected = 0;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    desktopBookListOpen = false;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && pageCount > 0) {
    const int idx = desktopBookListPage * DesktopHomeUi::BOOK_LIST_PAGE_SIZE + desktopBookListSelected;
    if (mappedInput.getHeldTime() >= DESKTOP_LONG_PRESS_MS) {
      openBookActionsFor(list[idx], DesktopHomeUi::bookListRowRect(area, desktopBookListSelected));
    } else {
      onRecentBookSelected(list[idx].path);
    }
    return;
  }

  buttonNavigator.onNext([this, pageCount, totalPages] {
    if (pageCount <= 0) return;
    if (desktopBookListSelected + 1 < pageCount) {
      ++desktopBookListSelected;
    } else if (totalPages > 1) {
      desktopBookListPage = (desktopBookListPage + 1) % totalPages;
      desktopBookListSelected = 0;
    } else {
      desktopBookListSelected = 0;
    }
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, pageCount, totalPages] {
    if (pageCount <= 0) return;
    if (desktopBookListSelected > 0) {
      --desktopBookListSelected;
    } else if (totalPages > 1) {
      desktopBookListPage = (desktopBookListPage + totalPages - 1) % totalPages;
      desktopBookListSelected = 0;
    }
    requestUpdate();
  });
}

std::vector<DesktopHomeUi::RecommendedBook>& HomeActivity::pageBooks() {
  return desktopListPage == DesktopListPage::Unfinished ? unfinishedBooks : recommendedBooks;
}

void HomeActivity::openVocab() {
  auto vocab = makeUniqueNoThrow<VocabActivity>(renderer, mappedInput);
  if (!vocab) {
    LOG_ERR("HOME", "OOM: vocabulary trainer");
    return;
  }
  startActivityForResult(std::move(vocab), [this](const ActivityResult&) { requestUpdate(); });
}

void HomeActivity::openRecommendedPage() {
  desktopListPage = DesktopListPage::Recommended;
  desktopBookListPage = 0;
  desktopBookListSelected = 0;
  recommendedCoversPage = -1;
  requestUpdate();
}

void HomeActivity::openUnfinishedPage() {
  loadUnfinishedBooks();
  desktopListPage = DesktopListPage::Unfinished;
  desktopBookListPage = 0;
  desktopBookListSelected = 0;
  recommendedCoversPage = -1;
  requestUpdate();
}

// Books begun and then left: 5-95% read, not the one open right now, and not
// touched for a while. The longest-neglected come first, since those are the
// ones a reader forgets.
// Every recent book started but not finished, most recently read first (the
// recents order), with how long ago it was last read.
void HomeActivity::loadUnfinishedBooks() {
  constexpr int MIN_PERCENT = 1;
  constexpr int MAX_PERCENT = 98;
  unfinishedBooks.clear();
  const int32_t today = ReadingStatsStore::today();
  struct Started {
    RecentBook book;
    int percent;
    int32_t idleDays;  // -1 when the stats do not know the book
  };
  std::vector<Started> started;
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (RecentBooksStore::isMissing(book)) continue;
    const int percent = DesktopHomeUi::readBookInfo(book).percent;
    if (percent < MIN_PERCENT || percent > MAX_PERCENT || READING_STATS.wasFinished(book.path)) continue;
    const int32_t lastDay = READING_STATS.lastReadDay(book.path);
    const int32_t idleDays = today >= 0 && lastDay >= 0 ? today - lastDay : -1;
    started.push_back(Started{book, percent, idleDays});
  }
  for (auto& s : started) {
    char reason[48];
    if (s.idleDays > 0) {
      snprintf(reason, sizeof(reason), tr(STR_UNFINISHED_REASON_FMT), s.percent, static_cast<int>(s.idleDays));
    } else {
      snprintf(reason, sizeof(reason), "%d%%", s.percent);
    }
    unfinishedBooks.push_back(DesktopHomeUi::RecommendedBook{std::move(s.book), reason});
  }
}

void HomeActivity::loadRecommendedPageCovers() {
  if (recommendedCoversPage == desktopBookListPage) return;
  // Marked first: a cover that fails to generate must not retrigger this on
  // every redraw.
  recommendedCoversPage = desktopBookListPage;
  bool showingLoading = false;
  Rect popupRect;
  auto& books = pageBooks();
  const int start = desktopBookListPage * DesktopHomeUi::BOOK_LIST_PAGE_SIZE;
  const int end = std::min(start + DesktopHomeUi::BOOK_LIST_PAGE_SIZE, static_cast<int>(books.size()));
  for (int i = start; i < end; ++i) {
    loadGridCover(books[i].book, DesktopHomeUi::RECENT_COVER_HEIGHT, showingLoading, popupRect);
  }
  if (showingLoading) requestUpdate();
}

void HomeActivity::loopDesktopRecommended() {
  auto& books = pageBooks();
  const int totalCount = static_cast<int>(books.size());
  const int totalPages =
      std::max(1, (totalCount + DesktopHomeUi::BOOK_LIST_PAGE_SIZE - 1) / DesktopHomeUi::BOOK_LIST_PAGE_SIZE);
  desktopBookListPage = std::clamp(desktopBookListPage, 0, totalPages - 1);
  const int pageCount = std::clamp(totalCount - desktopBookListPage * DesktopHomeUi::BOOK_LIST_PAGE_SIZE, 0,
                                   DesktopHomeUi::BOOK_LIST_PAGE_SIZE);
  desktopBookListSelected = std::clamp(desktopBookListSelected, 0, std::max(0, pageCount - 1));

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight;
  const Rect content = UITheme::getContentArea(renderer);
  const Rect area{content.x, top, content.width, content.y + content.height - metrics.buttonHintsHeight - top};
  const auto contains = [](const Rect& r, const int x, const int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
  };
  const auto close = [this] {
    desktopListPage = DesktopListPage::None;
    requestUpdate();
  };
  const auto turnPage = [this, totalPages](const int delta) {
    desktopBookListPage = (desktopBookListPage + delta + totalPages) % totalPages;
    desktopBookListSelected = 0;
    requestUpdate();
  };
  const auto bookAt = [this, &books](const int row) -> const RecentBook& {
    return books[desktopBookListPage * DesktopHomeUi::BOOK_LIST_PAGE_SIZE + row].book;
  };

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenLongPress(x, y)) {
    for (int i = 0; i < pageCount; ++i) {
      const Rect row = DesktopHomeUi::bookListRowRect(area, i);
      if (contains(row, x, y)) {
        desktopBookListSelected = i;
        openBookActionsFor(bookAt(i), row);
        return;
      }
    }
  }

  if (mappedInput.wasScreenTapped(x, y)) {
    if (y < top && x < 100) {
      haptic_feedback::touchAction();
      close();
      return;
    }
    if (totalPages > 1) {
      if (contains(DesktopHomeUi::bookListPrevPageRect(area), x, y)) {
        haptic_feedback::touchAction();
        turnPage(-1);
        return;
      }
      if (contains(DesktopHomeUi::bookListNextPageRect(area), x, y)) {
        haptic_feedback::touchAction();
        turnPage(1);
        return;
      }
    }
    for (int i = 0; i < pageCount; ++i) {
      if (contains(DesktopHomeUi::bookListRowRect(area, i), x, y)) {
        haptic_feedback::touchAction();
        desktopBookListSelected = i;
        onRecentBookSelected(bookAt(i).path);
        return;
      }
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left && totalPages > 1) {
    turnPage(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right && totalPages > 1) {
    turnPage(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && pageCount > 0) {
    if (mappedInput.getHeldTime() >= DESKTOP_LONG_PRESS_MS) {
      openBookActionsFor(bookAt(desktopBookListSelected),
                         DesktopHomeUi::bookListRowRect(area, desktopBookListSelected));
    } else {
      onRecentBookSelected(bookAt(desktopBookListSelected).path);
    }
    return;
  }

  buttonNavigator.onNext([this, pageCount, totalPages] {
    if (pageCount <= 0) return;
    if (desktopBookListSelected + 1 < pageCount) {
      ++desktopBookListSelected;
    } else if (totalPages > 1) {
      desktopBookListPage = (desktopBookListPage + 1) % totalPages;
      desktopBookListSelected = 0;
    } else {
      desktopBookListSelected = 0;
    }
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, pageCount, totalPages] {
    if (pageCount <= 0) return;
    if (desktopBookListSelected > 0) {
      --desktopBookListSelected;
    } else if (totalPages > 1) {
      desktopBookListPage = (desktopBookListPage + totalPages - 1) % totalPages;
      desktopBookListSelected = 0;
    }
    requestUpdate();
  });
}

void HomeActivity::loopDesktop() {
  if (desktopSheetOpen) {
    loopDesktopSheet();
    return;
  }

  if (desktopBookListOpen) {
    loopDesktopBookList();
    return;
  }

  if (desktopListPage != DesktopListPage::None) {
    loopDesktopRecommended();
    return;
  }

  if (desktopStatsOpen) {
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tapX, tapY)) {
      haptic_feedback::touchAction();
      desktopStatsOpen = false;
      requestUpdate();
    }
    return;
  }

  uint8_t hour = 0;
  uint8_t minute = 0;
  if (SETTINGS.desktopClockLive && halClock.getTime(hour, minute) && minute != desktopMinute) {
    requestUpdate();
  }

  const int count = desktopFocusCount();
  buttonNavigator.onNext([this, count] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = swipe == MappedInputManager::SwipeDir::Up ? ButtonNavigator::nextIndex(selectorIndex, count)
                                                              : ButtonNavigator::previousIndex(selectorIndex, count);
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading) {
    onRecentBookSelected(recentBooks[0].path);
    return;
  }

  if (handleDesktopTouch()) {
    activateDesktopFocus();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    Rect anchor{};
    const RecentBook* book = desktopFocusedBook(&anchor);
    if (book && mappedInput.getHeldTime() >= DESKTOP_LONG_PRESS_MS) {
      openBookActionsFor(*book, anchor);
    } else {
      activateDesktopFocus();
    }
  }
}

const RecentBook* HomeActivity::desktopFocusedBook(Rect* outAnchor) const {
  const auto layout = DesktopHomeUi::layout(renderer);
  const auto focusMap = desktopFocusMap();
  const int shelfCount = focusMap.shelfCount;
  const int recBase = focusMap.recBase;
  const int recCount = focusMap.recCount;

  if (selectorIndex == 0) {
    if (!hasContinueReading) return nullptr;
    if (outAnchor) *outAnchor = layout.hero;
    return &recentBooks[0];
  }
  if (selectorIndex >= 1 && selectorIndex <= shelfCount) {
    const int slot = selectorIndex - 1;
    if (outAnchor) *outAnchor = DesktopHomeUi::recentSlot(layout, slot);
    return desktopFavoritesTab ? &favoriteBooks[slot] : &recentBooks[1 + slot];
  }
  if (selectorIndex >= recBase && selectorIndex < recBase + recCount) {
    const int slot = selectorIndex - recBase;
    if (outAnchor) *outAnchor = DesktopHomeUi::recommendedSlot(layout, slot);
    return &recommendedBooks[slot].book;
  }
  return nullptr;
}

void HomeActivity::activateDesktopFocus() {
  const auto focusMap = desktopFocusMap();
  const int shelfCount = focusMap.shelfCount;
  const int seeAllIdx = focusMap.seeAll;
  const int recBase = focusMap.recBase;
  const int recCount = focusMap.recCount;
  const int dockBase = focusMap.dockBase;

  if (selectorIndex == 0) {
    hasContinueReading ? onRecentBookSelected(recentBooks[0].path) : onLibraryOpen();
    return;
  }
  if (selectorIndex >= 1 && selectorIndex <= shelfCount) {
    const int slot = selectorIndex - 1;
    const RecentBook& book = desktopFavoritesTab ? favoriteBooks[slot] : recentBooks[1 + slot];
    onRecentBookSelected(book.path);
    return;
  }
  if (selectorIndex == seeAllIdx) {
    desktopBookListOpen = true;
    desktopBookListPage = 0;
    desktopBookListSelected = 0;
    requestUpdate();
    return;
  }
  if (selectorIndex == focusMap.recMore) {
    openRecommendedPage();
    return;
  }
  if (selectorIndex >= recBase && selectorIndex < recBase + recCount) {
    onRecentBookSelected(recommendedBooks[selectorIndex - recBase].book.path);
    return;
  }
  switch (selectorIndex - dockBase) {
    case 0:
      onLibraryOpen();
      break;
    case 1:
      desktopStatsOpen = true;
      requestUpdate();
      break;
    case 2:
      openVocab();
      break;
    default:
      openMoreMenu();
      break;
  }
}

DesktopHomeUi::SheetItem HomeActivity::sheetItemFor(const DesktopAction action) const {
  using DesktopHomeUi::SheetGlyph;
  switch (action) {
    case DesktopAction::FileBrowser:
      return {tr(STR_BROWSE_FILES), SheetGlyph::Folder};
    case DesktopAction::FileTransfer:
      return {tr(STR_FILE_TRANSFER), SheetGlyph::Transfer};
    case DesktopAction::LibrarySlot:
      return {hasPlugins ? tr(STR_PLUGINS) : tr(STR_OPDS_BROWSER), SheetGlyph::Plugins};
    case DesktopAction::ReadingStats:
      return {tr(STR_READING_STATS), SheetGlyph::Stats};
    case DesktopAction::ControlCenter:
      return {tr(STR_DESKTOP_LIGHT), SheetGlyph::Light};
    case DesktopAction::RecentBooksPage:
      return {tr(STR_RECENT_BOOKS_TITLE), SheetGlyph::Details};
    case DesktopAction::FavoriteBooksPage:
      return {tr(STR_FAVORITE_BOOKS_TITLE), SheetGlyph::Heart};
    case DesktopAction::ToggleFavorite:
      return {RECENT_BOOKS.isFavorite(desktopTargetBook.path) ? tr(STR_REMOVE_FAVORITE) : tr(STR_ADD_FAVORITE),
              SheetGlyph::Heart};
    case DesktopAction::BookDetails:
      return {tr(STR_DETAILS), SheetGlyph::Details};
    case DesktopAction::NotInterested:
      return {tr(STR_RECOMMEND_NOT_INTERESTED), SheetGlyph::Remove};
    case DesktopAction::UnfinishedBooksPage:
      return {tr(STR_UNFINISHED_TITLE), SheetGlyph::Stats};
    case DesktopAction::ContinueReading:
      return {tr(STR_DESKTOP_CONTINUE), SheetGlyph::Details};
    case DesktopAction::DropBook:
      return {tr(STR_UNFINISHED_DROP), SheetGlyph::Remove};
    case DesktopAction::ReceiveBooks:
      return {tr(STR_INBOX_TITLE), SheetGlyph::Transfer};
    case DesktopAction::OpenDictionary:
      return {tr(STR_DICTIONARY), SheetGlyph::Details};
    case DesktopAction::OpenSettings:
      return {tr(STR_SETTINGS_TITLE), SheetGlyph::Settings};
    case DesktopAction::RemoveRecent:
    case DesktopAction::None:
      break;
  }
  return {tr(STR_DESKTOP_REMOVE_RECENT), SheetGlyph::Remove};
}

void HomeActivity::showDesktopMenu(const char* title) {
  desktopSheetTitle = title;
  desktopSheetIndex = 0;
  desktopSheetOpen = true;
  requestUpdate();
}

void HomeActivity::loopDesktopSheet() {
  const int count = static_cast<int>(desktopMenuActions.size());
  const auto close = [this] {
    desktopSheetOpen = false;
    requestUpdate();
  };
  const auto run = [this](const int index) {
    desktopSheetOpen = false;
    requestUpdate();
    if (index >= 0 && index < static_cast<int>(desktopMenuActions.size())) runDesktopAction(desktopMenuActions[index]);
  };

  const auto layout = DesktopHomeUi::layout(renderer);
  const Rect sheet = DesktopHomeUi::sheetRect(layout, count, desktopSheetAnchor);
  const Rect first = DesktopHomeUi::sheetRow(sheet, 0);
  int row = -1;
  const auto touch =
      mappedInput.rowTouch(row, first.y, first.height, count, first.x, first.x + first.width, first.height);
  if (touch == MappedInputManager::RowTouch::Down) {
    if (row != desktopSheetIndex) {
      desktopSheetIndex = row;
      requestUpdate();
    }
    return;
  }
  if (touch == MappedInputManager::RowTouch::Tap) {
    haptic_feedback::touchAction();
    run(row);
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y) &&
      !(x >= sheet.x && x < sheet.x + sheet.width && y >= sheet.y && y < sheet.y + sheet.height)) {
    close();
    return;
  }

  buttonNavigator.onNext([this, count] {
    desktopSheetIndex = ButtonNavigator::nextIndex(desktopSheetIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    desktopSheetIndex = ButtonNavigator::previousIndex(desktopSheetIndex, count);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    run(desktopSheetIndex);
  }
}

void HomeActivity::openMoreMenu() {
  desktopMenuActions = {DesktopAction::FavoriteBooksPage, DesktopAction::RecentBooksPage, DesktopAction::FileBrowser,
                        DesktopAction::FileTransfer};
  if (hasLibrarySlot()) desktopMenuActions.push_back(DesktopAction::LibrarySlot);
  desktopMenuActions.push_back(DesktopAction::UnfinishedBooksPage);
  if (InboxActivity::available()) desktopMenuActions.push_back(DesktopAction::ReceiveBooks);
  desktopMenuActions.push_back(DesktopAction::OpenDictionary);
  // Settings left the dock for the vocabulary trainer; it closes the list.
  desktopMenuActions.push_back(DesktopAction::OpenSettings);
  desktopSheetAnchor = DesktopHomeUi::dockSlot(DesktopHomeUi::layout(renderer), DesktopHomeUi::DOCK_COUNT - 1);
  showDesktopMenu(tr(STR_DESKTOP_MORE));
}

void HomeActivity::openBookActionsFor(const RecentBook& book, const Rect& anchor) {
  desktopTargetBook = book;
  // On the Unfinished page the choice that matters is read on or let go.
  if (desktopListPage == DesktopListPage::Unfinished) {
    desktopMenuActions = {DesktopAction::ContinueReading, DesktopAction::DropBook, DesktopAction::ToggleFavorite,
                          DesktopAction::BookDetails};
    desktopSheetAnchor = anchor;
    haptic_feedback::touchAction();
    showDesktopMenu(book.title.c_str());
    return;
  }
  desktopMenuActions = {DesktopAction::ToggleFavorite, DesktopAction::BookDetails};
  const auto& recents = RECENT_BOOKS.getBooks();
  if (std::any_of(recents.begin(), recents.end(), [&](const RecentBook& b) { return b.path == book.path; })) {
    desktopMenuActions.push_back(DesktopAction::RemoveRecent);
  }
  if (std::any_of(recommendedBooks.begin(), recommendedBooks.end(),
                  [&](const DesktopHomeUi::RecommendedBook& r) { return r.book.path == book.path; })) {
    desktopMenuActions.push_back(DesktopAction::NotInterested);
  }
  desktopSheetAnchor = anchor;
  haptic_feedback::touchAction();
  showDesktopMenu(book.title.c_str());
}

void HomeActivity::runDesktopAction(const DesktopAction action) {
  switch (action) {
    case DesktopAction::FileBrowser:
      onFileBrowserOpen();
      break;
    case DesktopAction::FileTransfer:
      onFileTransferOpen();
      break;
    case DesktopAction::LibrarySlot:
      hasPlugins ? onPluginsOpen() : onOpdsBrowserOpen();
      break;
    case DesktopAction::ReadingStats:
      desktopStatsOpen = true;
      requestUpdate();
      break;
    case DesktopAction::ControlCenter:
      activityManager.goToControlCenter();
      break;
    case DesktopAction::RecentBooksPage:
      desktopFavoritesTab = false;
      desktopBookListOpen = true;
      desktopBookListPage = 0;
      desktopBookListSelected = 0;
      requestUpdate();
      break;
    case DesktopAction::FavoriteBooksPage:
      desktopFavoritesTab = true;
      desktopBookListOpen = true;
      desktopBookListPage = 0;
      desktopBookListSelected = 0;
      requestUpdate();
      break;
    case DesktopAction::ToggleFavorite:
      if (!desktopTargetBook.path.empty()) {
        RECENT_BOOKS.toggleFavorite(desktopTargetBook);
        reloadDesktopBooks();
        selectorIndex = std::min(selectorIndex, desktopFocusCount() - 1);
        requestUpdate();
      }
      break;
    case DesktopAction::BookDetails:
      if (!desktopTargetBook.path.empty()) openBookDetails(desktopTargetBook);
      break;
    case DesktopAction::RemoveRecent:
      if (!desktopTargetBook.path.empty()) {
        RECENT_BOOKS.removeByPath(desktopTargetBook.path);
        reloadDesktopBooks();
        selectorIndex = std::min(selectorIndex, desktopFocusCount() - 1);
        requestUpdate();
      }
      break;
    case DesktopAction::NotInterested:
      if (!desktopTargetBook.path.empty()) {
        hidden_recommendations::hide(desktopTargetBook.path, desktopTargetBook.title, desktopTargetBook.author);
        // Drop it in place rather than rescanning: the next candidate moves up
        // and the scan's cost is not paid again for one dismissal.
        const std::string path = desktopTargetBook.path;
        const auto dismissed = [&](const DesktopHomeUi::RecommendedBook& r) { return r.book.path == path; };
        recommendedBooks.erase(std::remove_if(recommendedBooks.begin(), recommendedBooks.end(), dismissed),
                               recommendedBooks.end());
        // Books shift up a slot, so the Home cards and the open page may now
        // show covers that were never loaded.
        recentsLoaded = false;
        recommendedCoversPage = -1;
        selectorIndex = std::min(selectorIndex, desktopFocusCount() - 1);
        requestUpdate();
      }
      break;
    case DesktopAction::UnfinishedBooksPage:
      openUnfinishedPage();
      break;
    case DesktopAction::OpenDictionary: {
      auto dictionary = makeUniqueNoThrow<DictionaryActivity>(renderer, mappedInput);
      if (!dictionary) {
        LOG_ERR("HOME", "OOM: dictionary");
        break;
      }
      startActivityForResult(std::move(dictionary), [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case DesktopAction::OpenSettings:
      onSettingsOpen();
      break;
    case DesktopAction::ReceiveBooks: {
      auto inbox = makeUniqueNoThrow<InboxActivity>(renderer, mappedInput);
      if (!inbox) {
        LOG_ERR("HOME", "OOM: inbox");
        break;
      }
      startActivityForResult(std::move(inbox), [this](const ActivityResult&) {
        reloadDesktopBooks();
        requestUpdate();
      });
      break;
    }
    case DesktopAction::ContinueReading:
      if (!desktopTargetBook.path.empty()) onRecentBookSelected(desktopTargetBook.path);
      break;
    case DesktopAction::DropBook:
      if (!desktopTargetBook.path.empty()) {
        // Letting a book go takes it off the recents (so off this page) and out
        // of the recommendations, where it would otherwise surface straight
        // back; Settings -> hidden books gives it back.
        RECENT_BOOKS.removeByPath(desktopTargetBook.path);
        hidden_recommendations::hide(desktopTargetBook.path, desktopTargetBook.title, desktopTargetBook.author);
        const std::string path = desktopTargetBook.path;
        const auto dropped = [&](const DesktopHomeUi::RecommendedBook& r) { return r.book.path == path; };
        unfinishedBooks.erase(std::remove_if(unfinishedBooks.begin(), unfinishedBooks.end(), dropped),
                              unfinishedBooks.end());
        reloadDesktopBooks();
        selectorIndex = std::min(selectorIndex, desktopFocusCount() - 1);
        requestUpdate();
      }
      break;
    case DesktopAction::None:
      break;
  }
}

void HomeActivity::openBookDetails(const RecentBook& book) {
  DetailsSpec spec;
  spec.header = tr(STR_DETAILS);
  spec.title = book.title;
  spec.author = book.author;
  spec.source = book.path;
  const auto info = (!recentBooks.empty() && book.path == recentBooks[0].path) ? desktopBook
                                                                               : DesktopHomeUi::readBookInfo(book);
  if (info.percent >= 0) {
    spec.metadata = std::to_string(info.percent) + "%";
    if (!info.chapter.empty()) spec.metadata += " · " + info.chapter;
  }
  spec.showCover = true;
  if (!book.coverBmpPath.empty()) {
    const std::string thumb = UITheme::getCoverThumbPath(book.coverBmpPath, DesktopHomeUi::COVER_HEIGHT);
    if (Storage.exists(thumb.c_str())) spec.coverPath = thumb;
  }
  spec.actionLabel = tr(STR_DESKTOP_CONTINUE);
  auto details = makeUniqueNoThrow<DetailsActivity>(renderer, mappedInput, std::move(spec));
  if (!details) {
    LOG_ERR("HOME", "OOM: book details");
    return;
  }
  const std::string path = book.path;
  startActivityForResult(std::move(details), [this, path](const ActivityResult& result) {
    if (!result.isCancelled) onRecentBookSelected(path);
  });
}

bool HomeActivity::handleDesktopTouch() {
  const auto layout = DesktopHomeUi::layout(renderer);
  const auto focusMap = desktopFocusMap();
  const int shelfCount = focusMap.shelfCount;
  const int seeAllIdx = focusMap.seeAll;
  const int recBase = focusMap.recBase;
  const int recCount = focusMap.recCount;
  const int dockBase = focusMap.dockBase;

  const auto contains = [](const Rect& r, const int x, const int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
  };

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenLongPress(x, y)) {
    if (hasContinueReading && contains(layout.hero, x, y)) {
      selectorIndex = 0;
      openBookActionsFor(recentBooks[0], layout.hero);
      return false;
    }
    for (int i = 0; i < shelfCount; ++i) {
      const Rect slot = DesktopHomeUi::recentSlot(layout, i);
      if (contains(slot, x, y)) {
        selectorIndex = 1 + i;
        openBookActionsFor(desktopFavoritesTab ? favoriteBooks[i] : recentBooks[1 + i], slot);
        return false;
      }
    }
    for (int i = 0; i < recCount; ++i) {
      const Rect slot = DesktopHomeUi::recommendedSlot(layout, i);
      if (contains(slot, x, y)) {
        selectorIndex = recBase + i;
        openBookActionsFor(recommendedBooks[i].book, slot);
        return false;
      }
    }
  }

  // Shelf header tabs (GẦN ĐÂY | ♥ YÊU THÍCH) and "Xem tất cả >"
  if (mappedInput.wasScreenTapped(x, y)) {
    if (contains(DesktopHomeUi::recentTabRect(layout, false), x, y)) {
      if (desktopFavoritesTab) {
        haptic_feedback::touchAction();
        desktopFavoritesTab = false;
        selectorIndex = 0;
        requestUpdate();
      }
      return false;
    }
    if (contains(DesktopHomeUi::recentTabRect(layout, true), x, y)) {
      if (!desktopFavoritesTab) {
        haptic_feedback::touchAction();
        desktopFavoritesTab = true;
        selectorIndex = 0;
        requestUpdate();
      }
      return false;
    }
    if (contains(DesktopHomeUi::seeAllRect(layout), x, y)) {
      haptic_feedback::touchAction();
      selectorIndex = seeAllIdx;
      return true;
    }
    if (contains(DesktopHomeUi::recommendedSeeMoreRect(layout), x, y)) {
      haptic_feedback::touchAction();
      selectorIndex = focusMap.recMore;
      return true;
    }
  }

  const auto apply = [this](const MappedInputManager::RowTouch touch, const int index) {
    if (touch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != index) {
        selectorIndex = index;
        requestUpdate();
      }
      return false;
    }
    selectorIndex = index;
    haptic_feedback::touchAction();
    return true;
  };

  int col = -1;
  const Rect& hero = layout.hero;
  auto touch = mappedInput.colTouch(col, hero.x, hero.width, 1, hero.y, hero.y + hero.height);
  if (touch != MappedInputManager::RowTouch::None) return apply(touch, 0);

  if (shelfCount > 0) {
    const Rect first = DesktopHomeUi::recentSlot(layout, 0);
    touch = mappedInput.colTouch(col, layout.recents.x, layout.recents.width / DesktopHomeUi::MAX_RECENTS, shelfCount,
                                 first.y, first.y + first.height);
    if (touch != MappedInputManager::RowTouch::None) return apply(touch, 1 + col);
  }

  if (recCount > 0) {
    const Rect first = DesktopHomeUi::recommendedSlot(layout, 0);
    touch = mappedInput.colTouch(col, layout.recommended.x, layout.recommended.width / DesktopHomeUi::MAX_RECOMMENDED,
                                 recCount, first.y, first.y + first.height);
    if (touch != MappedInputManager::RowTouch::None) return apply(touch, recBase + col);
  }

  const Rect& dock = layout.dock;
  touch = mappedInput.colTouch(col, dock.x, dock.width / DesktopHomeUi::DOCK_COUNT, DesktopHomeUi::DOCK_COUNT, dock.y,
                               dock.y + dock.height);
  if (touch != MappedInputManager::RowTouch::None) return apply(touch, dockBase + col);
  return false;
}

void HomeActivity::renderDesktop() {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (desktopBookListOpen) {
    const int top = metrics.topPadding + metrics.headerHeight;
    const Rect content = UITheme::getContentArea(renderer);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   desktopFavoritesTab ? tr(STR_FAVORITE_BOOKS_TITLE) : tr(STR_RECENT_BOOKS_TITLE));
    const auto& list = desktopFavoritesTab ? favoriteBooks : recentBooks;
    DesktopHomeUi::drawBookListPage(
        renderer, Rect{content.x, top, content.width, content.y + content.height - metrics.buttonHintsHeight - top},
        list.empty() ? nullptr : list.data(), static_cast<int>(list.size()), desktopBookListPage,
        desktopBookListSelected, desktopFavoritesTab, static_cast<int>(recentBooks.size()),
        static_cast<int>(favoriteBooks.size()));

    if (desktopSheetOpen) {
      const auto layout = DesktopHomeUi::layout(renderer);
      const int count = static_cast<int>(desktopMenuActions.size());
      std::vector<DesktopHomeUi::SheetItem> items;
      items.reserve(desktopMenuActions.size());
      for (const DesktopAction action : desktopMenuActions) items.push_back(sheetItemFor(action));
      DesktopHomeUi::drawSheet(renderer, DesktopHomeUi::sheetRect(layout, count, desktopSheetAnchor),
                               desktopSheetAnchor, desktopSheetTitle.c_str(), items.data(), count, desktopSheetIndex);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  if (desktopListPage != DesktopListPage::None) {
    const bool unfinished = desktopListPage == DesktopListPage::Unfinished;
    const auto& books = pageBooks();
    const int top = metrics.topPadding + metrics.headerHeight;
    const Rect content = UITheme::getContentArea(renderer);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   unfinished ? tr(STR_UNFINISHED_TITLE) : tr(STR_RECOMMENDED_TITLE));
    DesktopHomeUi::drawRecommendedPage(
        renderer, Rect{content.x, top, content.width, content.y + content.height - metrics.buttonHintsHeight - top},
        books.empty() ? nullptr : books.data(), static_cast<int>(books.size()), desktopBookListPage,
        desktopBookListSelected, unfinished ? tr(STR_UNFINISHED_PAGE_HINT) : tr(STR_RECOMMENDED_PAGE_HINT),
        unfinished ? tr(STR_UNFINISHED_EMPTY) : tr(STR_DESKTOP_RECOMMENDED_EMPTY));

    if (desktopSheetOpen) {
      const auto layout = DesktopHomeUi::layout(renderer);
      const int count = static_cast<int>(desktopMenuActions.size());
      std::vector<DesktopHomeUi::SheetItem> items;
      items.reserve(desktopMenuActions.size());
      for (const DesktopAction action : desktopMenuActions) items.push_back(sheetItemFor(action));
      DesktopHomeUi::drawSheet(renderer, DesktopHomeUi::sheetRect(layout, count, desktopSheetAnchor),
                               desktopSheetAnchor, desktopSheetTitle.c_str(), items.data(), count, desktopSheetIndex);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    // After the frame, like the Home covers: the page shows at once and the
    // thumbnails fill in on the next redraw.
    if (!desktopSheetOpen && !renderer.inUiGrayPass()) loadRecommendedPageCovers();
    return;
  }

  if (desktopStatsOpen) {
    const int top = metrics.topPadding + metrics.headerHeight;
    const Rect content = UITheme::getContentArea(renderer);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   tr(STR_READING_STATS));
    DesktopHomeUi::drawStatsPage(
        renderer, Rect{content.x, top, content.width, content.y + content.height - metrics.buttonHintsHeight - top});
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  uint8_t hour = 0;
  uint8_t minute = 0;
  if (halClock.getTime(hour, minute)) desktopMinute = minute;

  const auto layout = DesktopHomeUi::layout(renderer);
  BaseTheme::headerStatusOverride = SETTINGS.deviceName[0] ? SETTINGS.deviceName : tr(STR_DEVICE_NAME_DEFAULT);
  BaseTheme::headerBatteryLabelOverride = DesktopHomeUi::batteryLabel();
  GUI.drawHeader(renderer, layout.header, nullptr, nullptr, false);
  BaseTheme::headerStatusOverride = nullptr;
  BaseTheme::headerBatteryLabelOverride = nullptr;

  const auto focusMap = desktopFocusMap();
  const int shelfCount = focusMap.shelfCount;
  const int seeAllIdx = focusMap.seeAll;
  const int recBase = focusMap.recBase;
  const int recCount = focusMap.recCount;
  const int dockBase = focusMap.dockBase;
  const int focus = selectorIndex;

  DesktopHomeUi::drawClock(renderer, layout.clock);
  DesktopHomeUi::drawHero(renderer, layout.hero, hasContinueReading ? &recentBooks[0] : nullptr, desktopBook,
                          focus == 0);
  const RecentBook* shelfBooks =
      shelfCount > 0 ? (desktopFavoritesTab ? favoriteBooks.data() : &recentBooks[1]) : nullptr;
  DesktopHomeUi::drawRecents(renderer, layout, shelfBooks, shelfCount,
                             focus >= 1 && focus <= shelfCount ? focus - 1 : -1, desktopFavoritesTab,
                             focus == seeAllIdx);
  DesktopHomeUi::drawRecommended(renderer, layout, recCount > 0 ? recommendedBooks.data() : nullptr, recCount,
                                 focus >= recBase && focus < recBase + recCount ? focus - recBase : -1,
                                 focus == focusMap.recMore);

  using DesktopHomeUi::DockGlyph;
  const DesktopHomeUi::DockItem dock[DesktopHomeUi::DOCK_COUNT] = {{tr(STR_LIBRARY), DockGlyph::Library},
                                                                  {tr(STR_READING_STATS), DockGlyph::Stats},
                                                                  {tr(STR_VOCAB_TITLE), DockGlyph::Vocab},
                                                                  {tr(STR_DESKTOP_MORE), DockGlyph::More}};
  DesktopHomeUi::drawDock(renderer, layout, dock, focus - dockBase);

  if (desktopSheetOpen) {
    const int count = static_cast<int>(desktopMenuActions.size());
    std::vector<DesktopHomeUi::SheetItem> items;
    items.reserve(desktopMenuActions.size());
    for (const DesktopAction action : desktopMenuActions) items.push_back(sheetItemFor(action));
    DesktopHomeUi::drawSheet(renderer, DesktopHomeUi::sheetRect(layout, count, desktopSheetAnchor),
                             desktopSheetAnchor, desktopSheetTitle.c_str(), items.data(), count, desktopSheetIndex);
  }

  const auto labels =
      desktopSheetOpen
          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
          : mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT), tr(STR_DIR_UP),
                                  tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  if (renderer.inUiGrayPass()) return;

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    loadDesktopCovers();
  }
}

void HomeActivity::render(RenderLock&&) {
  if (desktopHome) {
    renderDesktop();
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect content = UITheme::getInstance().getContentArea(renderer);

  renderer.clearScreen();
  if (coverGridUi) {
    coverGridUi->setSelection(selectorIndex);
    UITheme::getInstance().drawCoverGridHome(*coverGridUi);
    const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    if (renderer.inUiGrayPass()) return;
    const bool coverSpecChanged = coverGridUi->takeThumbHeightChanged();
    if (coverSpecChanged) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      requestUpdate();
    }
    return;
  }
  bool bufferRestored = coverRendered && coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                 nullptr, false);

  coverRectX = content.x;
  coverRectY = metrics.homeTopPadding;
  coverRectW = content.width;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{content.x, metrics.homeTopPadding, content.width, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_LIBRARY), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Library, Transfer, Settings};

  if (hasLibrarySlot()) {
    menuItems.insert(menuItems.begin() + 2, hasPlugins ? tr(STR_PLUGINS) : tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Plugins);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{content.x, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, content.width,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  if (renderer.inUiGrayPass()) return;

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
}

void HomeActivity::onRecentBookSelected(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(hasOpdsServers); }
