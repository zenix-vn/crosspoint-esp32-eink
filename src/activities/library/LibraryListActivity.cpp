#include "LibraryListActivity.h"

#include <BoardConfig.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "CollectionsStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/headerIcons.h"
#include "components/icons/libraryTabIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;

// Side clearance for the footer text in the panel's bottom corners.
int bottomCornerClearance() {
  const auto& board = BoardConfig::ACTIVE;
  return board.bottomCornerRadius != 0 ? board.bottomCornerRadius : board.cornerRadius;
}

constexpr unsigned long LONG_PRESS_MS = 1000;

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
constexpr int FOLDER_TAB = 3;
constexpr int GENRE_TAB = 4;
constexpr int SERIES_TAB = 5;
constexpr int COLLECTION_TAB = 6;
constexpr int TAB_SLOTS = COLLECTION_TAB + 1;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  // Folder, genre, series and collection scopes list their books by title
  // (a series scope then re-sorts its few volumes by position).
  if (tab == TITLE_TAB || tab == FOLDER_TAB || tab == GENRE_TAB || tab == SERIES_TAB || tab == COLLECTION_TAB) {
    return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  }
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab) {
  if (tab == TITLE_TAB) return tr(STR_LIBRARY_TAB_TITLE);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  if (tab == FOLDER_TAB) return tr(STR_LIBRARY_TAB_FOLDERS);
  if (tab == GENRE_TAB) return tr(STR_LIBRARY_TAB_GENRES);
  if (tab == SERIES_TAB) return tr(STR_LIBRARY_TAB_SERIES);
  if (tab == COLLECTION_TAB) return tr(STR_LIBRARY_TAB_COLLECTIONS);
  return tr(STR_LIBRARY_TAB_RECENT);
}

// "/Books/Novels/x.epub" -> "/Books/Novels"; books at the top level -> "/".
std::string parentFolder(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return path.substr(0, slash);
}

std::string folderLabel(const std::string& folder) {
  return folder == "/" ? std::string(tr(STR_LIBRARY_ROOT_FOLDER)) : folder.substr(1);
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {
  // Seven tab labels share the strip: keep each pill tight around its label.
  tabPillMaxPad = 8;
}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Rebuild when the index is missing, invalid, or was built with the other
  // metadata mode. Otherwise entering the screen stays instant.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool rebuildNeeded = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                             index.header().metadataEnabled != readMetadata;
  if (rebuildNeeded) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    rebuildIndex();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  resolvePinned();
  headerTabTitle = std::string(tr(STR_LIBRARY)) + " · " + tabLabelFor(activeTabIndex);

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  Activity::onExit();
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (activeTabIndex != RECENT_TAB || filterActive() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  pinnedTotal = static_cast<uint8_t>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (selectedEntry() < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (selectedEntry() >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(selectedEntry())].path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedEntry())));
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
      return;
    }
  }
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (pickerShown()) {
    choosePicker(index);
  } else if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent shelf always opens the row options menu.
bool LibraryListActivity::deleteEligible() const { return !groupsCollapsed && (filterActive() || !groupable()); }

void LibraryListActivity::onRowLongPress(const int index) {
  if (pickerShown()) {
    if (activeTabIndex == COLLECTION_TAB) showCollectionOptions(index);
  } else if (isRecentSort(sortOrder)) {
    showRecentBookOptions(index);
  } else if (deleteEligible()) {
    showBookOptions(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(entry)].path;
    title = books[static_cast<size_t>(entry)].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_COLLECTION_ADD), tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE),
                                 tr(STR_LIBRARY_REBUILD)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_COLLECTION_ADD), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 5 : 4, 0,
                   [this, path, title, isStoreRow](int choice) {
                     swallowHeldReleases();
                     if (choice == 1) {
                       showAddToCollection(path);
                       return;
                     }
                     if (choice > 1) choice--;  // the rest keep their original order
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         if (isStoreRow) {
                           promptRemoveRecentBook(path, title);
                         } else {
                           promptDeleteBookByPath(path, title);
                         }
                         break;
                       case 2:
                         if (isStoreRow)
                           promptDeleteBookByPath(path, title);
                         else
                           promptRebuildIndex();
                         break;
                       case 3:
                         if (isStoreRow) promptRebuildIndex();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

#if FREEINK_CAP_TOUCH
void LibraryListActivity::swipeDelete(const int entry) {
  activeNav().selected = entry + 1;
  if (isRecentSort(sortOrder) && entry < pinnedCount()) {
    const auto& book = RECENT_BOOKS.getBooks()[static_cast<size_t>(entry)];
    promptDeleteBookByPath(book.path, book.title);
  } else {
    promptDeleteBook(entry);
  }
}
#endif

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  index.close();
  rebuildIndex();
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  resetAfterRebuild();
  requestUpdate(true);
}

void LibraryListActivity::resetAfterRebuild() {
  // Sort positions, group starts, pinned rows and folder counts all describe the old index.
  if (activeTabIndex == FOLDER_TAB) {
    loadFolders();
  } else {
    foldersLoaded = false;
  }
  if (activeTabIndex == GENRE_TAB) {
    loadGenres();
  } else {
    genresLoaded = false;
  }
  if (activeTabIndex == SERIES_TAB) {
    loadSeries();
  } else {
    seriesLoaded = false;
  }
  applyFilter();
  resolvePinned();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        if (!removeBookFile(path)) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && filteredCount == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
      nav.selected = 0;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on the strip.
      nav.selected = 1;
    }
    nav.top = 0;
    requestUpdate();
  });
}

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
#if FREEINK_CAP_TOUCH
  closeSwipeDelete();
#endif
  if (index < 0 || index >= TAB_SLOTS) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  // A scope belongs to its tab; switching tabs starts from that tab's picker.
  scopeFolder.clear();
  scopeGenre.clear();
  scopeSeries.clear();
  scopeCollection = -1;
  if (index == FOLDER_TAB && !foldersLoaded) loadFolders();
  if (index == GENRE_TAB && !genresLoaded) loadGenres();
  if (index == SERIES_TAB && !seriesLoaded) loadSeries();
  sortOrder = orderForTab(index, descendingTabs);
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  activeTabIndex = index;
  headerTabTitle = std::string(tr(STR_LIBRARY)) + " · " + tabLabelFor(index);
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

fui::BitmapRef LibraryListActivity::tabIcon(const int index) const {
  switch (index) {
    case RECENT_TAB:
      return fui::bitmapFromIcon(icon_clock_24);
    case TITLE_TAB:
      return fui::bitmapFromIcon(icon_book_24);
    case AUTHOR_TAB:
      return fui::bitmapFromIcon(icon_user_24);
    case FOLDER_TAB:
      return fui::bitmapFromIcon(icon_folder_24);
    case GENRE_TAB:
      return fui::bitmapFromIcon(icon_bookmark_24);
    case SERIES_TAB:
      return fui::bitmapFromIcon(icon_blocks_24);
    default:
      return fui::bitmapFromIcon(icon_library_24);
  }
}

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab()) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (filterActive()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const {
  if (pickerShown()) return pickerCount();
  return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount();
}

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (filterActive()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

bool LibraryListActivity::groupable() const { return !degraded && !isRecentSort(sortOrder) && bookRowCount() > 0; }

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header names what narrowed the list (folder, collection, query) in
  // place of the screen title, so the reader can see it without reopening it.
  const BookCollection* collection =
      COLLECTIONS.valid(scopeCollection) ? &COLLECTIONS.all()[static_cast<size_t>(scopeCollection)] : nullptr;
  headerSearchTitle.clear();
  if (!scopeFolder.empty()) headerSearchTitle = folderLabel(scopeFolder);
  if (!scopeGenre.empty()) headerSearchTitle = scopeGenreShown;
  if (!scopeSeries.empty()) headerSearchTitle = scopeSeriesShown;
  if (collection) headerSearchTitle = collection->name;
  if (!query.empty()) headerSearchTitle += (headerSearchTitle.empty() ? "“" : " · “") + query + "”";
  if (!filterActive()) return;

  const std::string needle = query.empty() ? std::string() : library::fold(query);
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  std::string path;
  std::string subjects;
  std::string series;
  // Volume positions of the matched rows, parallel to `matches`, so a series
  // scope can be put in reading order once the pass is done.
  std::unique_ptr<float[]> positions;
  if (!scopeSeries.empty()) {
    positions = makeUniqueNoThrow<float[]>(static_cast<size_t>(total));
    if (!positions) {
      filterFailed = true;
      return;
    }
  }
  const bool scoped = !scopeFolder.empty() || scopeCollection >= 0;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (!scopeGenre.empty()) {
      if (!index.readSubjects(record, subjects)) continue;
      bool inGenre = false;
      library::forEachSubjectTerm(subjects, [&](const std::string& term, std::string_view) {
        inGenre = inGenre || term == scopeGenre;
      });
      if (!inGenre) continue;
    }
    float position = -1.0f;
    if (!scopeSeries.empty()) {
      if (!index.readSeries(record, series, position) || library::fold(series) != scopeSeries) continue;
      positions[matchCount] = position;
    }
    if (scoped) {
      if (!index.readPath(record, path)) continue;
      if (!scopeFolder.empty() && parentFolder(path) != scopeFolder) continue;
      if (scopeCollection >= 0 &&
          (!collection || !std::binary_search(collection->paths.begin(), collection->paths.end(), path))) {
        continue;
      }
    }
    if (needle.empty()) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  if (positions && matchCount > 1) {
    // Insertion sort: a series is a handful of volumes. Unnumbered ones go last,
    // keeping their title order.
    const auto key = [](const float p) { return p < 0.0f ? 1e9f : p; };
    for (uint16_t i = 1; i < matchCount; i++) {
      const uint16_t row = matches[i];
      const float pos = positions[i];
      uint16_t j = i;
      while (j > 0 && key(positions[j - 1]) > key(pos)) {
        matches[j] = matches[j - 1];
        positions[j] = positions[j - 1];
        j--;
      }
      matches[j] = row;
      positions[j] = pos;
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (!scopeFolder.empty() || !scopeGenre.empty() || !scopeSeries.empty() || scopeCollection >= 0) {
    clearScope();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (!tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptRebuildIndex();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    const auto& book = books[static_cast<size_t>(entry)];
    title = book.title;
    author = book.author;
    if (fileName) *fileName = book.path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
    // Inside a series: lead with the volume number and say which are read.
    if (!scopeSeries.empty()) {
      std::string series;
      float position = -1.0f;
      if (index.readSeries(record, series, position) && position >= 0.0f) {
        char number[16];
        snprintf(number, sizeof(number), "%g. ", static_cast<double>(position));
        title.insert(0, number);
      }
      std::string path;
      if (index.readPath(record, path) && READING_STATS.wasFinished(path)) {
        const std::string read = tr(STR_LIBRARY_READ);
        author = author.empty() ? read : read + " · " + author;
      }
    }
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (pickerShown()) {
      if (activeTabIndex == COLLECTION_TAB) showCollectionOptions(selectedEntry());
    } else if (isRecentSort(sortOrder)) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) showBookOptions(selectedEntry());
    } else if (!groupsCollapsed && groupable()) {
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  const bool pageForward = mappedInput.wasPressed(MappedInputManager::Button::PageForward);
  const bool pageBack = mappedInput.wasPressed(MappedInputManager::Button::PageBack);
  if (pageForward || pageBack) {
    sidePaging = true;
    if (count > 0) {
      if (tabsFocused()) {
        if (pageForward) moveRingTo(1);
      } else {
        const int rows = std::max(1, activeNav().pageRows());
        moveRingTo((pageForward ? ButtonNavigator::nextPageIndex(selectedEntry(), count, rows)
                                : ButtonNavigator::previousPageIndex(selectedEntry(), count, rows)) +
                   1);
      }
    }
    return;
  }
  // Swallow the rest of a side press (hold and release) so it never also
  // steps a row or opens search through the shared Next/Previous mapping.
  if (sidePaging) {
    if (!mappedInput.isPressed(MappedInputManager::Button::PageForward) &&
        !mappedInput.isPressed(MappedInputManager::Button::PageBack)) {
      sidePaging = false;
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = tabsFocused();
  }
  buttonNavigator.onNextPress([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if ((!navigationStartedOnTabs || degraded) && count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // Search is an activation: defer it so holding Previous can still step tabs.
  buttonNavigator.onPreviousRelease([this] {
    if (navigationStartedOnTabs && tabsFocused() && !degraded) openSearch();
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  if (pickerShown()) {
    buildPickerRows(screen);
    return;
  }
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  const bool grouped = !isRecentSort(sortOrder);

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the two-line
  // rows edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  std::string rowFile;
  rowFile.reserve(128);
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else {
      if (!rowTextFor(entry, title, author, &rowFile)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped && !author.empty()) item.subtitle = author.c_str();
    }

    item.label = title.c_str();
    // Group headings stay bare; every book row gets its file-type icon.
    if (!groupsCollapsed && !rowFile.empty()) item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
#if FREEINK_CAP_TOUCH
  configureSwipeDelete(screen, props);
#endif
  screen.list(props);
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
#endif
}

// --- Folders and Collections tabs ---------------------------------------------

bool LibraryListActivity::pickerShown() const {
  if (activeTabIndex == FOLDER_TAB) return scopeFolder.empty();
  if (activeTabIndex == GENRE_TAB) return scopeGenre.empty();
  if (activeTabIndex == SERIES_TAB) return scopeSeries.empty();
  if (activeTabIndex == COLLECTION_TAB) return scopeCollection < 0;
  return false;
}

int LibraryListActivity::pickerCount() const {
  if (activeTabIndex == FOLDER_TAB) return static_cast<int>(folders.size());
  if (activeTabIndex == GENRE_TAB) return static_cast<int>(genres.size());
  if (activeTabIndex == SERIES_TAB) return static_cast<int>(seriesList.size());
  return COLLECTIONS.count() + 1;  // the trailing "new collection" row
}

// One pass over the index collecting every book's genre terms, like
// loadFolders(). Subjects are only present when the index was built with
// metadata reading on; without them the picker is simply empty.
void LibraryListActivity::loadGenres() {
  // Bounds the picker's memory on a card full of exotic tags; the most common
  // genres are met early enough in practice to make the cut.
  constexpr size_t MAX_GENRES = 200;
  genres.clear();
  genresLoaded = true;
  if (!index.isOpen()) return;
  const int total = static_cast<int>(index.bookCount());
  if (total > 200) GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  std::string subjects;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentAsc, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readSubjects(record, subjects)) continue;
    library::forEachSubjectTerm(subjects, [&](const std::string& term, const std::string_view shown) {
      const auto it = std::find_if(genres.begin(), genres.end(), [&](const GenreEntry& g) { return g.folded == term; });
      if (it != genres.end()) {
        if (it->books < UINT16_MAX) it->books++;
      } else if (genres.size() < MAX_GENRES) {
        genres.push_back(GenreEntry{term, std::string(shown), 1});
      }
    });
  }
  std::stable_sort(genres.begin(), genres.end(), [](const GenreEntry& a, const GenreEntry& b) {
    return a.books != b.books ? a.books > b.books : a.folded < b.folded;
  });
}

// One pass over the index collecting each book's folder. Paths live in the
// index blob, so this reads every record once; it runs when the Folders tab
// first opens and after a rebuild, never per frame.
void LibraryListActivity::loadFolders() {
  folders.clear();
  foldersLoaded = true;
  if (!index.isOpen()) return;
  const int total = static_cast<int>(index.bookCount());
  if (total > 200) GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  std::string path;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentAsc, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path)) continue;
    const std::string folder = parentFolder(path);
    const auto it = std::lower_bound(folders.begin(), folders.end(), folder,
                                     [](const FolderEntry& entry, const std::string& key) { return entry.path < key; });
    if (it != folders.end() && it->path == folder) {
      if (it->books < UINT16_MAX) it->books++;
    } else {
      folders.insert(it, FolderEntry{folder, 1});
    }
  }
}

// One pass collecting every series name, like loadGenres().
void LibraryListActivity::loadSeries() {
  constexpr size_t MAX_SERIES = 200;
  seriesList.clear();
  seriesLoaded = true;
  if (!index.isOpen()) return;
  const int total = static_cast<int>(index.bookCount());
  if (total > 200) GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  std::string name;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentAsc, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    float position = -1.0f;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readSeries(record, name, position) ||
        name.empty()) {
      continue;
    }
    std::string folded = library::fold(name);
    const auto it = std::find_if(seriesList.begin(), seriesList.end(),
                                 [&](const SeriesEntry& s) { return s.folded == folded; });
    if (it != seriesList.end()) {
      if (it->books < UINT16_MAX) it->books++;
    } else if (seriesList.size() < MAX_SERIES) {
      seriesList.push_back(SeriesEntry{std::move(folded), name, 1});
    }
  }
  std::sort(seriesList.begin(), seriesList.end(),
            [](const SeriesEntry& a, const SeriesEntry& b) { return a.folded < b.folded; });
}

void LibraryListActivity::choosePicker(const int entry) {
  if (entry < 0 || entry >= pickerCount()) return;
  app.clearTapFlash();
  if (activeTabIndex == FOLDER_TAB) {
    scopeFolder = folders[static_cast<size_t>(entry)].path;
  } else if (activeTabIndex == GENRE_TAB) {
    scopeGenre = genres[static_cast<size_t>(entry)].folded;
    scopeGenreShown = genres[static_cast<size_t>(entry)].shown;
  } else if (activeTabIndex == SERIES_TAB) {
    scopeSeries = seriesList[static_cast<size_t>(entry)].folded;
    scopeSeriesShown = seriesList[static_cast<size_t>(entry)].shown;
  } else if (entry == COLLECTIONS.count()) {
    promptNewCollection(std::string());
    return;
  } else {
    scopeCollection = entry;
  }
  pickerReturnEntry = entry;
  applyFilter();
  auto& nav = activeNav();
  nav.selected = bookRowCount() > 0 ? 1 : 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::clearScope() {
  scopeFolder.clear();
  scopeGenre.clear();
  scopeSeries.clear();
  scopeCollection = -1;
  applyFilter();
  auto& nav = activeNav();
  nav.selected = std::min(pickerReturnEntry, pickerCount() - 1) + 1;
  nav.top = 0;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::buildPickerRows(UiScreen& screen) {
  const int count = pickerCount();
  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  props.valueText = screen.theme().bodyText;
  props.valueText.bold = true;
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  auto& nav = activeNav();
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  const int windowStart = static_cast<int>(props.topIndex);
  const auto& collections = COLLECTIONS.all();
  for (int entry = windowStart; entry < count && winItems.size() < cap; entry++) {
    std::string& label = winTitles[winItems.size()];
    std::string& value = winAuthors[winItems.size()];
    fui::ListItem item;
    if (activeTabIndex == FOLDER_TAB) {
      const auto& folder = folders[static_cast<size_t>(entry)];
      label = folderLabel(folder.path);
      value = std::to_string(folder.books);
      item.icon = listIconFor(UIIcon::Folder, 32);
    } else if (activeTabIndex == GENRE_TAB) {
      const auto& genre = genres[static_cast<size_t>(entry)];
      label = genre.shown;
      value = std::to_string(genre.books);
      item.icon = listIconFor(UIIcon::Bookmark, 32);
    } else if (activeTabIndex == SERIES_TAB) {
      const auto& series = seriesList[static_cast<size_t>(entry)];
      label = series.shown;
      value = std::to_string(series.books);
      item.icon = listIconFor(UIIcon::Blocks, 32);
    } else if (entry < static_cast<int>(collections.size())) {
      label = collections[static_cast<size_t>(entry)].name;
      value = std::to_string(collections[static_cast<size_t>(entry)].paths.size());
      item.icon = listIconFor(UIIcon::Library, 32);
    } else {
      label = tr(STR_COLLECTION_NEW);
      value.clear();
    }
    item.label = label.c_str();
    item.value = value.empty() ? nullptr : value.c_str();
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
  }
  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
}

// Path and display title of a book row (store rows first on the Recent shelf).
bool LibraryListActivity::resolveEntry(const int entry, std::string& path, std::string& title) {
  path.clear();
  title.clear();
  if (entry < 0 || entry >= bookRowCount()) return false;
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return false;
    path = books[static_cast<size_t>(entry)].path;
    title = books[static_cast<size_t>(entry)].title;
    return true;
  }
  if (!index.isOpen()) return false;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  std::string author;
  return ordinal != 0xFFFF && index.readRecord(ordinal, record) && index.readPath(record, path) &&
         rowTextFor(entry, title, author);
}

void LibraryListActivity::showBookOptions(const int entry) {
  std::string path;
  std::string title;
  if (!resolveEntry(entry, path, title)) {
    LOG_ERR("LIB", "cannot resolve row %d", entry);
    return;
  }
  // The Collections scope adds "remove from this collection" after "add".
  const bool inCollection = COLLECTIONS.valid(scopeCollection);
  const char* withRemove[] = {tr(STR_OPEN), tr(STR_COLLECTION_ADD), tr(STR_COLLECTION_REMOVE_BOOK), tr(STR_DELETE)};
  const char* plain[] = {tr(STR_OPEN), tr(STR_COLLECTION_ADD), tr(STR_DELETE)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), inCollection ? withRemove : plain, inCollection ? 4 : 3, 0,
                   [this, path, title, inCollection](int choice) {
                     swallowHeldReleases();
                     if (!inCollection && choice >= 2) choice++;  // align with the four-option layout
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         showAddToCollection(path);
                         break;
                       case 2:
                         COLLECTIONS.removeBook(scopeCollection, path);
                         resetAfterRebuild();  // re-filters the scope and clamps the selection
                         requestUpdate();
                         break;
                       case 3:
                         promptDeleteBookByPath(path, title);
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

void LibraryListActivity::showAddToCollection(const std::string& path) {
  // The popup holds at most OptionPopup::MAX_OPTIONS rows: the newest
  // collections past that are reached from the Collections tab.
  constexpr int MAX_LISTED = 15;
  const auto& collections = COLLECTIONS.all();
  const int listed = std::min(MAX_LISTED, static_cast<int>(collections.size()));
  std::vector<const char*> options;
  options.reserve(static_cast<size_t>(listed) + 1);
  for (int i = 0; i < listed; i++) options.push_back(collections[static_cast<size_t>(i)].name.c_str());
  options.push_back(tr(STR_COLLECTION_NEW));
  optionPopup.show(tr(STR_COLLECTION_ADD), options.data(), static_cast<int>(options.size()), 0,
                   [this, path, listed](const int choice) {
                     swallowHeldReleases();
                     if (choice == listed) {
                       promptNewCollection(path);
                       return;
                     }
                     COLLECTIONS.addBook(choice, path);
                     if (COLLECTIONS.valid(scopeCollection)) applyFilter();
                     requestUpdate();
                   });
  requestUpdate();
}

void LibraryListActivity::promptNewCollection(const std::string& addPath) {
  if (COLLECTIONS.count() >= CollectionsStore::MAX_COLLECTIONS) {
    GUI.drawPopup(renderer, tr(STR_COLLECTION_FULL));
    return;
  }
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_COLLECTION_NAME), "",
                                                           CollectionsStore::MAX_NAME, InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: collection keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this, addPath](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    const int created = COLLECTIONS.create(std::get<KeyboardResult>(result.data).text);
    if (created >= 0 && !addPath.empty()) COLLECTIONS.addBook(created, addPath);
    requestUpdate();
  });
}

void LibraryListActivity::showCollectionOptions(const int collection) {
  if (!COLLECTIONS.valid(collection)) return;
  const std::string name = COLLECTIONS.all()[static_cast<size_t>(collection)].name;
  const char* options[] = {tr(STR_OPEN), tr(STR_RENAME), tr(STR_COLLECTION_DELETE)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY_TAB_COLLECTIONS), name.c_str(), options, 3, 0, [this, collection, name](int choice) {
    swallowHeldReleases();
    if (choice == 0) {
      choosePicker(collection);
    } else if (choice == 1) {
      auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME), name,
                                                               CollectionsStore::MAX_NAME, InputType::Text);
      if (!keyboard) return;
      startActivityForResult(std::move(keyboard), [this, collection](const ActivityResult& result) {
        swallowHeldReleases();
        if (!result.isCancelled) COLLECTIONS.rename(collection, std::get<KeyboardResult>(result.data).text);
        requestUpdate();
      });
    } else if (choice == 2) {
      const char* confirm[] = {tr(STR_COLLECTION_DELETE), tr(STR_CANCEL)};
      optionPopup.showMessage(tr(STR_COLLECTION_DELETE), tr(STR_COLLECTION_DELETE_CONFIRM), confirm, 2, 1,
                              [this, collection](const int answer) {
                                swallowHeldReleases();
                                if (answer == 0) COLLECTIONS.remove(collection);
                                requestUpdate();
                              });
      requestUpdate();
    }
  });
  requestUpdate();
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.borderEdges = fui::EdgeBottom;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
  }
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().screen();
  // Header and tabs share a screen-relative boundary, independent of bezel insets.
  const fui::Rect band = BaseTheme::headerBand(renderer,
                                               fui::Rect{frameRect.x, static_cast<int16_t>(metrics.topPadding),
                                                         frameRect.width, static_cast<int16_t>(metrics.headerHeight)},
                                               header);
  fui::header(screen.frame(), band, header);
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  constexpr int READOUT_GAP = 4;  // the list's fold sits just above the readout's line box
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + READOUT_GAP);
  const Rect area = UITheme::getContentArea(renderer);
  const auto bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - area.y - area.height);
  buildHeader(screen);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                  static_cast<int16_t>(bottomInset + metrics.buttonHintsHeight + readoutReserved), 0});

  if (!degraded) buildTabBar(screen);
  if (pickerShown() ? pickerCount() == 0 : bookRowCount() == 0) {
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty() && scopeCollection >= 0) {
      message = tr(STR_COLLECTION_EMPTY);
    } else if (query.empty() && activeTabIndex == GENRE_TAB && pickerShown()) {
      message = tr(STR_LIBRARY_NO_GENRES);
    } else if (query.empty() && activeTabIndex == SERIES_TAB && pickerShown()) {
      message = tr(STR_LIBRARY_NO_SERIES);
    } else if (query.empty()) {
      message = tr(STR_LIBRARY_EMPTY);
    }
    screen.centeredText(message);
    return;
  }
  buildRows(screen);
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = listCount();
  if (count <= 0) return;

  char buf[32];
  const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  // Bottom-right of the viewable area, clear of the bezel and rounded corner.
  const Rect area = UITheme::getContentArea(renderer);
  const int right = std::max(renderer.getScreenWidth() - area.x - area.width, bottomCornerClearance());
  const int x = renderer.getScreenWidth() - right - width - SIDE_PADDING;
  const int y = area.y + area.height - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  if (degraded) return tr(STR_LIBRARY_TITLE_UNSORTED);
  return headerTabTitle.empty() ? tr(STR_LIBRARY) : headerTabTitle.c_str();
}

void LibraryListActivity::drawHoldHelp() const {
  if (mappedInput.hasTouch() || groupsCollapsed || pickerShown()) return;
  const char* help = nullptr;
  if (tabsFocused() && !degraded)
    help = tr(STR_LIBRARY_HOLD_SORT);
  else if (!tabsFocused() && isRecentSort(sortOrder) && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // recent rows: hold opens the row menu
  else if (!tabsFocused() && deleteEligible() && listCount() > 0)
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  else if (!tabsFocused() && groupable())
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const Rect area = UITheme::getContentArea(renderer);
  const int left = std::max(area.x, bottomCornerClearance());
  const int y = area.y + area.height - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer,
                   Rect{left + SIDE_PADDING, y, renderer.getScreenWidth() / 2 - left - SIDE_PADDING, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
}

void LibraryListActivity::drawFooter() {
  drawPositionReadout();
  drawHoldHelp();

  const bool backGoesHome = tabsFocused() && !groupsCollapsed && !filterActive();
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  const bool canSearch = tabsFocused() && !degraded;
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tr(STR_TOGGLE) : confirmLabel,
                                            canSearch ? tr(STR_SEARCH) : tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
